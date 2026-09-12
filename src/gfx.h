#pragma once

// ---------------------------------------------------------------------------
// Clippende Zeichenschicht ueber DisplayPort.
//
// DisplayPort::RLCD_SetPixel() prueft seine Koordinaten nicht. Die Lookup-
// Tabelle hat eine fest auf 300 gesetzte Zeilenlaenge (uint16_t (*)[300]),
// deshalb liest ein x >= 400 hinter die 240000-Byte-Allokation, und ein
// y >= 300 still in die Zeile des naechsten x — also einmal Speicherfehler,
// einmal falsches Pixel ohne Hinweis. Beides sind Faelle, die entstehen,
// sobald Koordinaten zur Laufzeit berechnet werden statt im Quelltext zu
// stehen.
//
// Der Treiber ruft RLCD_SetPixel() nirgends selbst auf, die Methode existiert
// nur fuer Code ausserhalb. Solange saemtliches Zeichnen ueber diese Huelle
// laeuft, ist die Pruefung damit lueckenlos — und components/port_bsp bleibt
// byte-identisch zu Waveshare, sodass Updates von dort weiter einfach
// nachzuziehen sind.
//
// Farben sind die Konstanten aus display_bsp.h: ColorBlack (0) und
// ColorWhite (0xff). Achtung, der Treiber wertet jeden Wert != 0 als weiss.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include "display_bsp.h"

class Canvas {
  public:
    Canvas(DisplayPort &display, int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    void clear(uint8_t color);
    void flush();

    // Alle Koordinaten sind int, nicht uint16_t: ein negativer Wert muss als
    // negativ erkennbar bleiben, statt vorher auf 65535 zu wrappen.
    void pixel(int x, int y, uint8_t color);
    void hline(int x0, int x1, int y, uint8_t color);
    void vline(int x, int y0, int y1, uint8_t color);
    void line(int x0, int y0, int x1, int y1, uint8_t color);
    void rect(int x0, int y0, int x1, int y1, uint8_t color);
    void fill_rect(int x0, int y0, int x1, int y1, uint8_t color);

  private:
    DisplayPort &d_;
    int          width_;
    int          height_;
};
