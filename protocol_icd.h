#ifndef PROTOCOL_ICD_H
#define PROTOCOL_ICD_H

#include <stdint.h>

#define ICD_START_BYTE 0x7E
#define MAX_PAYLOAD_SIZE 16
#define DEFAULT_ID 0X0

/* Sur le fil (taille variable) : 8 octets d'en-tête, puis "length" octets de
 * payload, puis le CRC (2 octets). Les champs multi-octets (id_src, crc) sont
 * en little-endian.
 * CRC-16/CCITT-FALSE (polynôme 0x1021, valeur initiale 0xFFFF), calculé sur
 * les 8 octets d'en-tête + les "length" octets de payload.
 * Une trame incomplète est abandonnée après 20 ms sans octet reçu. */

// Flags
typedef enum {
    CMD_INIT     = 0x00, // Initialisation
    CMD_RESET    = 0x01, // Réinitialisation
    CMD_ACK      = 0x02, // Bonne réception du message
    CMD_MSG      = 0x03, // Transmission d'un message texte
    CMD_ERROR    = 0x04, // Erreur
    CMD_GET_INFO = 0x05, // PC <-> nœud uniquement : identifiant, position, statut
    CMD_GET_KEY  = 0x06  // PC <-> nœud uniquement : clé utilisée
} CommandType_t;

/* Payloads :
 *  CMD_INIT  PC -> maître     : vide, ou [0] = nombre de nœuds (défaut 2)
 *            nœud -> suivant  : [0..3] clé | [4] nombre de nœuds |
 *                               [5] position de l'émetteur (le maître = 1)
 *  CMD_RESET : vide
 *  CMD_ACK   : vide
 *  CMD_MSG   : 1 à 16 octets, chiffrés entre les nœuds, en clair avec le PC.
 *              "reserve" = numéro du message (nonce du chiffrement).
 *  CMD_ERROR : [0] = code d'erreur (ErrorCode_t)
 *  CMD_GET_INFO réponse : [0] ID (8 bits) | [1] position | [2] statut
 *                         (0 inconnu, 1 maître, 2 esclave)
 *  CMD_GET_KEY  réponse : [0..3] clé (0 si non initialisé) */

// Codes d'erreur (payload de CMD_ERROR)
typedef enum {
    ERR_CRC      = 1, // CRC faux
    ERR_LONGUEUR = 2, // length > 16 ou incohérent avec le flag
    ERR_FLAG     = 3, // flag inconnu
    ERR_TIMEOUT  = 4, // trame incomplète
    ERR_ETAT     = 5, // commande refusée dans l'état courant (ex. MSG hors RUNNING)
    ERR_ROLE     = 6, // commande refusée pour ce rôle (ex. 2e maître)
    ERR_CONFIG   = 7  // configuration de CMD_INIT invalide
} ErrorCode_t;

// Header
typedef struct __attribute__((packed)) {
    uint8_t  start_byte;      // Octet de début de trame (0x7E)
    uint8_t  command_type;   // Type de commande issue de CommandType_t (1 octet)
    uint8_t  length;         // Taille utile du payload (1 octet)
    uint8_t reserve;        // Reserve d'alignement
    uint32_t id_src;         // Identifiant de la source (4 octets)
} MessageHeader_t;


// La trame complète circulant entre les nœuds
typedef struct __attribute__((packed)) {
    MessageHeader_t header;
    uint8_t         payload[MAX_PAYLOAD_SIZE]; // Les données (16 octets max)
    uint16_t        crc;
} ProtocolMessage_t;

#endif // PROTOCOL_ICD_H
