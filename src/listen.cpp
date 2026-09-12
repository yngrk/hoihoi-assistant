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
    peak_        = 0;
    started_ms_  = now_ms();
    last_frames_ = 0;
    listening_   = 1;

    ESP_LOGI(TAG, "Zuhoeren gestartet.");
}

void Listener::stop(const char *grund)
{
    listening_   = 0;
    last_ms_     = now_ms() - started_ms_;
    last_peak_   = peak_;
    last_frames_ = (int32_t)fill_;

    // Der Puffer bleibt stehen — hier setzt spaeter die Worterkennung an.
    ESP_LOGI(TAG, "Zuhoeren beendet (%s): %d ms, %u Frames, Spitze %d.",
             grund, (int)last_ms_, (unsigned)fill_, (int)peak_);
}

void Listener::poll_key(bool pressed)
{
    const int32_t t = now_ms();

    // Nur die fallende Flanke zaehlt, und die auch nur mit Mindestabstand:
    // ein Prellen der Taste wuerde sonst sofort wieder zurueckschalten.
    if (pressed && !key_was_pressed_ && (t - last_edge_ms_) >= kDebounceMs) {
        last_edge_ms_ = t;
        if (listening_ != 0) {
            stop("Taste");
        } else {
            start();
        }
    }
    key_was_pressed_ = pressed;

    if (listening_ != 0 && (t - started_ms_) >= kMaxSeconds * 1000) {
        stop("Zeit abgelaufen");
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
    }

    // Der Puffer reicht fuer kMaxSeconds, die Zeitschranke in poll_key() greift
    // normalerweise zuerst. Trotzdem abfangen, statt still zu verwerfen.
    if (fill_ >= capacity_) {
        stop("Puffer voll");
    }
}
