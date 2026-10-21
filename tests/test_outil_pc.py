"""Vérifie que l'outil PC (tools/chaine.py) parle le même protocole que le C."""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from chaine import CMD, Decodeur, construire_trame, crc16

ok = ko = 0


def verifier(cond, texte):
    global ok, ko
    if cond:
        ok += 1
    else:
        ko += 1
        print('  ECHEC :', texte)


print('Outil PC')
verifier(crc16(b'123456789') == 0x29B1, 'CRC de reference')

# même trame de référence que tests/test_briques.c
t = construire_trame(CMD['MSG'], b'Hi', id_src=0x11223344, reserve=7)
verifier(t[:10] == bytes([0x7E, 3, 2, 7, 0x44, 0x33, 0x22, 0x11]) + b'Hi', 'en-tete')
verifier(t[10:] == bytes([0xF8, 0xE5]), 'CRC 0xE5F8 en little-endian')

# logs et trames mélangés sur le même port
d = Decodeur()
flux = b'[   12] Etat INIT\n' + construire_trame(CMD['ACK'], id_src=5) + b'suite\n'
sortie = d.pousser(flux[:7], now=0) + d.pousser(flux[7:], now=0)
verifier([g for g, _ in sortie] == ['texte', 'trame', 'texte'], f'demultiplexage {sortie}')
verifier(sortie[1][1]['cmd'] == CMD['ACK'] and sortie[1][1]['crc_ok'], 'trame ACK decodee')

mauvaise = construire_trame(CMD['MSG'], b'x', crc_faux=True)
verifier(not Decodeur().pousser(mauvaise, now=0)[0][1]['crc_ok'], 'CRC faux detecte')

print(f'{ok} verifications OK, {ko} en echec')
sys.exit(1 if ko else 0)
