/* Tests unitaires des briques portables : CRC16, trames, chiffrement. */
#include <string.h>
#include "test.h"
#include "crc16.h"
#include "crypto.h"
#include "frame.h"

/* Trame de référence, aussi vérifiée par tests/test_outil_pc.py :
 * CMD_MSG, reserve 7, id 0x11223344, payload "Hi" */
static const uint8_t TRAME_REF[] = { 0x7E, 0x03, 0x02, 0x07, 0x44, 0x33, 0x22, 0x11,
                                     'H', 'i', 0x00, 0x00 /* CRC complété plus bas */ };

static frame_res_t pousser(frame_parser_t *p, const uint8_t *data, size_t len,
                           uint32_t now, ProtocolMessage_t *out)
{
    frame_res_t r = FRAME_EN_COURS;
    for (size_t i = 0; i < len && r == FRAME_EN_COURS; i++) {
        r = frame_parser_octet(p, data[i], now, out);
    }
    return r;
}

static void test_crc(void)
{
    printf("CRC16\n");
    /* valeur de contrôle officielle de CRC-16/CCITT-FALSE */
    VERIFIER(crc16_ccitt((const uint8_t *)"123456789", 9, CRC16_INIT) == 0x29B1);
    VERIFIER(crc16_ccitt(NULL, 0, CRC16_INIT) == 0xFFFF);
}

static void test_serialisation(void)
{
    printf("Serialisation\n");
    ProtocolMessage_t m;
    uint8_t buf[FRAME_MAX_SIZE];
    frame_preparer(&m, CMD_MSG, 0x11223344, 7, (const uint8_t *)"Hi", 2);
    size_t n = frame_serialiser(&m, buf);
    VERIFIER(n == 12);
    VERIFIER(memcmp(buf, TRAME_REF, 10) == 0);
    uint16_t crc = crc16_ccitt(TRAME_REF, 10, CRC16_INIT);
    VERIFIER(buf[10] == (crc & 0xFF) && buf[11] == (crc >> 8));
    printf("  trame de reference : CRC = 0x%04X\n", crc);
    VERIFIER(crc == 0xE5F8);   /* même valeur attendue dans test_outil_pc.py */
}

static void test_parseur(void)
{
    printf("Parseur\n");
    frame_parser_t p;
    ProtocolMessage_t m, r;
    uint8_t buf[FRAME_MAX_SIZE];
    size_t n;

    /* trame valide précédée de bruit */
    frame_parser_reset(&p);
    frame_preparer(&m, CMD_MSG, 0xCAFE, 1, (const uint8_t *)"Bonjour", 7);
    n = frame_serialiser(&m, buf);
    const uint8_t bruit[] = { 0x00, 0xFF, 0x55, 'a' };
    VERIFIER(pousser(&p, bruit, sizeof(bruit), 0, &r) == FRAME_EN_COURS);
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);
    VERIFIER(r.header.id_src == 0xCAFE && r.header.length == 7 && r.header.reserve == 1);
    VERIFIER(memcmp(r.payload, "Bonjour", 7) == 0);

    /* payload contenant 0x7E : pas de confusion avec un début de trame */
    const uint8_t avec_7e[] = { 0x7E, 0x7E, 0x01, 0x7E };
    frame_preparer(&m, CMD_MSG, 0x7E7E7E7E, 0x7E, avec_7e, sizeof(avec_7e));
    n = frame_serialiser(&m, buf);
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);
    VERIFIER(memcmp(r.payload, avec_7e, sizeof(avec_7e)) == 0);

    /* payload vide (CMD_RESET) et payload maximal */
    frame_preparer(&m, CMD_RESET, 1, 0, NULL, 0);
    n = frame_serialiser(&m, buf);
    VERIFIER(n == 10);
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);
    uint8_t seize[16];
    memcpy(seize, "0123456789ABCDEF", 16);
    frame_preparer(&m, CMD_MSG, 1, 0, seize, 16);
    n = frame_serialiser(&m, buf);
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);

    /* length = 17 : rejetée dès l'en-tête */
    const uint8_t trop_long[] = { 0x7E, CMD_MSG, 17 };
    VERIFIER(pousser(&p, trop_long, sizeof(trop_long), 0, &r) == FRAME_ERR_LONGUEUR);

    /* flag inconnu */
    const uint8_t flag_inconnu[] = { 0x7E, 0x42 };
    VERIFIER(pousser(&p, flag_inconnu, sizeof(flag_inconnu), 0, &r) == FRAME_ERR_FLAG);

    /* CRC faux (un bit du payload inversé) */
    frame_preparer(&m, CMD_MSG, 1, 0, (const uint8_t *)"abc", 3);
    n = frame_serialiser(&m, buf);
    buf[9] ^= 0x01;
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_ERR_CRC);

    /* après une erreur, le parseur repart et accepte la trame suivante */
    buf[9] ^= 0x01;
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);

    /* trame tronquée : abandon après le timeout, puis resynchronisation */
    VERIFIER(pousser(&p, buf, 5, 100, &r) == FRAME_EN_COURS);
    VERIFIER(frame_parser_timeout(&p, 100 + FRAME_TIMEOUT_MS) == FRAME_EN_COURS);
    VERIFIER(frame_parser_timeout(&p, 100 + FRAME_TIMEOUT_MS + 1) == FRAME_ERR_TIMEOUT);
    VERIFIER(pousser(&p, buf, n, 200, &r) == FRAME_OK);

    /* pas de timeout quand le parseur attend 0x7E */
    VERIFIER(frame_parser_timeout(&p, 100000) == FRAME_EN_COURS);

    /* 0x7E en double au début (octet parasite) : resynchronisation */
    frame_parser_reset(&p);
    VERIFIER(frame_parser_octet(&p, 0x7E, 0, &r) == FRAME_EN_COURS);
    VERIFIER(pousser(&p, buf, n, 0, &r) == FRAME_OK);
}

static void test_crypto(void)
{
    printf("Chiffrement\n");
    uint8_t msg[16] = "Message secret!";
    uint8_t c1[16], c2[16];

    memcpy(c1, msg, 16);
    crypto_appliquer(c1, 16, 0x12345678, 1);
    VERIFIER(memcmp(c1, msg, 16) != 0);
    memcpy(c2, c1, 16);
    crypto_appliquer(c2, 16, 0x12345678, 1);
    VERIFIER(memcmp(c2, msg, 16) == 0);         /* déchiffrer = rechiffrer */

    memcpy(c2, msg, 16);
    crypto_appliquer(c2, 16, 0x12345678, 2);
    VERIFIER(memcmp(c1, c2, 16) != 0);          /* autre nonce, autre flot */

    memcpy(c2, c1, 16);
    crypto_appliquer(c2, 16, 0x12345679, 1);
    VERIFIER(memcmp(c2, msg, 16) != 0);         /* mauvaise clé : illisible */

    VERIFIER(crypto_generer_cle(0) != 0);
    VERIFIER(crypto_generer_cle(1) != crypto_generer_cle(2));
}

int main(void)
{
    test_crc();
    test_serialisation();
    test_parseur();
    test_crypto();
    return BILAN();
}
