#ifndef NOEUD_H
#define NOEUD_H

/* Logique d'un nœud de la chaîne : machine à états, rôle maître/esclave,
 * initialisation, relais, chiffrement, reset, erreurs.
 * C pur, sans Zephyr : le matériel (UART, LED, aléa, logs) est fourni par
 * noeud_ops_t. Ainsi la même logique tourne sur la Blue Pill (main.c) et dans
 * la simulation d'une chaîne sur PC (tests/test_chaine.c).
 * Aucune fonction ne bloque : le temps est passé en paramètre (now_ms) et les
 * échéances sont vérifiées par noeud_tick(). */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "frame.h"

/* Lignes physiques. Les deux lignes de chaîne sont interchangeables : un
 * esclave prend pour amont celle où arrive CMD_INIT, l'autre est l'aval ; le
 * maître prend pour aval celle où revient le ACK. Le câblage entre cartes
 * est donc libre (USART2 ou USART3 de chaque côté). */
typedef enum {
    LIGNE_PC,      /* USART1 : PC (logs, PC IN pour le maître, PC OUT pour le dernier) */
    LIGNE_A,       /* USART2 */
    LIGNE_B,       /* USART3 */
    NB_LIGNES      /* aussi : ligne inconnue */
} ligne_t;

typedef enum { ETAT_INIT, ETAT_RUNNING, ETAT_ERROR } etat_t;
typedef enum { ROLE_INCONNU, ROLE_MAITRE, ROLE_ESCLAVE } role_t;

/* Motifs de la LED : INIT = 0,5 Hz, SYNCHRO = 2 Hz (maître en attente du
 * ACK), RUNNING = fixe, ERROR = 5 Hz, RESET = 3 flashs rapides (prioritaire
 * pendant NOEUD_DUREE_SIGNAL_RESET_MS, sinon la relance automatique du
 * maître le rendrait invisible) */
typedef enum { LED_INIT, LED_SYNCHRO, LED_RUNNING, LED_ERROR, LED_RESET } led_mode_t;

#define NOEUD_DUREE_ERREUR_MS  1000
#define NOEUD_TIMEOUT_ACK_MS   1000
#define NOEUD_DELAI_RELANCE_MS 500
#define NOEUD_DUREE_SIGNAL_RESET_MS 900
#define NOEUD_NB_DEFAUT        2

typedef struct {
    void (*envoyer)(void *ctx, ligne_t ligne, const uint8_t *data, size_t len);
    void (*led)(void *ctx, led_mode_t mode);
    uint32_t (*aleatoire)(void *ctx);
    void (*vlog)(void *ctx, const char *fmt, va_list ap);
    void *ctx;
} noeud_ops_t;

typedef struct {
    const noeud_ops_t *ops;
    uint32_t id;
    uint32_t now;
    role_t   role;
    etat_t   etat;            /* INIT ou RUNNING ; ERROR se superpose (erreur) */
    bool     erreur;
    uint32_t fin_erreur_ms;
    /* configuration de la chaîne */
    bool     configure;
    uint32_t cle;
    uint8_t  nb_noeuds;
    uint8_t  position;        /* 0 = non initialisé, le maître = 1 */
    bool     dernier;
    ligne_t  amont;           /* esclave : ligne du nœud précédent */
    ligne_t  aval;            /* ligne du nœud suivant (NB_LIGNES : inconnue) */
    uint8_t  compteur_msg;    /* nonce des CMD_MSG émis par le maître */
    /* échéances du maître */
    bool     attente_ack;
    uint32_t echeance_ack_ms;
    bool     relance;
    uint32_t echeance_relance_ms;
    bool     signal_reset;    /* LED_RESET en cours */
    uint32_t fin_signal_reset_ms;
    led_mode_t led;
    frame_parser_t parseurs[NB_LIGNES];
} noeud_t;

void noeud_demarrer(noeud_t *n, uint32_t id, const noeud_ops_t *ops, uint32_t now_ms);
void noeud_octet(noeud_t *n, ligne_t ligne, uint8_t octet, uint32_t now_ms);
void noeud_tick(noeud_t *n, uint32_t now_ms);
void noeud_appui_simple(noeud_t *n, uint32_t now_ms);
void noeud_triple_appui(noeud_t *n, uint32_t now_ms);

/* État visible : ERROR pendant 1 s après une erreur, sinon INIT ou RUNNING */
etat_t noeud_etat(const noeud_t *n);

#endif
