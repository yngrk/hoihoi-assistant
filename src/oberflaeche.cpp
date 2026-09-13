#include "oberflaeche.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_timer.h>

#include "display_bsp.h"

namespace oberflaeche {
namespace {

const uint8_t S = ColorBlack;
const uint8_t W = ColorWhite;

// Wie bayer(8) in tools/film.py und in display_sync.cpp.
const uint8_t kBayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

// Kleine Bilder, '#' ist schwarz. Alle Zeilen eines Bildes gleich lang.
const char *const kKatze[] = {
    "..#.........#..", ".#.#.......#.#.", ".#..#######..#.", ".#...........#.",
    "#.............#", "#..##.....##..#", "#..##.....##..#", "#.............#",
    "#......#......#", "##....#.#....##", ".#...........#.", "..##.......##..",
    "....#######....",
};
const char *const kThermometer[] = {
    "..#..", ".#.#.", ".#.#.", ".#.#.", ".###.", ".###.", "#####", "#####", ".###.",
};
const char *const kTropfen[] = {
    "...#...", "...#...", "..###..", "..###..", ".#####.", "#######", "#######", ".#####.", "..###..",
};
const char *const kMikro[] = {
    "..###..", ".#####.", ".#####.", ".#####.", "#.###.#", "#.....#", ".#####.", "...#...", "..###..",
};
const char *const kLautsprecher[] = {
    "...#.....", "..##..#..", "####...#.", "####.#.#.", "####.#.#.", "####...#.", "..##..#..", "...#.....",
};
const char *const kOhr[] = {
    "..###..", ".#...#.", "#.....#", "#..#..#", "#.#...#", "....#.#", "....#..", "..##...", "..#....",
};
const char *const kSanduhr[] = {
    "#######", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#.#.#.", "#######",
};

#define ANZAHL(a) (int)(sizeof(a) / sizeof((a)[0]))

void bild(Canvas &c, int x, int y, const char *const *zeilen, int n, uint8_t farbe, int k = 1)
{
    for (int j = 0; j < n; j++) {
        for (int i = 0; zeilen[j][i] != '\0'; i++) {
            if (zeilen[j][i] == '#') c.fill_rect(x + i * k, y + j * k, x + (i + 1) * k - 1, y + (j + 1) * k - 1, farbe);
        }
    }
}

// Flaeche im Raster: stufe 0 weiss bis 64 schwarz.
void raster(Canvas &c, int x0, int y0, int x1, int y1, int stufe)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) c.pixel(x, y, kBayer8[y & 7][x & 7] < stufe ? S : W);
    }
}

// Rechteck mit abgeschnittenen Ecken, wahlweise gefuellt.
void rund(Canvas &c, int x0, int y0, int x1, int y1, uint8_t rand, int fuellung = -1)
{
    if (fuellung >= 0) c.fill_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, (uint8_t)fuellung);
    c.hline(x0 + 2, x1 - 2, y0, rand);
    c.hline(x0 + 2, x1 - 2, y1, rand);
    c.vline(x0, y0 + 2, y1 - 2, rand);
    c.vline(x1, y0 + 2, y1 - 2, rand);
    c.pixel(x0 + 1, y0 + 1, rand);
    c.pixel(x1 - 1, y0 + 1, rand);
    c.pixel(x0 + 1, y1 - 1, rand);
    c.pixel(x1 - 1, y1 - 1, rand);
}

// Text mit zwei Pixel breitem Rand in der Gegenfarbe, damit er auf Raster
// stehen kann.
void umrandet(Canvas &c, int x, int y, const char *s, uint8_t farbe, int k)
{
    static const int8_t kVersatz[][2] = {
        {-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-1, -1}, {1, 1}, {-1, 1}, {1, -1},
        {-2, -1}, {2, 1}, {-1, 2}, {1, -2}, {-2, 1}, {2, -1}, {1, 2}, {-1, -2},
    };
    const uint8_t gegen = farbe == S ? W : S;
    for (const auto &v : kVersatz) c.text(x + v[0], y + v[1], s, gegen, k);
    c.text(x, y, s, farbe, k);
}

// Fenster mit gerastertem Rahmen und schwarzer Titelleiste.
void fenster(Canvas &c, int x, int y, int w, int h, const char *titel)
{
    const int x1 = x + w - 1, y1 = y + h - 1;
    rund(c, x, y, x1, y1, S, W);
    raster(c, x + 2, y + 2, x1 - 2, y + 3, 20);
    raster(c, x + 2, y1 - 3, x1 - 2, y1 - 2, 20);
    raster(c, x + 2, y + 4, x + 3, y1 - 4, 20);
    raster(c, x1 - 3, y + 4, x1 - 2, y1 - 4, 20);
    c.rect(x + 3, y + 3, x1 - 3, y1 - 3, S);
    c.fill_rect(x + 4, y + 4, x1 - 4, y + 15, S);
    raster(c, x + 4, y + 16, x1 - 4, y + 17, 40);
    c.text(x + 10, y + 7, titel, W);
    rund(c, x1 - 16, y + 6, x1 - 7, y + 13, W, S);
    c.text(x1 - 14, y + 7, "x", W);
}

// Balken auf der schwarzen Leiste, wie HP und MP im Spiel.
void balken(Canvas &c, int x, int y, int w, int h, float anteil, int stufe)
{
    c.rect(x, y, x + w - 1, y + h - 1, W);
    c.fill_rect(x + 1, y + 1, x + w - 2, y + h - 2, S);
    if (anteil <= 0.0f) return;
    if (anteil > 1.0f) anteil = 1.0f;
    const int ende = x + 1 + (int)lroundf((w - 3) * anteil);
    for (int yy = y + 2; yy <= y + h - 3; yy++) {
        for (int xx = x + 2; xx <= ende; xx++) c.pixel(xx, yy, kBayer8[yy & 7][xx & 7] < stufe ? W : S);
    }
}

void mittig(Canvas &c, int mitte, int y, const char *s, uint8_t farbe, int k = 1)
{
    c.text(mitte - (Canvas::text_width(s, k) >> 1), y, s, farbe, k);
}

// Diagonale von links oben nach rechts unten, n Schritte zu je k Pixeln. Rechts
// daneben ein Rand in der Gegenfarbe, sonst ginge sie im Bild darunter unter.
void strich(Canvas &c, int x0, int y0, int n, uint8_t farbe, uint8_t gegen, int k = 1)
{
    for (int i = 0; i < n; i++) {
        const int x = x0 + i * k, y = y0 + i * k;
        c.fill_rect(x + k, y, x + 2 * k - 1, y + k - 1, gegen);
        c.fill_rect(x, y, x + k - 1, y + k - 1, farbe);
    }
}

// Das Mikrofon, durchgestrichen.
void mikro_aus(Canvas &c, int x, int y, uint8_t farbe, uint8_t gegen, int k = 1)
{
    bild(c, x, y, kMikro, ANZAHL(kMikro), farbe, k);
    strich(c, x - k, y, ANZAHL(kMikro), farbe, gegen, k);
}

// --- Welle -------------------------------------------------------------------
//
// Balken mit runden Enden, aus der Mitte nach aussen: das Neueste steht in
// der Mitte, zu den Raendern hin wird es aelter und flacher. Auch ohne
// Ausschlag kraeuselt sie sich ein wenig, damit man sieht, dass sie lebt.

const int kBalken = 57;             // ungerade, damit es eine Mitte gibt
const int kHalb   = kBalken / 2;
float     verlauf[kHalb + 1];

const int kWelleX0 = 40, kWelleY0 = 167, kWelleX1 = 360, kWelleY1 = 212;

void welle(Canvas &c, bool stumm)
{
    c.fill_rect(kWelleX0, kWelleY0, kWelleX1, kWelleY1, W);

    const int   mitte = (kWelleY0 + kWelleY1 + 1) / 2;
    const int   hmax  = 21;
    const int   x0    = 200 - ((kBalken * 5 - 2) >> 1);
    const float phase = (float)(esp_timer_get_time() / 1000) * 0.004f;

    for (int i = 0; i < kBalken; i++) {
        const int   d      = i > kHalb ? i - kHalb : kHalb - i;
        const float r      = (float)d / (kHalb + 1);
        const float huelle = 1.0f - 0.6f * r * r;
        const float ruhig  = 1.5f + 1.5f * sinf(phase - d * 0.7f);
        int h = (int)lroundf((ruhig + verlauf[d] * (hmax - 3)) * huelle);
        if (h < 1) h = 1;
        if (h > hmax) h = hmax;
        // Stumm kraeuselt sich nichts: flach heisst, hier kommt nichts an.
        if (stumm) h = 1;

        const int bx = x0 + i * 5;
        c.fill_rect(bx, mitte - h + 1, bx + 2, mitte + h - 1, S);
        c.pixel(bx + 1, mitte - h, S);
        c.pixel(bx + 1, mitte + h, S);
    }

    if (stumm) {
        // Ein Schild mitten auf der flachen Linie.
        const int k  = 2;
        const int tw = Canvas::text_width("STUMM", k);
        const int bw = 7 * k + 8 + tw + 20;
        const int bx = 200 - bw / 2;
        rund(c, bx, mitte - 13, bx + bw - 1, mitte + 13, S, W);
        mikro_aus(c, bx + 10, mitte - 9, S, W, k);
        c.text(bx + 10 + 7 * k + 8, mitte - 6, "STUMM", S, k);
    }
}

// --- Ganzes Bild --------------------------------------------------------------

const char *const kWochentage[] = {
    "SONNTAG", "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG",
};

void kopfzeile(Canvas &c, const Stand &s);
void leiste(Canvas &c, const Stand &s);

void alles(Canvas &c, const Stand &s)
{
    char buf[40];

    // Das Fenster laesst aussen vier Pixel und seine Ecken frei. Was dort
    // vorher stand, soll nicht durchscheinen.
    c.clear(W);

    if (s.zeit_gueltig) {
        snprintf(buf, sizeof(buf), "%s  %02d.%02d.%04d", kWochentage[s.wochentag % 7], s.tag, s.monat, s.jahr);
    } else {
        snprintf(buf, sizeof(buf), "HOIHOI");
    }
    fenster(c, 4, 4, 392, 292, buf);

    // HoiHoi im Helm.
    rund(c, 14, 26, 124, 142, S, W);
    raster(c, 16, 28, 122, 140, 22);
    rund(c, 22, 32, 116, 120, S, W);
    c.circle(69, 76, 37, S);
    bild(c, 47, 57, kKatze, ANZAHL(kKatze), S, 3);
    rund(c, 22, 124, 116, 138, S, S);
    mittig(c, 69, 128, "HOIHOI", W);

    // Die Uhr auf dem Himmel. Doppelt um vier Pixel versetzt, damit sie fett
    // wird; der Rand gehoert zu beiden.
    rund(c, 130, 26, 386, 142, S, W);
    raster(c, 132, 28, 384, 140, 7);
    c.hline(132, 384, 28, S);
    static const int16_t kWolken[][2] = {{150, 44}, {330, 120}, {352, 40}};
    for (const auto &w : kWolken) {
        const int x = w[0], y = w[1];
        rund(c, x, y, x + 26, y + 9, S, W);
        rund(c, x + 6, y - 4, x + 20, y + 3, S, W);
        c.fill_rect(x + 7, y + 1, x + 19, y + 8, W);
    }
    if (s.zeit_gueltig) {
        snprintf(buf, sizeof(buf), "%02d:%02d", s.stunde, s.minute);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    const int k  = 8;
    const int ux = 254 - (Canvas::text_width(buf, k) >> 1);
    umrandet(c, ux, 56, buf, S, k);
    umrandet(c, ux + 4, 56, buf, S, k);
    c.text(ux, 56, buf, S, k);

    // Wellenfeld; Kopfzeile und Leiste zeichnen ihre eigenen Teile.
    rund(c, 14, 148, 386, 216, S, W);
    raster(c, 16, 163, 384, 214, 4);
    kopfzeile(c, s);

    c.fill_rect(8, 222, 391, 291, S);
    raster(c, 8, 222, 391, 223, 40);
    leiste(c, s);
}

void kopfzeile(Canvas &c, const Stand &s)
{
    c.fill_rect(16, 150, 384, 162, S);
    switch (s.zustand) {
    case Zustand::Ruhe:
        if (s.stumm) {
            mikro_aus(c, 22, 152, W, S);
            c.text(36, 153, "MIKROFON AUS  -  TASTE DRUECKEN", W);
        } else {
            bild(c, 22, 152, kOhr, ANZAHL(kOhr), W);
            c.text(36, 153, "SAG HOIHOI", W);
        }
        break;
    case Zustand::HoertZu:
        bild(c, 22, 152, kMikro, ANZAHL(kMikro), W);
        c.text(36, 153, "HOERT ZU", W);
        break;
    case Zustand::DenktNach:
        bild(c, 22, 153, kSanduhr, ANZAHL(kSanduhr), W);
        c.text(36, 153, "DENKT NACH", W);
        break;
    case Zustand::Antwortet:
        bild(c, 21, 152, kLautsprecher, ANZAHL(kLautsprecher), W);
        c.text(36, 153, "ANTWORTET", W);
        break;
    }
}

// Die schwarze Leiste unten, ohne ihren Rasterrand.
void leiste(Canvas &c, const Stand &s)
{
    char buf[16];
    c.fill_rect(8, 224, 391, 291, S);

    rund(c, 14, 228, 106, 285, W, W);
    c.text(20, 233, "TEMP", S);
    bild(c, 20, 251, kThermometer, ANZAHL(kThermometer), S);
    if (s.klima_gueltig) {
        const int32_t t = s.temp_c100 < 0 ? -s.temp_c100 : s.temp_c100;
        snprintf(buf, sizeof(buf), "%s%d.%d\x7F", s.temp_c100 < 0 ? "-" : "", (int)(t / 100), (int)(t / 10 % 10));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    c.text(32, 251, buf, S, 2);

    rund(c, 112, 228, 184, 285, W, W);
    c.text(118, 233, "FEUCHTE", S);
    bild(c, 118, 251, kTropfen, ANZAHL(kTropfen), S);
    if (s.klima_gueltig) {
        snprintf(buf, sizeof(buf), "%d%%", (int)(s.feuchte_100 / 100));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    c.text(132, 251, buf, S, 2);

    c.text(194, 236, "AKKU", W);
    if (s.akku_prozent >= 0) {
        balken(c, 224, 233, 124, 15, s.akku_prozent / 100.0f, 64);
        snprintf(buf, sizeof(buf), "%d%%", s.akku_prozent);
    } else {
        balken(c, 224, 233, 124, 15, 0.0f, 64);
        snprintf(buf, sizeof(buf), "--");
    }
    c.text(354, 237, buf, W);

    // Empfang zwischen -90 dBm (kaum) und -40 dBm (voll).
    c.text(194, 266, "WLAN", W);
    float wlan = 0.0f;
    switch (s.wlan) {
    case Stand::Wlan::Aus:
        snprintf(buf, sizeof(buf), "AUS");
        break;
    case Stand::Wlan::Einrichten:
        snprintf(buf, sizeof(buf), "SETUP");
        break;
    case Stand::Wlan::Verbunden:
        wlan = (s.rssi + 90) / 50.0f;
        snprintf(buf, sizeof(buf), "%d", s.rssi);
        break;
    }
    balken(c, 224, 263, 124, 15, wlan, 26);
    c.text(354, 267, buf, W);
}

// Was sichtbar ist, in der Aufloesung, in der es sichtbar ist — je Bereich.
// Die Uhr sitzt im Fenster, und das Fenster liegt unter allem: aendert sie
// sich, wird alles neu gezeichnet. Einmal je Minute ist das billig genug.
bool zeit_gleich(const Stand &a, const Stand &b)
{
    return a.zeit_gueltig == b.zeit_gueltig
           && a.stunde == b.stunde && a.minute == b.minute
           && a.wochentag == b.wochentag && a.tag == b.tag && a.monat == b.monat && a.jahr == b.jahr;
}

bool leiste_gleich(const Stand &a, const Stand &b)
{
    return a.klima_gueltig == b.klima_gueltig
           && a.temp_c100 / 10 == b.temp_c100 / 10 && a.feuchte_100 / 100 == b.feuchte_100 / 100
           && a.akku_prozent == b.akku_prozent
           && a.wlan == b.wlan && a.rssi == b.rssi;
}

Stand letzter;
bool  gezeichnet = false;

}  // namespace

void zeichnen(Canvas &c, const Stand &s)
{
    float p = s.pegel;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    memmove(verlauf + 1, verlauf, kHalb * sizeof(verlauf[0]));
    verlauf[0] = p;

    if (!gezeichnet || !zeit_gleich(s, letzter)) {
        alles(c, s);
    } else {
        if (s.zustand != letzter.zustand || s.stumm != letzter.stumm) kopfzeile(c, s);
        if (!leiste_gleich(s, letzter)) leiste(c, s);
    }
    letzter    = s;
    gezeichnet = true;
    welle(c, s.stumm && s.zustand == Zustand::Ruhe);
}

}  // namespace oberflaeche
