"""Rechnet assets/avatar.png auf das Display um und schreibt src/avatar.cpp.

Aufruf aus dem Projektverzeichnis, mit jedem Python 3 (nur Standardbibliothek):

    python tools/avatar.py

Die Vorlage ist selbst schon gerastert, aber in einem anderen Massstab als das
Panel (etwa 3,5 Bildpunkte je Rasterpunkt). Einfach verkleinern ergibt dabei
Moiré: das alte Raster schlaegt gegen das neue. Deshalb erst leicht
weichzeichnen, bis aus dem Raster wieder Grau wird, dann flaechentreu auf
400x300 verkleinern, die Mitteltoene anheben und neu rastern — nach Atkinson,
weil das nur drei Viertel des Fehlers weitergibt: Linien und ruhige Flaechen
bleiben sauber, statt wie bei Floyd-Steinberg in Punkte zu zerfallen.

Nebenbei entsteht assets/avatar_display.png, das Bild genau so, wie es das
Panel zeigt.
"""
import math
import os
import struct
import zlib

BREITE, HOEHE = 400, 300

# Unter 1 hellt die Mitteltoene auf. Die Vorlage ist ueberwiegend dunkel, und
# Atkinson verschluckt ein Viertel des Fehlers — ohne Anhebung saufen Fell und
# Helm im Schwarz ab.
GAMMA = 0.8
WURZEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def png_lesen(pfad):
    """Liest ein PNG mit 8 Bit je Kanal ohne Zeilensprung, liefert Graustufen."""
    d = open(pfad, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'kein PNG'
    pos, idat = 8, b''
    while pos < len(d):
        n, typ = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            w, h, tiefe, farbtyp, _, _, zeilensprung = struct.unpack('>IIBBBBB', body)
            assert tiefe == 8 and zeilensprung == 0, 'nur 8 Bit ohne Zeilensprung'
        elif typ == b'IDAT':
            idat += body
    kan = {0: 1, 2: 3, 4: 2, 6: 4}[farbtyp]
    roh = zlib.decompress(idat)
    st = w * kan
    zeilen, vorige, p = [], bytearray(st), 0
    for _ in range(h):
        f = roh[p]
        z = bytearray(roh[p + 1:p + 1 + st])
        p += 1 + st
        for i in range(st):
            a = z[i - kan] if i >= kan else 0
            b = vorige[i]
            c = vorige[i - kan] if i >= kan else 0
            if f == 1:
                z[i] = (z[i] + a) & 255
            elif f == 2:
                z[i] = (z[i] + b) & 255
            elif f == 3:
                z[i] = (z[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                z[i] = (z[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        vorige = z
        if kan >= 3:
            zeilen.append([(z[i * kan] * 299 + z[i * kan + 1] * 587 + z[i * kan + 2] * 114) / 1000
                           for i in range(w)])
        else:
            zeilen.append([float(z[i * kan]) for i in range(w)])
    return w, h, zeilen


def png_schreiben(pfad, w, h, zeilen):
    roh = b''.join(b'\x00' + bytes(z) for z in zeilen)

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

    open(pfad, 'wb').write(b'\x89PNG\r\n\x1a\n'
                           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(roh, 9)) + chunk(b'IEND', b''))


def weichzeichnen(bild, radius):
    """Kastenfilter, getrennt nach Zeilen und Spalten."""
    def eindimensional(reihe):
        n = len(reihe)
        summe = [0.0]
        for v in reihe:
            summe.append(summe[-1] + v)
        return [(summe[min(n, i + radius + 1)] - summe[max(0, i - radius)])
                / (min(n, i + radius + 1) - max(0, i - radius)) for i in range(n)]
    zeilen = [eindimensional(r) for r in bild]
    spalten = [eindimensional(list(s)) for s in zip(*zeilen)]
    return [list(r) for r in zip(*spalten)]


def verkleinern(bild, w, h):
    """Flaechentreu auf BREITE x HOEHE; was ueber das Seitenverhaeltnis hinausragt, wird mittig beschnitten."""
    s = min(w / BREITE, h / HOEHE)
    x0 = (w - BREITE * s) / 2
    y0 = (h - HOEHE * s) / 2
    out = []
    for ty in range(HOEHE):
        y_von = int(y0 + ty * s)
        y_bis = min(h, int(math.ceil(y0 + (ty + 1) * s)))
        reihe = []
        for tx in range(BREITE):
            x_von = int(x0 + tx * s)
            x_bis = min(w, int(math.ceil(x0 + (tx + 1) * s)))
            werte = [bild[y][x] for y in range(y_von, y_bis) for x in range(x_von, x_bis)]
            reihe.append(sum(werte) / len(werte))
        out.append(reihe)
    return out


def atkinson(grau):
    e = [r[:] for r in grau]
    schwarz = []
    for y in range(HOEHE):
        reihe = []
        for x in range(BREITE):
            v = e[y][x]
            ist_schwarz = v < 128
            reihe.append(ist_schwarz)
            fehler = (v - (0 if ist_schwarz else 255)) / 8
            for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                if 0 <= x + dx < BREITE and y + dy < HOEHE:
                    e[y + dy][x + dx] += fehler
        schwarz.append(reihe)
    return schwarz


def main():
    w, h, bild = png_lesen(os.path.join(WURZEL, 'assets', 'avatar.png'))
    for _ in range(2):
        bild = weichzeichnen(bild, 1)
    grau = [[255 * (v / 255) ** GAMMA for v in r] for r in verkleinern(bild, w, h)]
    schwarz = atkinson(grau)

    png_schreiben(os.path.join(WURZEL, 'assets', 'avatar_display.png'), BREITE, HOEHE,
                  [[0 if s else 255 for s in r] for r in schwarz])

    daten = bytearray()
    for reihe in schwarz:
        for x in range(0, BREITE, 8):
            b = 0
            for i in range(8):
                if x + i < BREITE and reihe[x + i]:
                    b |= 0x80 >> i
            daten.append(b)

    zeilen = [', '.join('0x%02x' % b for b in daten[i:i + 16]) + ','
              for i in range(0, len(daten), 16)]
    with open(os.path.join(WURZEL, 'src', 'avatar.cpp'), 'w', newline='\n', encoding='utf-8') as f:
        f.write('// Erzeugt von tools/avatar.py aus assets/avatar.png — nicht von Hand aendern.\n\n')
        f.write('#include "avatar.h"\n\n')
        f.write('const uint8_t kAvatar[%d] = {\n' % len(daten))
        for z in zeilen:
            f.write('    ' + z + '\n')
        f.write('};\n')
    print('src/avatar.cpp: %dx%d, %d Bytes, %d%% schwarz'
          % (BREITE, HOEHE, len(daten), 100 * sum(map(sum, schwarz)) // (BREITE * HOEHE)))


if __name__ == '__main__':
    main()
