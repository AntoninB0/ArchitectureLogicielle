/* Simulation sur PC d'une chaîne PC -> nœud 0 -> nœud 1 -> nœud 2 -> PC,
 * avec le même noeud.c que sur les cartes. Les lignes série sont remplacées
 * par une file d'octets ; le temps est simulé.
 * VERBOSE=1 ./test_chaine affiche les logs des nœuds. */
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "noeud.h"
#include "crypto.h"

#define N 3
#define MAX_TRAMES 64
#define MAX_FILE 8192

typedef struct {
    int idx;
    led_mode_t led;
} sim_ctx_t;

typedef struct {
    frame_parser_t parseur;
    ProtocolMessage_t trames[MAX_TRAMES];
    int nb;
} ecoute_t;

static noeud_t noeuds[N];
static sim_ctx_t ctx[N];
static noeud_ops_t ops[N];
static ecoute_t pc[N];         /* ce que chaque nœud envoie à son PC */
static ecoute_t espion_0_1;    /* ce qui passe sur le fil nœud 0 -> nœud 1 */
static bool lien_coupe;        /* fil 0 <-> 1 débranché */

/* Câblage : le lien k relie la ligne cote_gauche du nœud k à la ligne
 * cote_droit du nœud k+1. Par défaut USART2 -> USART3 ; les tests changent
 * ce tableau pour vérifier que le câblage est libre. */
static struct { ligne_t cote_gauche, cote_droit; } liens[N - 1];
static uint32_t now;
static bool verbose;

static struct { uint8_t dest, ligne, octet; } file_octets[MAX_FILE];
static int tete, queue;

static void ecouter(ecoute_t *e, const uint8_t *data, size_t len)
{
    ProtocolMessage_t m;
    for (size_t i = 0; i < len; i++) {
        if (frame_parser_octet(&e->parseur, data[i], now, &m) == FRAME_OK && e->nb < MAX_TRAMES) {
            e->trames[e->nb++] = m;
        }
    }
}

static void vider(ecoute_t *e)
{
    e->nb = 0;
}

static const ProtocolMessage_t *derniere(const ecoute_t *e)
{
    return e->nb ? &e->trames[e->nb - 1] : NULL;
}

static void transmettre(int dest, ligne_t ligne, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        file_octets[queue].dest = (uint8_t)dest;
        file_octets[queue].ligne = (uint8_t)ligne;
        file_octets[queue].octet = data[i];
        queue = (queue + 1) % MAX_FILE;
    }
}

static void sim_envoyer(void *c, ligne_t ligne, const uint8_t *data, size_t len)
{
    int i = ((sim_ctx_t *)c)->idx;
    if (ligne == LIGNE_PC) {
        ecouter(&pc[i], data, len);
    } else if (i + 1 < N && ligne == liens[i].cote_gauche) {
        if (i == 0) {
            if (lien_coupe) return;
            ecouter(&espion_0_1, data, len);
        }
        transmettre(i + 1, liens[i].cote_droit, data, len);
    } else if (i > 0 && ligne == liens[i - 1].cote_droit) {
        if (i == 1 && lien_coupe) return;
        transmettre(i - 1, liens[i - 1].cote_gauche, data, len);
    }
}

static void sim_led(void *c, led_mode_t mode)
{
    ((sim_ctx_t *)c)->led = mode;
}

static uint32_t sim_aleatoire(void *c)
{
    (void)c;
    static uint32_t x = 12345;
    return x = x * 1103515245u + 12345u + now;
}

static void sim_vlog(void *c, const char *fmt, va_list ap)
{
    if (verbose) {
        printf("    %5u ms [noeud %d] ", now, ((sim_ctx_t *)c)->idx);
        vprintf(fmt, ap);
    }
}

/* Achemine tous les octets en attente (les réponses peuvent en créer d'autres) */
static void pomper(void)
{
    while (tete != queue) {
        int d = file_octets[tete].dest;
        ligne_t l = (ligne_t)file_octets[tete].ligne;
        uint8_t o = file_octets[tete].octet;
        tete = (tete + 1) % MAX_FILE;
        noeud_octet(&noeuds[d], l, o, now);
    }
}

static void avancer(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        now += 10;
        for (int i = 0; i < N; i++) {
            noeud_tick(&noeuds[i], now);
        }
        pomper();
    }
}

static void pc_envoyer_brut(int i, const uint8_t *data, size_t len)
{
    transmettre(i, LIGNE_PC, data, len);
    pomper();
}

static void pc_envoyer(int i, uint8_t cmd, const void *payload, uint8_t len)
{
    ProtocolMessage_t m;
    uint8_t buf[FRAME_MAX_SIZE];
    frame_preparer(&m, cmd, DEFAULT_ID, 0, payload, len);
    pc_envoyer_brut(i, buf, frame_serialiser(&m, buf));
}

static bool tous(etat_t etat)
{
    for (int i = 0; i < N; i++) {
        if (noeud_etat(&noeuds[i]) != etat) return false;
    }
    return true;
}

static bool est_erreur(const ProtocolMessage_t *m, uint8_t code)
{
    return m && m->header.command_type == CMD_ERROR && m->header.length == 1 &&
           m->payload[0] == code;
}

static void demarrer(void)
{
    tete = queue = 0;
    lien_coupe = false;
    for (int k = 0; k < N - 1; k++) {
        liens[k].cote_gauche = LIGNE_A;
        liens[k].cote_droit = LIGNE_B;
    }
    memset(&espion_0_1, 0, sizeof(espion_0_1));
    for (int i = 0; i < N; i++) {
        memset(&pc[i], 0, sizeof(pc[i]));
        ctx[i].idx = i;
        ops[i] = (noeud_ops_t){ sim_envoyer, sim_led, sim_aleatoire, sim_vlog, &ctx[i] };
        noeud_demarrer(&noeuds[i], 0x1000 + i, &ops[i], now);
    }
}

static void initialiser_chaine(void)
{
    uint8_t nb = N;
    pc_envoyer(0, CMD_INIT, &nb, 1);
    avancer(20);
}

/* ------------------------------------------------------------ scénarios */

static void test_demarrage(void)
{
    printf("Demarrage\n");
    demarrer();
    VERIFIER(tous(ETAT_INIT));
    VERIFIER(ctx[0].led == LED_INIT && ctx[2].led == LED_INIT);
}

static void test_initialisation(void)
{
    printf("Initialisation de la chaine\n");
    demarrer();
    uint8_t nb = N;
    pc_envoyer(0, CMD_INIT, &nb, 1);
    /* tout se propage dans pomper() : la chaîne est déjà configurée */
    VERIFIER(tous(ETAT_RUNNING));
    VERIFIER(noeuds[0].role == ROLE_MAITRE && noeuds[0].position == 1);
    VERIFIER(noeuds[1].role == ROLE_ESCLAVE && noeuds[1].position == 2 && !noeuds[1].dernier);
    VERIFIER(noeuds[2].role == ROLE_ESCLAVE && noeuds[2].position == 3 && noeuds[2].dernier);
    VERIFIER(noeuds[1].cle == noeuds[0].cle && noeuds[2].cle == noeuds[0].cle);
    VERIFIER(noeuds[0].cle != 0);
    VERIFIER(derniere(&pc[0]) && derniere(&pc[0])->header.command_type == CMD_ACK);
    VERIFIER(ctx[0].led == LED_RUNNING && ctx[1].led == LED_RUNNING);
}

static void test_synchronisation_visible(void)
{
    printf("Maitre en synchronisation tant que le ACK n'est pas revenu\n");
    demarrer();
    lien_coupe = true;
    initialiser_chaine();
    VERIFIER(noeud_etat(&noeuds[0]) == ETAT_INIT && ctx[0].led == LED_SYNCHRO);
    uint32_t cle1 = noeuds[0].cle;
    avancer(NOEUD_TIMEOUT_ACK_MS);
    VERIFIER(noeuds[0].cle != cle1);          /* nouvelle tentative, nouvelle clé */
    lien_coupe = false;                       /* on rebranche */
    avancer(NOEUD_TIMEOUT_ACK_MS);
    VERIFIER(tous(ETAT_RUNNING));
}

static void test_message(void)
{
    printf("Message chiffre de bout en bout\n");
    demarrer();
    initialiser_chaine();
    vider(&pc[0]);
    vider(&espion_0_1);

    const char *texte = "Bonjour la carte";
    uint8_t len = (uint8_t)strlen(texte);
    pc_envoyer(0, CMD_MSG, texte, len);

    VERIFIER(derniere(&pc[0]) && derniere(&pc[0])->header.command_type == CMD_ACK);
    const ProtocolMessage_t *fil = derniere(&espion_0_1);
    VERIFIER(fil && fil->header.command_type == CMD_MSG && fil->header.length == len);
    VERIFIER(fil && memcmp(fil->payload, texte, len) != 0);   /* chiffré sur le fil */
    const ProtocolMessage_t *sortie = derniere(&pc[2]);
    VERIFIER(sortie && sortie->header.command_type == CMD_MSG);
    VERIFIER(sortie && sortie->header.length == len && memcmp(sortie->payload, texte, len) == 0);
    VERIFIER(pc[1].nb == 0);   /* l'intermédiaire ne livre rien à son PC */

    /* deux messages identiques ne donnent pas le même chiffré */
    uint8_t premier[MAX_PAYLOAD_SIZE];
    memcpy(premier, fil->payload, len);
    pc_envoyer(0, CMD_MSG, texte, len);
    VERIFIER(memcmp(derniere(&espion_0_1)->payload, premier, len) != 0);
    VERIFIER(memcmp(derniere(&pc[2])->payload, texte, len) == 0);
}

static void test_cablage_libre(void)
{
    printf("Cablage libre : USART2 <-> USART2 puis USART3 <-> USART2\n");
    demarrer();
    liens[0].cote_gauche = LIGNE_A;   /* nœud 0 USART2 <-> nœud 1 USART2 */
    liens[0].cote_droit = LIGNE_A;
    liens[1].cote_gauche = LIGNE_B;   /* nœud 1 USART3 <-> nœud 2 USART2 */
    liens[1].cote_droit = LIGNE_A;
    initialiser_chaine();
    VERIFIER(tous(ETAT_RUNNING));
    VERIFIER(noeuds[1].amont == LIGNE_A && noeuds[1].aval == LIGNE_B);
    VERIFIER(noeuds[2].amont == LIGNE_A && noeuds[2].dernier);
    VERIFIER(noeuds[0].aval == LIGNE_A);
    pc_envoyer(0, CMD_MSG, "cablage libre", 13);
    VERIFIER(derniere(&pc[2]) && memcmp(derniere(&pc[2])->payload, "cablage libre", 13) == 0);
    noeud_triple_appui(&noeuds[1], now);
    pomper();
    VERIFIER(tous(ETAT_INIT));
    avancer(NOEUD_DELAI_RELANCE_MS + 20);
    VERIFIER(tous(ETAT_RUNNING));
}

static void test_message_hors_running(void)
{
    printf("Aucun message hors RUNNING\n");
    demarrer();
    pc_envoyer(0, CMD_MSG, "test", 4);
    VERIFIER(est_erreur(derniere(&pc[0]), ERR_ETAT));
    VERIFIER(espion_0_1.nb == 0);
    VERIFIER(noeud_etat(&noeuds[0]) == ETAT_ERROR);
    avancer(NOEUD_DUREE_ERREUR_MS + 10);
    VERIFIER(noeud_etat(&noeuds[0]) == ETAT_INIT);   /* retour à l'état précédent */
}

static void test_trames_invalides(void)
{
    printf("Trames invalides : rejet, ERROR 1 s, pas de relais\n");
    demarrer();
    initialiser_chaine();
    vider(&pc[0]);
    vider(&pc[2]);

    /* trame corrompue arrivant sur l'entrée du nœud 1 */
    ProtocolMessage_t m;
    uint8_t buf[FRAME_MAX_SIZE];
    frame_preparer(&m, CMD_MSG, noeuds[0].id, 1, (const uint8_t *)"xyz", 3);
    size_t n = frame_serialiser(&m, buf);
    buf[9] ^= 0x10;
    transmettre(1, LIGNE_B, buf, n);
    pomper();
    VERIFIER(noeud_etat(&noeuds[1]) == ETAT_ERROR && ctx[1].led == LED_ERROR);
    VERIFIER(pc[2].nb == 0);                              /* pas relayée */
    VERIFIER(est_erreur(derniere(&pc[0]), ERR_CRC));      /* remontée jusqu'au PC IN */
    VERIFIER(noeud_etat(&noeuds[0]) == ETAT_RUNNING);     /* le maître n'est pas en erreur */
    avancer(NOEUD_DUREE_ERREUR_MS + 10);
    VERIFIER(noeud_etat(&noeuds[1]) == ETAT_RUNNING && ctx[1].led == LED_RUNNING);

    /* depuis le PC : length = 17, flag inconnu, trame tronquée */
    const uint8_t trop_long[] = { 0x7E, CMD_MSG, 17 };
    pc_envoyer_brut(0, trop_long, sizeof(trop_long));
    VERIFIER(est_erreur(derniere(&pc[0]), ERR_LONGUEUR));
    const uint8_t flag[] = { 0x7E, 0x09 };
    pc_envoyer_brut(0, flag, sizeof(flag));
    VERIFIER(est_erreur(derniere(&pc[0]), ERR_FLAG));
    frame_preparer(&m, CMD_MSG, DEFAULT_ID, 0, (const uint8_t *)"coupe", 5);
    n = frame_serialiser(&m, buf);
    pc_envoyer_brut(0, buf, n - 3);
    avancer(FRAME_TIMEOUT_MS + 20);
    VERIFIER(est_erreur(derniere(&pc[0]), ERR_TIMEOUT));

    /* la chaîne fonctionne encore après tout ça */
    avancer(NOEUD_DUREE_ERREUR_MS + 10);
    pc_envoyer(0, CMD_MSG, "encore ok", 9);
    VERIFIER(derniere(&pc[2]) && memcmp(derniere(&pc[2])->payload, "encore ok", 9) == 0);
}

static void test_un_seul_maitre(void)
{
    printf("Un seul maitre\n");
    demarrer();
    initialiser_chaine();
    pc_envoyer(1, CMD_INIT, NULL, 0);    /* un PC branché sur un esclave */
    VERIFIER(est_erreur(derniere(&pc[1]), ERR_ROLE));
    VERIFIER(noeuds[1].role == ROLE_ESCLAVE);
    pc_envoyer(2, CMD_MSG, "pirate", 6);  /* un esclave n'accepte pas de message du PC */
    VERIFIER(est_erreur(derniere(&pc[2]), ERR_ETAT));
}

static void test_reset(void)
{
    printf("Reset par triple appui sur un esclave, puis relance par le maitre\n");
    demarrer();
    initialiser_chaine();
    uint32_t ancienne_cle = noeuds[0].cle;

    noeud_triple_appui(&noeuds[2], now);
    pomper();
    VERIFIER(tous(ETAT_INIT));
    VERIFIER(noeuds[1].cle == 0 && noeuds[2].cle == 0 && noeuds[0].cle == 0);
    VERIFIER(noeuds[0].role == ROLE_MAITRE && noeuds[2].role == ROLE_INCONNU);
    VERIFIER(ctx[0].led == LED_RESET && ctx[2].led == LED_RESET);   /* reset visible */

    avancer(NOEUD_DELAI_RELANCE_MS + 20);
    VERIFIER(tous(ETAT_RUNNING));
    VERIFIER(noeuds[2].cle == noeuds[0].cle && noeuds[0].cle != ancienne_cle);
    VERIFIER(ctx[0].led == LED_RESET);        /* signal encore affiché après la relance */
    avancer(NOEUD_DUREE_SIGNAL_RESET_MS);
    VERIFIER(ctx[0].led == LED_RUNNING && ctx[2].led == LED_RUNNING);

    printf("Reset par commande du PC\n");
    vider(&pc[0]);
    pc_envoyer(0, CMD_RESET, NULL, 0);
    VERIFIER(tous(ETAT_INIT));
    VERIFIER(derniere(&pc[0]) && derniere(&pc[0])->header.command_type == CMD_ACK);
    avancer(NOEUD_DELAI_RELANCE_MS + 20);
    VERIFIER(tous(ETAT_RUNNING));

    printf("Reset pendant un ERROR\n");
    const uint8_t flag[] = { 0x7E, 0x09 };
    pc_envoyer_brut(0, flag, sizeof(flag));
    VERIFIER(noeud_etat(&noeuds[0]) == ETAT_ERROR);
    noeud_triple_appui(&noeuds[0], now);
    pomper();
    VERIFIER(tous(ETAT_INIT));
    avancer(NOEUD_DELAI_RELANCE_MS + 20);
    VERIFIER(tous(ETAT_RUNNING));
}

static void test_get_info_get_key(void)
{
    printf("GET_INFO et GET_KEY\n");
    demarrer();
    pc_envoyer(1, CMD_GET_INFO, NULL, 0);
    const ProtocolMessage_t *r = derniere(&pc[1]);
    VERIFIER(r && r->header.command_type == CMD_GET_INFO && r->payload[1] == 0 && r->payload[2] == 0);

    initialiser_chaine();
    pc_envoyer(1, CMD_GET_INFO, NULL, 0);
    r = derniere(&pc[1]);
    VERIFIER(r && r->payload[0] == (uint8_t)noeuds[1].id && r->payload[1] == 2 &&
             r->payload[2] == ROLE_ESCLAVE);
    pc_envoyer(0, CMD_GET_KEY, NULL, 0);
    r = derniere(&pc[0]);
    uint32_t cle = r ? (uint32_t)r->payload[0] | ((uint32_t)r->payload[1] << 8) |
                       ((uint32_t)r->payload[2] << 16) | ((uint32_t)r->payload[3] << 24) : 0;
    VERIFIER(r && r->header.command_type == CMD_GET_KEY && cle == noeuds[0].cle);
}

int main(void)
{
    verbose = getenv("VERBOSE") != NULL;
    test_demarrage();
    test_initialisation();
    test_synchronisation_visible();
    test_message();
    test_cablage_libre();
    test_message_hors_running();
    test_trames_invalides();
    test_un_seul_maitre();
    test_reset();
    test_get_info_get_key();
    return BILAN();
}
