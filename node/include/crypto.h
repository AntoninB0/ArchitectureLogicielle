#ifndef CRYPTO_H
#define CRYPTO_H

/* Chiffrement symétrique par flot : payload XOR une suite pseudo-aléatoire
 * (xorshift32) initialisée avec la clé et le numéro du message (nonce).
 * Le nonce évite de réutiliser le même flot pour deux messages.
 * Chiffrer et déchiffrer sont la même opération. C pur, testé sur PC. */

#include <stdint.h>

void crypto_appliquer(uint8_t *data, uint8_t len, uint32_t cle, uint8_t nonce);

/* Mélange une graine en une clé non nulle */
uint32_t crypto_generer_cle(uint32_t graine);

#endif
