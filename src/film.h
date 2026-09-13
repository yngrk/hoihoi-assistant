#pragma once

// ---------------------------------------------------------------------------
// Bildfolge aus tools/film.py abspielen.
//
// Die Datei liegt im Flash und bleibt dort; im Speicher steht immer nur das
// eine Bild, bei dem die Wiedergabe gerade ist. Jedes Bild ist als XOR zum
// vorigen gespeichert, deshalb geht es nur schrittweise weiter — vorwaerts
// wie rueckwaerts mit denselben Deltas. Ein Sprung ueber mehrere Bilder
// rechnet alle dazwischen durch; bei ein bis vier Kilobyte je Delta ist das
// billiger als ein Bild zu zeichnen.
//
// Das Format steht in tools/film.py.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <esp_err.h>

class Film {
  public:
    // start/ende: die eingebettete Datei. Legt den Bildpuffer im PSRAM an.
    esp_err_t begin(const uint8_t *start, const uint8_t *ende);

    int breite() const { return breite_; }
    int hoehe() const { return hoehe_; }
    int bilder() const { return bilder_; }
    int fps() const { return fps_; }

    // Bringt den Puffer auf Bild ziel (0 .. bilder()-1).
    void gehe_zu(int ziel);

    // Das aktuelle Bild: 1 Bit je Pixel, hoechstes Bit links, 1 = schwarz —
    // passt direkt zu Canvas::bitmap().
    const uint8_t *puffer() const { return puffer_; }

  private:
    void delta(int i);

    const uint8_t  *anfang_  = nullptr;   // Tabelle der Bildanfaenge
    const uint8_t  *daten_   = nullptr;
    uint8_t        *puffer_  = nullptr;
    int             bytes_   = 0;
    int             breite_  = 0;
    int             hoehe_   = 0;
    int             bilder_  = 0;
    int             fps_     = 0;
    int             bild_    = -1;        // -1: Puffer leer, vor Bild 0
};
