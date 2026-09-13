#pragma once

// ---------------------------------------------------------------------------
// Proportionale Bitmapschrift fuer Datum und Werte, im Stil der iOS-Anzeigen.
// Gerastert aus Source Han Sans JP (SIL Open Font License 1.1), siehe
// schrift.cpp; der 5x7-Font bleibt fuer alles Uebrige.
//
// Jede Glyphe ist so hoch wie die Schrift, ein Bit je Pixel, zeilenweise auf
// ganze Bytes aufgefuellt, hoechstes Bit links.
// ---------------------------------------------------------------------------

#include <cstdint>

struct Glyphe {
    uint16_t zeichen;   // Unicode, nur Latin-1
    uint16_t ofs;       // in bits
    int8_t   breite;    // Tinte
    int8_t   links;     // Abstand vom Stift bis zur Tinte
    int8_t   vorschub;
};

struct Schrift {
    int           hoehe;
    int           basis;   // Grundlinie von oben
    int           anzahl;
    const Glyphe *glyphen;
    const uint8_t *bits;
};

extern const Schrift kSchriftFett;     // 17 px, Datum
extern const Schrift kSchriftNormal;   // 14 px, Werte

// Naechstes Zeichen aus UTF-8, 0 am Ende
inline uint16_t schrift_naechstes(const char *&p)
{
    const uint8_t c = (uint8_t)*p;
    if (c == 0) return 0;
    p++;
    if (c < 0x80) return c;
    if ((c & 0xe0) == 0xc0 && (*p & 0xc0) == 0x80) return (uint16_t)(((c & 0x1f) << 6) | (*p++ & 0x3f));
    while ((*p & 0xc0) == 0x80) p++;
    return '?';
}

inline const Glyphe *schrift_glyphe(const Schrift &s, uint16_t z)
{
    for (int i = 0; i < s.anzahl; i++)
        if (s.glyphen[i].zeichen == z) return &s.glyphen[i];
    return nullptr;
}
