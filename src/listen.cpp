#include "listen.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "listen";

static inline int32_t now_ms(void)
{
    return (int32_t)(esp_timer_get_time() / 1000);
}

esp_err_t Listener::begin(uint32_t sample_rate)
{
    rate_ = sample_rate;

    // Eine Sekunde mehr als die Zeitschranke. Genau kMaxSeconds waeren zu
    // knapp: der Puffer liefe voll, bevor die Uhr abgelaufen ist, und jede
    // Aufnahme endete mit der falschen Begruendung. So bleibt "Puffer voll"
    // das, was es sein soll — ein Netz, das normalerweise nicht traegt.
    capacity_ = (size_t)sample_rate * (kMaxSeconds + 1);

    // Der Mitschnitt gehoert in den PSRAM: 320 KB waeren ein Zehntel des
    // internen Speichers, und schnell genug ist er allemal fuer 16 kHz.
    buf_ = (int16_t *)heap_caps_malloc(capacity_ * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf_ == nullptr) {
        capacity_ = 0;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int32_t Listener::elapsed_ms() const
{
    if (listening_ == 0) return 0;
    return now_ms() - started_ms_;
}

void Listener::start()
{
    fill_        = 0;
    live_frames_ = 0;
    peak_        = 0;
    started_ms_  = now_ms();
    last_frames_ = 0;
    listening_   = 1;

    ESP_LOGI(TAG, "Zuhoeren gestartet.");
}

void Listener::stop(const char *grund)
{
    // Erst das Ergebnis, dann das Ende: wer auf die fallende Flanke von
    // listening_ wartet, um den Mitschnitt abzuholen, muss ihn zu diesem
    // Zeitpunkt schon vollstaendig vorfinden. Andersherum saehe der
    // Transkriptionstask fuer einen Moment noch last_frames_ == 0 und
    // schickte das Ende der Aeusserung nicht mehr los.
    last_ms_     = now_ms() - started_ms_;
    last_peak_   = peak_;
    last_frames_ = (int32_t)fill_;
    listening_   = 0;

    // Der Puffer bleibt stehen — hier setzt spaeter die Worterkennung an.
    ESP_LOGI(TAG, "Zuhoeren beendet (%s): %d ms, %u Frames, Spitze %d.",
             grund, (int)last_ms_, (unsigned)fill_, (int)peak_);
}

void Listener::poll_key(bool pressed)
{
    const int32_t t = now_ms();

    if (pressed) {
        released_ = 0;
        if (listening_ == 0 && !gesperrt_) {
            start();
        }
    } else {
        // Erst ein paar Abfragen ohne Tastendruck gelten als Loslassen. Ein
        // einzelner Prellimpuls waehrend des Haltens schneidet sonst mitten
        // im Wort ab.
        if (released_ < kReleasePolls) released_++;
        if (released_ >= kReleasePolls) {
            gesperrt_ = false;
            if (listening_ != 0) {
                stop("Taste losgelassen");
            }
        }
    }

    if (listening_ != 0 && (t - started_ms_) >= kMaxSeconds * 1000) {
        stop("Zeit abgelaufen");
        // Wer die Taste weiter haelt, bekommt keine zweite Aufnahme
        // hinterher — erst loslassen.
        gesperrt_ = true;
    }
}

void Listener::feed(const int16_t *pcm, size_t frames)
{
    if (listening_ == 0 || buf_ == nullptr) return;

    size_t room = capacity_ - fill_;
    if (frames < room) room = frames;

    if (room > 0) {
        memcpy(&buf_[fill_], pcm, room * sizeof(int16_t));

        for (size_t i = 0; i < room; i++) {
            const int16_t s = pcm[i];
            const int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
            if (a > peak_) peak_ = a;
        }
        fill_ += room;
        live_frames_ = (int32_t)fill_;
    }

    // Der Puffer reicht fuer kMaxSeconds, die Zeitschranke in poll_key() greift
    // normalerweise zuerst. Trotzdem abfangen, statt still zu verwerfen.
    if (fill_ >= capacity_) {
        stop("Puffer voll");
    }
}
