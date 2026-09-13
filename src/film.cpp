#include "film.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "film";

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t Film::begin(const uint8_t *start, const uint8_t *ende)
{
    const size_t groesse = (size_t)(ende - start);
    if (groesse < 16 || memcmp(start, "HOIF", 4) != 0 || u16(start + 4) != 1) {
        ESP_LOGE(TAG, "Keine Bildfolge (Kennung oder Version falsch).");
        return ESP_ERR_INVALID_ARG;
    }
    breite_ = u16(start + 6);
    hoehe_  = u16(start + 8);
    bilder_ = u16(start + 10);
    fps_    = u16(start + 12);
    bytes_  = (breite_ + 7) / 8 * hoehe_;

    anfang_ = start + 16;
    daten_  = anfang_ + 4 * (bilder_ + 1);
    if (bilder_ == 0 || daten_ > ende || daten_ + u32(anfang_ + 4 * bilder_) != ende) {
        ESP_LOGE(TAG, "Bildfolge unvollstaendig.");
        return ESP_ERR_INVALID_SIZE;
    }

    puffer_ = (uint8_t *)heap_caps_calloc(1, bytes_, MALLOC_CAP_SPIRAM);
    if (puffer_ == nullptr) return ESP_ERR_NO_MEM;
    bild_ = -1;

    ESP_LOGI(TAG, "%dx%d, %d Bilder mit %d fps, %u KB.", breite_, hoehe_, bilder_, fps_,
             (unsigned)(groesse / 1024));
    return ESP_OK;
}

// Lauflaengen: n < 0x80 kuendigt n Bytes an, 0x80 | n ueberspringt n Bytes.
// Uebersprungen heisst hier unveraendert, denn das Delta ist dort null.
void Film::delta(int i)
{
    const uint8_t *p = daten_ + u32(anfang_ + 4 * i);
    const uint8_t *e = daten_ + u32(anfang_ + 4 * (i + 1));
    int o = 0;
    while (p < e) {
        const uint8_t c = *p++;
        if (c & 0x80) {
            o += c & 0x7F;
        } else {
            if (o + c > bytes_) break;   // kaputte Datei: lieber ein falsches Bild als ein Absturz
            for (int k = 0; k < c; k++) puffer_[o + k] ^= p[k];
            p += c;
            o += c;
        }
    }
}

void Film::gehe_zu(int ziel)
{
    if (puffer_ == nullptr) return;
    if (ziel < 0) ziel = 0;
    if (ziel >= bilder_) ziel = bilder_ - 1;

    // Rueckwaerts ueber viele Bilder ist teurer als von vorn: beim Sprung
    // vom Ende an den Anfang waren das 120 Deltas, gemessen bis 40 ms. Von
    // einem leeren Puffer aus sind es ziel + 1.
    if (ziel < bild_ && ziel + 1 < bild_ - ziel) {
        memset(puffer_, 0, bytes_);
        bild_ = -1;
    }

    while (bild_ < ziel) delta(++bild_);
    while (bild_ > ziel) delta(bild_--);
}
