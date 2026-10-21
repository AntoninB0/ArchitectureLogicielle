#ifndef FRAME_H
#define FRAME_H

/* Trames de l'ICD : construction, sérialisation, réception octet par octet.
 * C pur, sans Zephyr : testé sur PC (tests/). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol_icd.h"

#define FRAME_HEADER_SIZE 8
#define FRAME_MAX_SIZE    (FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE + 2)
#define FRAME_TIMEOUT_MS  20   /* délai max entre deux octets d'une trame */

typedef enum {
    FRAME_EN_COURS,      /* octet consommé, trame pas encore complète */
    FRAME_OK,            /* trame complète et valide dans *out */
    FRAME_ERR_LONGUEUR,
    FRAME_ERR_FLAG,
    FRAME_ERR_CRC,
    FRAME_ERR_TIMEOUT
} frame_res_t;

typedef struct {
    uint8_t  buf[FRAME_MAX_SIZE];
    uint8_t  pos;        /* 0 = en attente de 0x7E */
    uint8_t  attendu;    /* taille totale de la trame en cours */
    uint32_t dernier_ms; /* heure du dernier octet reçu */
} frame_parser_t;

bool frame_flag_valide(uint8_t flag);

/* Remplit l'en-tête et le payload, puis calcule le CRC */
void frame_preparer(ProtocolMessage_t *m, uint8_t cmd, uint32_t id_src, uint8_t reserve,
                    const uint8_t *payload, uint8_t len);

uint16_t frame_crc(const ProtocolMessage_t *m);

/* Écrit la trame telle qu'elle passe sur le fil, renvoie sa taille */
size_t frame_serialiser(const ProtocolMessage_t *m, uint8_t *out);

void frame_parser_reset(frame_parser_t *p);

/* Donne un octet reçu au parseur. FRAME_OK : *out contient la trame. */
frame_res_t frame_parser_octet(frame_parser_t *p, uint8_t octet, uint32_t now_ms,
                               ProtocolMessage_t *out);

/* À appeler régulièrement : FRAME_ERR_TIMEOUT si la trame en cours est abandonnée */
frame_res_t frame_parser_timeout(frame_parser_t *p, uint32_t now_ms);

#endif
