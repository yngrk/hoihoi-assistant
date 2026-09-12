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

#include <driver/gpio.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "display_bsp.h"

class Canvas {
  public:
    Canvas(DisplayPort &display, int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    void clear(uint8_t color);

    // Schiebt den Puffer zum Panel. Ist die TE-Synchronisation aktiv, wartet
    // flush() vorher auf die naechste Austastluecke.
    void flush();

    // --- Tearing-Effect-Synchronisation ----------------------------------
    //
    // Der ST7305 meldet ueber die TE-Leitung (Befehl 0x35, in RLCD_Init()
    // bereits mit Parameter 0x00 aktiviert), wann er sich im vertikalen
    // Austastintervall befindet. Der Waveshare-Treiber wertet das Signal nicht
    // aus und schickt den Puffer per DMA los, wann immer es passt — der
    // Transfer laeuft dann quer ueber den Bildaufbau des Panels und wandert
    // von Bild zu Bild, was als Tearing sichtbar wird.
    //
    // enable_tearing_sync() haengt eine Unterbrechungsroutine an den Pin und
    // laesst flush() auf die naechste Flanke warten.
    esp_err_t enable_tearing_sync(int te_gpio);

    // Gemessene Periode zwischen zwei TE-Flanken, also die tatsaechliche
    // Bildwiederholzeit des Panels. 0, solange nichts gemessen wurde.
    uint32_t te_period_us() const { return te_period_us_; }

    // Anzahl der flush()-Aufrufe, die vergeblich auf TE gewartet haben.
    uint32_t te_timeouts() const { return te_timeouts_; }

    // Alle Koordinaten sind int, nicht uint16_t: ein negativer Wert muss als
    // negativ erkennbar bleiben, statt vorher auf 65535 zu wrappen.
    void pixel(int x, int y, uint8_t color);
    void hline(int x0, int x1, int y, uint8_t color);
    void vline(int x, int y0, int y1, uint8_t color);
    void line(int x0, int y0, int x1, int y1, uint8_t color);
    void rect(int x0, int y0, int x1, int y1, uint8_t color);
    void fill_rect(int x0, int y0, int x1, int y1, uint8_t color);

    // Text aus dem 5x7-Bitmapfont. x/y ist die linke obere Ecke des ersten
    // Zeichens, scale vergroessert ganzzahlig — bei einem Bit je Pixel gibt es
    // keine Zwischenstufen, also auch keinen Grund fuer etwas anderes.
    // Zeichen ausserhalb von 0x20..0x7F werden als '?' gezeichnet.
    void text(int x, int y, const char *s, uint8_t color, int scale = 1);

    // Breite in Pixeln, die text() belegen wuerde — fuer rechtsbuendige oder
    // zentrierte Ausgabe, ohne die Zeichenbreite an der Aufrufstelle
    // nachzurechnen.
    static int text_width(const char *s, int scale = 1);
    static int text_height(int scale = 1);

  private:
    static void te_isr(void *arg);

    DisplayPort     &d_;
    int              width_;
    int              height_;

    SemaphoreHandle_t te_sem_       = nullptr;
    int               te_gpio_      = -1;
    volatile int64_t  te_last_us_   = 0;
    volatile uint32_t te_period_us_ = 0;
    volatile uint32_t te_timeouts_  = 0;
};
