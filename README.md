# Chaîne d'information sécurisée : CY-Tech

Binôme : Antonin Bo et Shawrov. Matériel : 2 × STM32F103C8T6 (« Blue Pill »), Zephyr (NCS v3.4.1).

Des messages passent de proche en proche : `PC IN → nœud 1 (maître) → nœud 2 → … → nœud N → PC OUT`.
Ils sont en clair à l'entrée et à la sortie, chiffrés entre les nœuds et protégés par un CRC16.
Toutes les cartes ont le même firmware : chacune trouve son rôle à l'exécution.

## Arborescence

| Chemin | Contenu |
|---|---|
| `protocol_icd.h` | ICD : format de trame, flags, payloads, codes d'erreur |
| `node/` | firmware du nœud (application Zephyr) |
| `node/src/crc16.c`, `frame.c`, `crypto.c` | briques en C pur (testées sur PC) |
| `node/src/noeud.c` | logique du nœud en C pur : machine à états, rôles, init, relais, reset, erreurs |
| `node/src/main.c` | partie Zephyr : UART par interruption, LED, bouton, UID, aléa |
| `tests/` | tests unitaires et simulation d'une chaîne de 3 nœuds sur PC (`make`) |
| `tools/chaine.py` | outil PC : PC IN (commandes) et PC OUT / moniteur (logs et trames décodées) |
| `build.sh` | compilation, flash par le bootloader UART, moniteur |

## Architecture matérielle

| Fonction | Périph. | Broches | Branché sur |
|---|---|---|---|
| Ligne PC (logs, PC IN pour le maître, PC OUT pour le dernier, flash) | USART1 | PA9 TX / PA10 RX | adaptateur USB-série |
| Ligne de chaîne A | USART2 | PA2 TX / PA3 RX | nœud précédent ou suivant |
| Ligne de chaîne B | USART3 | PB10 TX / PB11 RX | nœud précédent ou suivant |
| LED d'état | GPIO | PC13 | LED de la carte |
| Bouton USER | EXTI | PB0, pull-up | bouton vers GND |

**Le câblage entre cartes est libre** : on relie une ligne de chaîne (USART2 ou USART3) d'un nœud à une ligne de chaîne du suivant, en croisant TX et RX, à 115200 bauds 8N1. Le sens est découvert à l'initialisation. Le maître envoie `CMD_INIT` sur ses deux lignes. Un esclave prend pour amont la ligne où `CMD_INIT` arrive et relaie sur l'autre. Le maître prend pour aval la ligne où revient le ACK. Exemple utilisé pour la démo :

```
nœud 1  PA2 (TX2) ───────► PA3 (RX2)  nœud 2     CMD_INIT, CMD_MSG, CMD_RESET
nœud 1  PA3 (RX2) ◄─────── PA2 (TX2)  nœud 2     CMD_ACK, CMD_ERROR, CMD_RESET
        GND ────────────── GND                   masses communes
```

Avec N > 2, un nœud intermédiaire utilise ses deux lignes, une vers chaque voisin.

Sur l'USART1, les logs texte et les trames du PC partagent la ligne : les logs ne contiennent jamais `0x7E` (`~`), et `tools/chaine.py` sépare les deux.
La RX des USART2/3 est en pull-up (`app.overlay`), pour qu'une ligne non câblée, comme l'amont du maître ou l'aval du dernier, ne capte pas de bruit.

> Ce choix diffère du plan initial (entrée = USART1). Sur Zephyr, la console est sur l'USART1, et c'est aussi par là qu'on flashe. Chaque carte a donc son adaptateur sur l'USART1 : celui du maître sert de PC IN, celui du dernier de PC OUT.

## Architecture logicielle

Le firmware tourne sur Zephyr RTOS avec **un seul thread de traitement** (`main`). Les interruptions (octets UART, bouton, timers) déposent des événements dans une `k_msgq`. Le thread les traite un par un et appelle `noeud_tick()` au moins toutes les 10 ms pour les échéances. Rien ne bloque, et l'état du nœud n'est jamais modifié en interruption.

`noeud.c` ne dépend pas de Zephyr : le matériel lui est passé sous forme de pointeurs de fonctions (`noeud_ops_t` : envoyer, LED, aléa, log) et le temps en paramètre. Le **même code** tourne sur la carte et dans `tests/test_chaine.c`, qui simule une chaîne de 3 nœuds.

### Machine à états et LED

| État | LED | Entrée | Sortie |
|---|---|---|---|
| INIT | 0,5 Hz | démarrage, reset | configuration et clé appliquées → RUNNING |
| INIT (synchronisation, maître) | 2 Hz | CMD_INIT émis | ACK du dernier → RUNNING ; sinon nouvelle tentative au bout de 1 s |
| RUNNING | fixe | | erreur → ERROR |
| ERROR | 5 Hz | trame invalide ou commande refusée | après 1 s, **retour à l'état précédent** |

Cela donne les trois indications du sujet : non initialisé (0,5 Hz), en synchronisation (2 Hz), synchronisé (fixe).

### Protocole (détail dans `protocol_icd.h`)

* Sur le fil : `0x7E | cmd | length | reserve | id_src (4, LE) | payload (length) | CRC (2, LE)`, taille variable.
* CRC-16/CCITT-FALSE (0x1021, init 0xFFFF) sur l'en-tête et le payload.
* Réception octet par octet : les octets sont ignorés jusqu'à un `0x7E`. Une trame est rejetée si `length > 16` ou si le flag est inconnu. Une trame incomplète est abandonnée après 20 ms sans octet.

**Initialisation.** Le PC envoie `CMD_INIT` (id_src = `DEFAULT_ID`, payload = nombre de nœuds) au nœud qui sera maître. Le maître génère une clé et émet `CMD_INIT [clé | nb | position=1]`. Chaque esclave enregistre la clé et prend la position reçue + 1. Il est le dernier si sa position est égale à `nb` ; sinon il relaie la trame avec la position incrémentée. Le dernier renvoie `CMD_ACK`, qui remonte la chaîne. À sa réception, le maître passe en RUNNING et répond `CMD_ACK` au PC.

**Message.** Le PC envoie `CMD_MSG` en clair au maître. Le maître chiffre le payload (flot xorshift32 initialisé avec la clé et le numéro du message, transmis dans `reserve`) et l'émet. Les intermédiaires relaient sans rien modifier. Le dernier déchiffre et envoie le message en clair à son PC (`CMD_MSG`). Aucun `CMD_MSG` n'est émis hors de l'état RUNNING.

**Reset.** Un triple appui en moins de 1 s, ou `CMD_RESET` reçu du PC, émet `CMD_RESET` vers l'amont **et** vers l'aval. Un `CMD_RESET` reçu est relayé dans son sens de propagation. Chaque nœud efface sa clé et sa configuration et revient en INIT. Le maître garde son rôle et relance l'initialisation 0,5 s plus tard, avec une nouvelle clé.

**Erreurs.** Un nœud qui reçoit une trame invalide renvoie `CMD_ERROR [code]` à l'émetteur et passe en ERROR pendant 1 s. Les erreurs remontent jusqu'au maître, qui les transmet au PC. Tant qu'un nœud est déjà en ERROR, il n'envoie pas de nouveau `CMD_ERROR` vers un autre nœud, pour éviter les boucles.

**Commandes PC → nœud** (extension de l'ICD) : `CMD_GET_INFO` → `[ID 8 bits | position | statut]`, `CMD_GET_KEY` → `[clé]`.

## Précisions apportées à la spec du 08/10 (spec v2)

| # | Point de la spec | Décision |
|---|---|---|
| P1 | « revenir dans l'état ATTENTE » | INIT |
| P2 | retour depuis ERROR | vers l'état précédent (INIT ou RUNNING) |
| P3 | comment l'esclave sait qu'il est le dernier | `CMD_INIT` contient le nombre de nœuds et une position que chaque esclave incrémente. C'est la seule trame modifiée au relais, avec un nouveau CRC |
| P4 | « le nœud recevant 0x0 » | le nœud qui reçoit du PC un `CMD_INIT` avec `id_src = DEFAULT_ID` devient maître, sauf s'il est déjà esclave (refus `ERR_ROLE`). Un maître qui reçoit `CMD_INIT` de l'amont le refuse aussi |
| P5 | retour du ACK | relayé vers l'amont par chaque esclave |
| P6 | CRC | CRC-16/CCITT-FALSE sur l'en-tête et le payload |
| P7 | format sur le fil, timeout | taille variable, timeout inter-octets de 20 ms |
| P8 | chiffrement | flot XOR xorshift32 (clé + nonce) |
| P9 | codes d'erreur | 1 CRC, 2 longueur, 3 flag, 4 timeout, 5 état, 6 rôle, 7 configuration |
| P10 | aléa sans RNG matériel | heure en cycles CPU (72 MHz) de chaque événement (octets reçus, appuis), mélangée à l'UID |
| S1 | GET_INFO / GET_KEY / UPDT_KEY | GET_INFO et GET_KEY ajoutés (flags 0x05 et 0x06). UPDT_KEY non implémenté : la clé change à chaque init ou reset |
| S3 | 3 indications visuelles | ajout du clignotement à 2 Hz « en synchronisation » |

### Limites connues

* Un nœud intermédiaire ne doit pas avoir ses deux lignes reliées au même voisin.
* **Interchangement des modules (S2)** : rien ne détecte l'échange de deux modules déjà initialisés. Une permutation est détectée seulement si elle fausse la position (`ERR_CONFIG`) ou si elle place un esclave en entrée (`ERR_ROLE`). Pour aller plus loin, il faudrait une clé par lien, qui lie chaque nœud à son voisin.
* **La clé circule en clair dans `CMD_INIT` (S4)**. Il n'existe pas de secret partagé avant l'initialisation.
* Le chiffrement est un exercice : flot XOR simple, sans authentification cryptographique. Le CRC protège l'intégrité contre les erreurs de transmission, pas contre un attaquant.
* Le rôle de maître est conservé jusqu'à la remise sous tension : pour changer de maître, il faut redémarrer les cartes.

## Compiler, tester, flasher, utiliser

```bash
./build.sh node                    # compile le firmware
make -C tests                      # tests sur PC : briques, simulation de chaîne, outil PC
VERBOSE=1 tests/build/test_chaine  # simulation avec les logs des nœuds

# BOOT0 = 1 + RESET sur chaque carte, puis :
./build.sh node flash              # flashe toutes les cartes branchées
# BOOT0 = 0 + RESET

python3 tools/chaine.py --in /dev/ttyUSB0 --log logs/demo.txt
#  > init 2
#  > msg Bonjour
#  > corrompu
#  > info /dev/ttyUSB1
#  > reset
```

## Matrice exigences → tests

| Exigence (spec du 08/10) | Preuve |
|---|---|
| Machine à états, démarrage en INIT, ERROR 1 s puis retour | `test_chaine` : Démarrage, Aucun message hors RUNNING, Trames invalides |
| ID 32 bits issu de l'UID | `main.c:lire_id`, log « Noeud 0x… demarre » |
| Un seul maître, init maître et esclave distinctes | `test_chaine` : Initialisation, Un seul maître |
| Nouvelle clé à chaque init, ACK du dernier | `test_chaine` : Initialisation, Synchronisation, Reset |
| Réception octet par octet, 0x7E, length > 16, flag inconnu, timeout | `test_briques` : Parseur ; `test_chaine` : Trames invalides |
| CRC16 émis et vérifié | `test_briques` : CRC16, Parseur |
| Relais sans modification, pas de relais d'une trame invalide | `test_chaine` : Message, Trames invalides |
| Câblage libre (sens découvert à l'init) | `test_chaine` : Câblage libre |
| Chiffré entre les nœuds, en clair au PC IN et au PC OUT | `test_chaine` : Message (espion sur le fil 0 → 1) |
| Triple appui, CMD_RESET, relance par le maître | `test_chaine` : Reset |
| LED 0,5 Hz / fixe / 5 Hz | `test_chaine` (motif demandé) + vidéo |
