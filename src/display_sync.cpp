#include "display_sync.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

bool IRAM_ATTR SyncDisplay::on_trans_done(esp_lcd_panel_io_handle_t,
                                          esp_lcd_panel_io_event_data_t *,
                                          void *ctx)
{
    SyncDisplay *self = (SyncDisplay *)ctx;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->dma_sem_, &woken);
    if (woken == pdTRUE) {
        // Der SPI-Treiber wertet den Rueckgabewert nicht aus, also hier selbst
        // umschalten — sonst wartet der Anzeigetask bis zum naechsten Tick.
        portYIELD_FROM_ISR();
    }
    return woken == pdTRUE;
}

esp_err_t SyncDisplay::enable_transfer_wait()
{
    if (dma_sem_ != nullptr) {
        return ESP_OK;
    }

    dma_sem_ = xSemaphoreCreateBinary();
    if (dma_sem_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done          = on_trans_done;

    const esp_err_t err = esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, this);
    if (err != ESP_OK) {
        vSemaphoreDelete(dma_sem_);
        dma_sem_ = nullptr;
    }
    return err;
}

void SyncDisplay::send_and_wait()
{
    RLCD_Display();

    if (dma_sem_ == nullptr) {
        return;
    }

    // Der Rueckruf kommt genau einmal je Uebertragung, das Semaphor ist damit
    // ausgeglichen und kann keinen Rest aus dem Vorbild enthalten. Trotzdem
    // mit Zeitschranke: bleibt die Meldung aus, soll die Anzeige langsamer
    // werden, nicht stehen bleiben — 12 ms Nutzdauer gegen 100 ms Schranke.
    if (xSemaphoreTake(dma_sem_, pdMS_TO_TICKS(100)) != pdTRUE) {
        dma_timeouts_++;
    }
}

esp_err_t SyncDisplay::pin_buffer_to_dma()
{
    if (DispBuffer == nullptr || DisplayLen <= 0) return ESP_ERR_INVALID_STATE;
    if (esp_ptr_dma_capable(DispBuffer)) return ESP_OK;

    uint8_t *neu = (uint8_t *)heap_caps_malloc((size_t)DisplayLen,
                                               MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (neu == nullptr) return ESP_ERR_NO_MEM;

    memcpy(neu, DispBuffer, (size_t)DisplayLen);
    heap_caps_free(DispBuffer);
    DispBuffer = neu;

    ESP_LOGI("display", "Bildpuffer im internen Speicher (%d Byte).", DisplayLen);
    return ESP_OK;
}

bool SyncDisplay::vollbild(const uint8_t *bits)
{
    if (width_ != 400 || (height_ & 3) != 0) return false;

    const int zeile = (width_ + 7) / 8;
    const int h4    = height_ >> 2;

    for (int by = 0; by < h4; by++) {
        // Das Pufferbyte (k, by) enthaelt die Zeilen height-1-4*by .. -3, in
        // dieser Reihenfolge von oben nach unten in den Bitpaaren.
        const uint8_t *r0 = bits + (height_ - 1 - 4 * by) * zeile;
        const uint8_t *r1 = r0 - zeile;
        const uint8_t *r2 = r1 - zeile;
        const uint8_t *r3 = r2 - zeile;
        for (int k = 0; k < width_ / 2; k++) {
            // Die Spalten 2k und 2k+1 liegen immer im selben Quellbyte.
            const int b = k >> 2;
            const int s = 6 - 2 * (k & 3);
            const uint8_t v = (uint8_t)((((r0[b] >> s) & 3) << 6) | (((r1[b] >> s) & 3) << 4)
                                        | (((r2[b] >> s) & 3) << 2) | ((r3[b] >> s) & 3));
            DispBuffer[k * h4 + by] = (uint8_t)~v;   // im Puffer ist gesetzt weiss
        }
    }
    return true;
}

// Wie bayer(8) in tools/film.py.
static const uint8_t kBayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

void SyncDisplay::abdunkeln(int stufe)
{
    if (stufe >= 64) return;
    if (width_ != 400 || (height_ & 3) != 0) {
        if (stufe <= 0) RLCD_ColorClear(ColorBlack);
        return;
    }

    // Die Matrix wiederholt sich alle acht Pixel, also alle vier Pufferbytes
    // in der Breite und alle zwei in der Hoehe: acht Masken reichen.
    uint8_t maske[2][4];
    for (int yb = 0; yb < 2; yb++) {
        for (int xb = 0; xb < 4; xb++) {
            uint8_t m = 0;
            for (int ly = 0; ly < 4; ly++) {
                for (int lx = 0; lx < 2; lx++) {
                    const int y = height_ - 1 - (4 * yb + ly);
                    const int x = 2 * xb + lx;
                    if (kBayer8[y & 7][x & 7] < stufe) m |= (uint8_t)(0x80 >> ((ly << 1) | lx));
                }
            }
            maske[yb][xb] = m;
        }
    }

    const int h4 = height_ >> 2;
    for (int k = 0; k < width_ / 2; k++) {
        for (int by = 0; by < h4; by++) {
            DispBuffer[k * h4 + by] &= maske[by & 1][k & 3];
        }
    }
}
