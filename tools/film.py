"""Rechnet die Clips in assets/ in Bildfolgen fuer das Panel um: wach.mp4 wird
wach.film (HoiHoi kommt nach vorn), zu.mp4 wird zu.film (er zieht den Vorhang zu).

Aufruf aus dem Projektverzeichnis, braucht Python 3 und ffmpeg im Pfad:

    python tools/film.py

ffmpeg dekodiert das Video und verkleinert flaechentreu auf 400x300, alles
Weitere passiert hier.

Gerastert wird mit einer festen 8x8-Bayer-Matrix, nicht nach Atkinson wie der
Avatar. Fehlerverteilung sieht im Standbild besser aus, aber jedes Bild
verteilt seinen Fehler neu: gemessen springen dabei 16 bis 24 Prozent der
Pixel von Bild zu Bild um, auch wo sich nichts bewegt — das flimmert. Die
feste Matrix haengt am Bildraum, und es springt nur um, was sich wirklich
aendert, gut ein Prozent.

Das Video ist sehr dunkel (Median 10 von 255). Unter SCHWARZ wird alles
schwarz, damit der Hintergrund ruhig bleibt, und die Mitteltoene werden
angehoben, sonst bleibt von der schwarzen Katze nur der Helmrand.

Ausprobiert am Panel: WEISS 140 mit GAMMA 0,6 war zu dunkel (93 Prozent der
Pixel schwarz), WEISS 70 mit GAMMA 0,45 zu hart. Das Schwarz insgesamt
anzuheben machte das Bild heller, aber auch dort ein Punktmuster, wo tiefes
Schwarz stehen soll. Jetzt hellt WEISS 85 mit GAMMA 0,5 nur die Katze und den
Lichtkegel auf, der Hintergrund bleibt schwarz.

Format der Datei, alles little endian:

    "HOIF"  u16 Version  u16 Breite  u16 Hoehe  u16 Bilder  u16 fps  u16 frei
    u32 Anfang[Bilder + 1]      Lage jedes Bildes ab Datenbeginn
    Daten

Bild i ist das XOR mit Bild i-1 (Bild 0 mit einem leeren Bild), 1 Bit je Pixel,
hoechstes Bit links, 1 = schwarz. Das XOR ist in beide Richtungen dasselbe:
rueckwaerts spielt man dieselben Deltas in umgekehrter Reihenfolge. Es ist
lauflaengenkodiert: ein Byte n < 0x80 kuendigt n Bytes an, die folgen;
0x80 | n steht fuer n Nullbytes.

Nebenbei entsteht je Clip ein <name>_vorschau.gif, die Folge genau so, wie das
Panel sie zeigt.
"""
import os
import struct
import subprocess

BREITE, HOEHE = 400, 300
FPS = 24
SCHWARZ, WEISS, GAMMA = 16, 85, 0.5

WURZEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
CLIPS = ('wach', 'zu')


def bayer(n):
    m = [[0]]
    while len(m) < n:
        k = len(m)
        m = [[4 * m[y % k][x % k] + (0, 2, 3, 1)[(y // k) * 2 + (x // k)]
              for x in range(2 * k)] for y in range(2 * k)]
    return m


def lauflaengen(daten):
    out = bytearray()
    i, n = 0, len(daten)
    while i < n:
        j = i
        if daten[i] == 0:
            while j < n and daten[j] == 0 and j - i < 127:
                j += 1
            out.append(0x80 | (j - i))
        else:
            while j < n and daten[j] != 0 and j - i < 127:
                j += 1
            out.append(j - i)
            out += daten[i:j]
        i = j
    return out


def umrechnen(name):
    QUELLE = os.path.join(WURZEL, 'assets', name + '.mp4')
    ZIEL = os.path.join(WURZEL, 'assets', name + '.film')
    VORSCHAU = os.path.join(WURZEL, 'assets', name + '_vorschau.gif')

    # Seitenverhaeltnis erst beschneiden, dann verkleinern: so bleibt die
    # Katze rund, egal wie das Video gerendert wurde.
    roh = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', QUELLE, '-vf',
         'crop=min(iw\,ih*4/3):min(ih\,iw*3/4),scale=%d:%d:flags=area,fps=%d,format=gray'
         % (BREITE, HOEHE, FPS),
         '-f', 'rawvideo', '-'], check=True, stdout=subprocess.PIPE).stdout
    groesse = BREITE * HOEHE
    bilder = len(roh) // groesse

    lut = []
    for v in range(256):
        x = min(1.0, max(0.0, (v - SCHWARZ) / (WEISS - SCHWARZ)))
        lut.append(255 * x ** GAMMA)
    b8 = bayer(8)
    schwelle = [(b8[y % 8][x % 8] + 0.5) * 4 for y in range(HOEHE) for x in range(BREITE)]

    vorher = bytes(groesse // 8)
    daten = bytearray()
    anfang = []
    panelbilder = bytearray()
    for f in range(bilder):
        grau = roh[f * groesse:(f + 1) * groesse]
        schwarz = [lut[grau[i]] <= schwelle[i] for i in range(groesse)]
        panelbilder += bytes(0 if s else 255 for s in schwarz)

        gepackt = bytearray(groesse // 8)
        for i, s in enumerate(schwarz):
            if s:
                gepackt[i >> 3] |= 0x80 >> (i & 7)
        anfang.append(len(daten))
        daten += lauflaengen(bytes(a ^ b for a, b in zip(gepackt, vorher)))
        vorher = gepackt
    anfang.append(len(daten))

    with open(ZIEL, 'wb') as f:
        f.write(b'HOIF' + struct.pack('<6H', 1, BREITE, HOEHE, bilder, FPS, 0))
        f.write(struct.pack('<%dI' % len(anfang), *anfang))
        f.write(daten)

    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'gray',
                    '-s', '%dx%d' % (BREITE, HOEHE), '-r', str(FPS), '-i', '-',
                    '-vf', 'scale=%d:%d:flags=neighbor' % (2 * BREITE, 2 * HOEHE), VORSCHAU],
                   input=bytes(panelbilder), check=True)

    print('%s: %d Bilder, %d fps, %d KB' % (os.path.relpath(ZIEL, WURZEL), bilder, FPS,
                                            os.path.getsize(ZIEL) // 1024))


def main():
    for name in CLIPS:
        umrechnen(name)


if __name__ == '__main__':
    main()
