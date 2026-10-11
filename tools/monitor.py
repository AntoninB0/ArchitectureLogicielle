#!/usr/bin/env python3
"""Affiche en même temps ce que toutes les cartes envoient (USART1/A9 -> PC).
Chaque ligne est préfixée par le port. Ctrl-C pour quitter.
Usage : monitor.py [fichier_log]"""
import glob, sys, time, serial

ports = sorted(glob.glob('/dev/ttyUSB*'))
if not ports:
    sys.exit("Aucun adaptateur USB-série branché (/dev/ttyUSB*)")
log = open(sys.argv[1], 'a') if len(sys.argv) > 1 else None
links = {p: serial.Serial(p, 115200, timeout=0) for p in ports}
bufs = {p: b'' for p in ports}
print("Écoute de " + ", ".join(ports) + " (Ctrl-C pour quitter)")
try:
    while True:
        for p, s in links.items():
            bufs[p] += s.read(512)
            while b'\n' in bufs[p]:
                line, bufs[p] = bufs[p].split(b'\n', 1)
                txt = f"{time.strftime('%H:%M:%S')} [{p[-7:]}] {line.decode(errors='replace').rstrip()}"
                print(txt, flush=True)
                if log:
                    log.write(txt + '\n'); log.flush()
        time.sleep(0.01)
except KeyboardInterrupt:
    pass
