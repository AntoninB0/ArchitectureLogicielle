#!/usr/bin/env python3
"""Outil PC de la chaîne : PC IN (envoi des commandes au maître) et PC OUT /
moniteur (affiche ce que toutes les cartes envoient sur leur USART1 : logs
texte et trames décodées).

Usage :
  chaine.py                          écoute toutes les cartes, commandes au clavier
  chaine.py --in /dev/ttyUSB0        les commandes partent vers ce port (maître)
  chaine.py --log demo.txt           enregistre tout dans un fichier
  chaine.py --script "init 2; attendre 1; msg Bonjour; attendre 1"
                                     joue les commandes puis quitte

Commandes :
  init [n]        CMD_INIT vers le maître (n = nombre de nœuds, défaut 2)
  msg <texte>     CMD_MSG (coupé en morceaux de 16 octets)
  reset           CMD_RESET
  info [port]     CMD_GET_INFO (vers le maître, ou vers [port])
  key [port]      CMD_GET_KEY
  corrompu        CMD_MSG avec un CRC faux (démo de l'état ERROR)
  attendre <s>    pause (pour les scripts)
  quit
"""
import argparse, glob, struct, sys, threading, time

START = 0x7E
MAX_PAYLOAD = 16
DEFAULT_ID = 0
CMD = {'INIT': 0, 'RESET': 1, 'ACK': 2, 'MSG': 3, 'ERROR': 4, 'GET_INFO': 5, 'GET_KEY': 6}
NOM_CMD = {v: k for k, v in CMD.items()}
ERREURS = {1: 'CRC faux', 2: 'longueur invalide', 3: 'flag inconnu', 4: 'trame incomplete',
           5: "refusee dans l'etat courant", 6: 'refusee pour ce role', 7: 'configuration invalide'}
STATUTS = {0: 'inconnu', 1: 'maitre', 2: 'esclave'}
TIMEOUT_TRAME_S = 0.1


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE, identique à node/src/crc16.c"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def construire_trame(cmd, payload=b'', id_src=DEFAULT_ID, reserve=0, crc_faux=False):
    assert len(payload) <= MAX_PAYLOAD
    entete = struct.pack('<BBBBI', START, cmd, len(payload), reserve, id_src)
    crc = crc16(entete + payload) ^ (0xFFFF if crc_faux else 0)
    return entete + payload + struct.pack('<H', crc)


class Decodeur:
    """Sépare le flux d'un USART1 en lignes de log et en trames.
    Les logs ne contiennent jamais 0x7E, qui marque donc un début de trame."""

    def __init__(self):
        self.texte = bytearray()
        self.trame = None
        self.debut = 0.0

    def pousser(self, data, now=None):
        now = time.monotonic() if now is None else now
        sortie = []
        if self.trame is not None and now - self.debut > TIMEOUT_TRAME_S:
            self.texte += self.trame
            self.trame = None
        for b in data:
            if self.trame is None:
                if b == START:
                    self.trame, self.debut = bytearray([b]), now
                elif b == ord('\n'):
                    sortie.append(('texte', self.texte.decode(errors='replace').rstrip('\r')))
                    self.texte.clear()
                else:
                    self.texte.append(b)
                continue
            self.trame.append(b)
            t = self.trame
            if (len(t) == 2 and t[1] not in NOM_CMD) or (len(t) == 3 and t[2] > MAX_PAYLOAD):
                self.texte += t           # ce n'était pas une trame
                self.trame = None
            elif len(t) >= 3 and len(t) == 8 + t[2] + 2:
                sortie.append(('trame', decoder(bytes(t))))
                self.trame = None
        return sortie


def decoder(t):
    _, cmd, length, reserve, id_src = struct.unpack('<BBBBI', t[:8])
    payload = t[8:8 + length]
    (crc,) = struct.unpack('<H', t[8 + length:])
    return {'cmd': cmd, 'id': id_src, 'reserve': reserve, 'payload': payload,
            'crc_ok': crc == crc16(t[:8 + length])}


def decrire(f):
    nom, p = NOM_CMD.get(f['cmd'], '?'), f['payload']
    if not f['crc_ok']:
        return f"TRAME {nom} avec CRC FAUX"
    if nom == 'MSG':
        return f'MSG (PC OUT) : "{p.decode(errors="replace")}"'
    if nom == 'ERROR' and p:
        return f"ERROR {p[0]} ({ERREURS.get(p[0], '?')}) signalee par 0x{f['id']:08x}"
    if nom == 'GET_INFO' and len(p) == 3:
        return f"INFO id=0x{p[0]:02x} position={p[1]} statut={STATUTS.get(p[2], '?')}"
    if nom == 'GET_KEY' and len(p) == 4:
        return f"KEY cle=0x{struct.unpack('<I', p)[0]:08x}"
    return f"{nom} de 0x{f['id']:08x} {p.hex()}"


class Chaine:
    def __init__(self, ports, port_in, log):
        import serial
        self.links = {p: serial.Serial(p, 115200, timeout=0.05) for p in ports}
        self.port_in = port_in or ports[0]
        self.log = log
        self.verrou = threading.Lock()
        self.actif = True
        for p in ports:
            threading.Thread(target=self.ecouter, args=(p,), daemon=True).start()

    def afficher(self, port, texte):
        ligne = f"{time.strftime('%H:%M:%S')} [{port[-7:]}] {texte}"
        with self.verrou:
            print(ligne, flush=True)
            if self.log:
                self.log.write(ligne + '\n')
                self.log.flush()

    def ecouter(self, port):
        dec = Decodeur()
        while self.actif:
            data = self.links[port].read(256)
            for genre, contenu in dec.pousser(data):
                self.afficher(port, contenu if genre == 'texte' else '<- ' + decrire(contenu))

    def envoyer(self, cmd, payload=b'', port=None, crc_faux=False):
        port = port or self.port_in
        self.afficher(port, f"-> {NOM_CMD[cmd]} {payload!r}" + (" (CRC faux)" if crc_faux else ""))
        self.links[port].write(construire_trame(cmd, payload, crc_faux=crc_faux))

    def commande(self, ligne):
        mots = ligne.strip().split(maxsplit=1)
        if not mots:
            return True
        c, arg = mots[0].lower(), (mots[1] if len(mots) > 1 else '')
        if c == 'init':
            self.envoyer(CMD['INIT'], bytes([int(arg)]) if arg else b'')
        elif c == 'msg':
            data = arg.encode()
            for i in range(0, max(len(data), 1), MAX_PAYLOAD):
                self.envoyer(CMD['MSG'], data[i:i + MAX_PAYLOAD])
                time.sleep(0.05)
        elif c == 'reset':
            self.envoyer(CMD['RESET'])
        elif c in ('info', 'key'):
            self.envoyer(CMD['GET_INFO' if c == 'info' else 'GET_KEY'], port=arg or None)
        elif c == 'corrompu':
            self.envoyer(CMD['MSG'], b'corrompu', crc_faux=True)
        elif c == 'attendre':
            time.sleep(float(arg or 1))
        elif c in ('quit', 'exit', 'q'):
            return False
        else:
            print("Commandes : init [n] | msg <texte> | reset | info [port] | key [port] | "
                  "corrompu | attendre <s> | quit")
        return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--in', dest='port_in', help='port du maître (défaut : le premier)')
    ap.add_argument('--log', help='fichier où enregistrer la session')
    ap.add_argument('--script', help='commandes séparées par ";", puis fin')
    ap.add_argument('ports', nargs='*', help='ports à écouter (défaut : /dev/ttyUSB*)')
    a = ap.parse_args()

    ports = a.ports or sorted(glob.glob('/dev/ttyUSB*'))
    if a.port_in and a.port_in not in ports:
        ports.append(a.port_in)
    if not ports:
        sys.exit("Aucun adaptateur USB-série branché (/dev/ttyUSB*)")
    log = open(a.log, 'a') if a.log else None
    ch = Chaine(ports, a.port_in, log)
    print(f"Ecoute de {', '.join(ports)} ; commandes vers {ch.port_in}. 'quit' pour sortir.")
    try:
        if a.script:
            for c in a.script.split(';'):
                ch.commande(c)
            time.sleep(0.5)
        else:
            for ligne in sys.stdin:
                if not ch.commande(ligne):
                    break
    except KeyboardInterrupt:
        pass
    ch.actif = False


if __name__ == '__main__':
    main()
