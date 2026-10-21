#include <string.h>
#include "noeud.h"
#include "crypto.h"

static const char *const NOMS_ETATS[] = { "INIT", "RUNNING", "ERROR" };
static const char *const NOMS_ROLES[] = { "inconnu", "maitre", "esclave" };
static const char *const NOMS_LIGNES[] = { "PC", "amont", "aval" };

/* "now" est-il après "echeance" ? (correct même quand le compteur déborde) */
static bool atteint(uint32_t now, uint32_t echeance)
{
    return (int32_t)(now - echeance) >= 0;
}

static void journal(noeud_t *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    n->ops->vlog(n->ops->ctx, fmt, ap);
    va_end(ap);
}

/* Les logs partagent l'USART1 avec les trames envoyées au PC : ils ne doivent
 * jamais contenir 0x7E ('~'), ni de caractère non imprimable. */
static void texte_affichable(const uint8_t *data, uint8_t len, char *out)
{
    for (uint8_t i = 0; i < len; i++) {
        out[i] = (data[i] >= 0x20 && data[i] < 0x7E) ? (char)data[i] : '.';
    }
    out[len] = '\0';
}

etat_t noeud_etat(const noeud_t *n)
{
    return n->erreur ? ETAT_ERROR : n->etat;
}

static void maj_led(noeud_t *n)
{
    led_mode_t mode;
    if (n->erreur) {
        mode = LED_ERROR;
    } else if (n->etat == ETAT_RUNNING) {
        mode = LED_RUNNING;
    } else if (n->attente_ack) {
        mode = LED_SYNCHRO;
    } else {
        mode = LED_INIT;
    }
    if (mode != n->led) {
        n->led = mode;
        n->ops->led(n->ops->ctx, mode);
    }
}

static void passer_etat(noeud_t *n, etat_t etat)
{
    if (n->etat != etat) {
        n->etat = etat;
        journal(n, "Etat %s\n", NOMS_ETATS[etat]);
    }
    maj_led(n);
}

static void envoyer_trame(noeud_t *n, ligne_t ligne, const ProtocolMessage_t *m)
{
    uint8_t buf[FRAME_MAX_SIZE];
    size_t len = frame_serialiser(m, buf);
    n->ops->envoyer(n->ops->ctx, ligne, buf, len);
}

static void envoyer(noeud_t *n, ligne_t ligne, uint8_t cmd, uint8_t reserve,
                    const uint8_t *payload, uint8_t len)
{
    ProtocolMessage_t m;
    frame_preparer(&m, cmd, n->id, reserve, payload, len);
    envoyer_trame(n, ligne, &m);
}

/* Erreur de traitement : CMD_ERROR renvoyé à l'émetteur, puis ERROR pendant 1 s.
 * Entre nœuds, pas de nouveau CMD_ERROR tant qu'on est déjà en ERROR : deux
 * nœuds ne peuvent pas s'envoyer des erreurs en boucle. */
static void signaler_erreur(noeud_t *n, ligne_t ligne, uint8_t code)
{
    journal(n, "Erreur %u sur la ligne %s\n", code, NOMS_LIGNES[ligne]);
    if (ligne == LIGNE_PC || !n->erreur) {
        envoyer(n, ligne, CMD_ERROR, 0, &code, 1);
    }
    if (!n->erreur) {
        n->erreur = true;
        n->fin_erreur_ms = n->now + NOEUD_DUREE_ERREUR_MS;
        journal(n, "Etat ERROR\n");
        maj_led(n);
    }
}

static void effacer_configuration(noeud_t *n)
{
    n->configure = false;
    n->cle = 0;
    n->position = 0;
    n->dernier = false;
    n->compteur_msg = 0;
    n->attente_ack = false;
    n->relance = false;
}

/* ---------------------------------------------------------------- maître */

static void init_maitre(noeud_t *n)
{
    n->cle = crypto_generer_cle(n->ops->aleatoire(n->ops->ctx));
    n->position = 1;
    n->compteur_msg = 0;
    n->configure = false;
    journal(n, "Init maitre : %u noeuds, cle 0x%08x\n", n->nb_noeuds, (unsigned)n->cle);

    if (n->nb_noeuds == 1) {
        /* chaîne d'un seul nœud : le maître est aussi le dernier */
        n->dernier = true;
        n->configure = true;
        passer_etat(n, ETAT_RUNNING);
        envoyer(n, LIGNE_PC, CMD_ACK, 0, NULL, 0);
        return;
    }

    uint8_t p[6] = { (uint8_t)n->cle, (uint8_t)(n->cle >> 8), (uint8_t)(n->cle >> 16),
                     (uint8_t)(n->cle >> 24), n->nb_noeuds, n->position };
    envoyer(n, LIGNE_AVAL, CMD_INIT, 0, p, sizeof(p));
    n->attente_ack = true;
    n->echeance_ack_ms = n->now + NOEUD_TIMEOUT_ACK_MS;
    passer_etat(n, ETAT_INIT);
}

/* Réinitialisation : efface clé et configuration, retour en INIT.
 * origine = ligne d'où vient le CMD_RESET, ou NB_LIGNES s'il est local
 * (triple appui, commande du PC) : il part alors dans les deux sens. */
static void reinitialiser(noeud_t *n, ligne_t origine, const ProtocolMessage_t *m)
{
    journal(n, "Reinitialisation (%s)\n",
            origine == NB_LIGNES ? "locale" : NOMS_LIGNES[origine]);
    effacer_configuration(n);
    n->erreur = false;

    if (origine == LIGNE_AMONT) {
        envoyer_trame(n, LIGNE_AVAL, m);
    } else if (origine == LIGNE_AVAL) {
        envoyer_trame(n, LIGNE_AMONT, m);
    } else {
        envoyer(n, LIGNE_AMONT, CMD_RESET, 0, NULL, 0);
        envoyer(n, LIGNE_AVAL, CMD_RESET, 0, NULL, 0);
    }

    if (n->role == ROLE_MAITRE) {
        /* le maître garde son rôle et relance l'initialisation */
        n->relance = true;
        n->echeance_relance_ms = n->now + NOEUD_DELAI_RELANCE_MS;
    } else {
        n->role = ROLE_INCONNU;
    }
    passer_etat(n, ETAT_INIT);
}

static void livrer_au_pc(noeud_t *n, const uint8_t *texte, uint8_t len)
{
    char affiche[MAX_PAYLOAD_SIZE + 1];
    texte_affichable(texte, len, affiche);
    journal(n, "PC OUT : \"%s\"\n", affiche);
    envoyer(n, LIGNE_PC, CMD_MSG, 0, texte, len);
}

/* --------------------------------------------- trames reçues par ligne */

static void depuis_pc(noeud_t *n, const ProtocolMessage_t *m)
{
    const uint8_t len = m->header.length;

    switch (m->header.command_type) {
    case CMD_INIT:
        /* le nœud qui reçoit DEFAULT_ID du PC devient maître (un seul maître) */
        if (m->header.id_src != DEFAULT_ID || n->role == ROLE_ESCLAVE) {
            signaler_erreur(n, LIGNE_PC, ERR_ROLE);
            return;
        }
        if (len > 1) {
            signaler_erreur(n, LIGNE_PC, ERR_LONGUEUR);
            return;
        }
        if (len == 1 && m->payload[0] == 0) {
            signaler_erreur(n, LIGNE_PC, ERR_CONFIG);
            return;
        }
        n->role = ROLE_MAITRE;
        n->nb_noeuds = len ? m->payload[0] : NOEUD_NB_DEFAUT;
        n->relance = false;
        init_maitre(n);
        break;

    case CMD_RESET:
        if (len != 0) {
            signaler_erreur(n, LIGNE_PC, ERR_LONGUEUR);
            return;
        }
        reinitialiser(n, NB_LIGNES, m);
        envoyer(n, LIGNE_PC, CMD_ACK, 0, NULL, 0);
        break;

    case CMD_MSG: {
        if (len == 0) {
            signaler_erreur(n, LIGNE_PC, ERR_LONGUEUR);
            return;
        }
        if (n->role != ROLE_MAITRE || noeud_etat(n) != ETAT_RUNNING) {
            signaler_erreur(n, LIGNE_PC, ERR_ETAT);
            return;
        }
        char affiche[MAX_PAYLOAD_SIZE + 1];
        texte_affichable(m->payload, len, affiche);
        journal(n, "PC IN : \"%s\"\n", affiche);
        envoyer(n, LIGNE_PC, CMD_ACK, 0, NULL, 0);
        if (n->dernier) {
            livrer_au_pc(n, m->payload, len);
            return;
        }
        uint8_t chiffre[MAX_PAYLOAD_SIZE];
        memcpy(chiffre, m->payload, len);
        n->compteur_msg++;
        crypto_appliquer(chiffre, len, n->cle, n->compteur_msg);
        envoyer(n, LIGNE_AVAL, CMD_MSG, n->compteur_msg, chiffre, len);
        break;
    }

    case CMD_GET_INFO: {
        uint8_t p[3] = { (uint8_t)n->id, n->position, (uint8_t)n->role };
        envoyer(n, LIGNE_PC, CMD_GET_INFO, 0, p, sizeof(p));
        break;
    }

    case CMD_GET_KEY: {
        uint8_t p[4] = { (uint8_t)n->cle, (uint8_t)(n->cle >> 8),
                         (uint8_t)(n->cle >> 16), (uint8_t)(n->cle >> 24) };
        envoyer(n, LIGNE_PC, CMD_GET_KEY, 0, p, sizeof(p));
        break;
    }

    default:
        journal(n, "Commande %u du PC ignoree\n", m->header.command_type);
        break;
    }
}

static void depuis_amont(noeud_t *n, const ProtocolMessage_t *m)
{
    const uint8_t len = m->header.length;

    switch (m->header.command_type) {
    case CMD_INIT: {
        if (len != 6) {
            signaler_erreur(n, LIGNE_AMONT, ERR_LONGUEUR);
            return;
        }
        if (n->role == ROLE_MAITRE) {
            signaler_erreur(n, LIGNE_AMONT, ERR_ROLE);
            return;
        }
        uint8_t nb = m->payload[4];
        uint8_t position = m->payload[5] + 1;
        if (nb < 2 || m->payload[5] == 0 || position > nb) {
            signaler_erreur(n, LIGNE_AMONT, ERR_CONFIG);
            return;
        }
        n->role = ROLE_ESCLAVE;
        n->cle = (uint32_t)m->payload[0] | ((uint32_t)m->payload[1] << 8) |
                 ((uint32_t)m->payload[2] << 16) | ((uint32_t)m->payload[3] << 24);
        n->nb_noeuds = nb;
        n->position = position;
        n->dernier = (position == nb);
        n->configure = true;
        journal(n, "Init esclave : position %u/%u%s, cle 0x%08x\n", position, nb,
                n->dernier ? " (dernier)" : "", (unsigned)n->cle);
        passer_etat(n, ETAT_RUNNING);

        if (n->dernier) {
            envoyer(n, LIGNE_AMONT, CMD_ACK, 0, NULL, 0);
        } else {
            /* seule exception au relais sans modification : la position
             * avance d'un cran, le CRC est donc recalculé */
            ProtocolMessage_t suite;
            uint8_t p[6];
            memcpy(p, m->payload, sizeof(p));
            p[5] = position;
            frame_preparer(&suite, CMD_INIT, m->header.id_src, m->header.reserve, p, sizeof(p));
            envoyer_trame(n, LIGNE_AVAL, &suite);
        }
        break;
    }

    case CMD_RESET:
        reinitialiser(n, LIGNE_AMONT, m);
        break;

    case CMD_MSG:
        if (len == 0) {
            signaler_erreur(n, LIGNE_AMONT, ERR_LONGUEUR);
            return;
        }
        if (!n->configure || noeud_etat(n) != ETAT_RUNNING) {
            signaler_erreur(n, LIGNE_AMONT, ERR_ETAT);
            return;
        }
        if (n->dernier) {
            uint8_t clair[MAX_PAYLOAD_SIZE];
            memcpy(clair, m->payload, len);
            crypto_appliquer(clair, len, n->cle, m->header.reserve);
            livrer_au_pc(n, clair, len);
        } else {
            envoyer_trame(n, LIGNE_AVAL, m);
        }
        break;

    case CMD_ERROR:
        journal(n, "Erreur %u signalee par le noeud amont\n", len ? m->payload[0] : 0);
        break;

    default:
        journal(n, "Commande %u de l'amont ignoree\n", m->header.command_type);
        break;
    }
}

static void depuis_aval(noeud_t *n, const ProtocolMessage_t *m)
{
    switch (m->header.command_type) {
    case CMD_ACK:
        if (n->role == ROLE_ESCLAVE) {
            envoyer_trame(n, LIGNE_AMONT, m);   /* remonte vers le maître */
        } else if (n->role == ROLE_MAITRE && n->attente_ack) {
            n->attente_ack = false;
            n->configure = true;
            journal(n, "ACK du dernier : chaine configuree\n");
            passer_etat(n, ETAT_RUNNING);
            envoyer(n, LIGNE_PC, CMD_ACK, 0, NULL, 0);
        }
        break;

    case CMD_RESET:
        reinitialiser(n, LIGNE_AVAL, m);
        break;

    case CMD_ERROR:
        journal(n, "Erreur %u signalee par le noeud aval\n",
                m->header.length ? m->payload[0] : 0);
        /* remonte jusqu'au maître, qui la transmet au PC */
        envoyer_trame(n, n->role == ROLE_MAITRE ? LIGNE_PC : LIGNE_AMONT, m);
        break;

    default:
        journal(n, "Commande %u de l'aval ignoree\n", m->header.command_type);
        break;
    }
}

/* ------------------------------------------------------------ interface */

void noeud_demarrer(noeud_t *n, uint32_t id, const noeud_ops_t *ops, uint32_t now_ms)
{
    memset(n, 0, sizeof(*n));
    n->ops = ops;
    n->id = id;
    n->now = now_ms;
    n->role = ROLE_INCONNU;
    n->etat = ETAT_INIT;
    n->led = LED_INIT;
    ops->led(ops->ctx, LED_INIT);
    journal(n, "Noeud 0x%08x demarre, etat INIT\n", (unsigned)id);
}

static void verifier_timeout(noeud_t *n, ligne_t ligne)
{
    if (frame_parser_timeout(&n->parseurs[ligne], n->now) == FRAME_ERR_TIMEOUT) {
        signaler_erreur(n, ligne, ERR_TIMEOUT);
    }
}

void noeud_octet(noeud_t *n, ligne_t ligne, uint8_t octet, uint32_t now_ms)
{
    ProtocolMessage_t m;

    n->now = now_ms;
    verifier_timeout(n, ligne);

    switch (frame_parser_octet(&n->parseurs[ligne], octet, now_ms, &m)) {
    case FRAME_EN_COURS:
        break;
    case FRAME_OK:
        if (ligne == LIGNE_PC) {
            depuis_pc(n, &m);
        } else if (ligne == LIGNE_AMONT) {
            depuis_amont(n, &m);
        } else {
            depuis_aval(n, &m);
        }
        break;
    case FRAME_ERR_LONGUEUR:
        signaler_erreur(n, ligne, ERR_LONGUEUR);
        break;
    case FRAME_ERR_FLAG:
        signaler_erreur(n, ligne, ERR_FLAG);
        break;
    case FRAME_ERR_CRC:
        signaler_erreur(n, ligne, ERR_CRC);
        break;
    case FRAME_ERR_TIMEOUT:
        signaler_erreur(n, ligne, ERR_TIMEOUT);
        break;
    }
}

void noeud_tick(noeud_t *n, uint32_t now_ms)
{
    n->now = now_ms;

    for (int l = 0; l < NB_LIGNES; l++) {
        verifier_timeout(n, (ligne_t)l);
    }
    if (n->erreur && atteint(now_ms, n->fin_erreur_ms)) {
        n->erreur = false;
        journal(n, "Fin ERROR, retour en %s\n", NOMS_ETATS[n->etat]);
        maj_led(n);
    }
    if (n->role == ROLE_MAITRE && n->attente_ack && atteint(now_ms, n->echeance_ack_ms)) {
        journal(n, "Pas de ACK du dernier noeud, nouvelle tentative\n");
        init_maitre(n);
    }
    if (n->role == ROLE_MAITRE && n->relance && atteint(now_ms, n->echeance_relance_ms)) {
        n->relance = false;
        init_maitre(n);
    }
}

void noeud_appui_simple(noeud_t *n, uint32_t now_ms)
{
    n->now = now_ms;
    journal(n, "Info : id 0x%08x, %s, etat %s, position %u/%u, cle 0x%08x\n",
            (unsigned)n->id, NOMS_ROLES[n->role], NOMS_ETATS[noeud_etat(n)],
            n->position, n->nb_noeuds, (unsigned)n->cle);
}

void noeud_triple_appui(noeud_t *n, uint32_t now_ms)
{
    n->now = now_ms;
    journal(n, "Triple appui\n");
    reinitialiser(n, NB_LIGNES, NULL);
}
