#include "verbindung.h"

#include <stdio.h>
#include <string.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "weg";

esp_err_t Verbindung::begin(const char *url, const char *waerm, const char *key,
                            const char *accept, int wartems)
{
    if (url == nullptr || key == nullptr || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    url_    = url;
    waerm_  = waerm;
    accept_ = accept;
    warte_  = wartems;

    esp_http_client_config_t cfg = {};
    cfg.url                 = url_;
    cfg.method              = HTTP_METHOD_POST;
    cfg.crt_bundle_attach   = esp_crt_bundle_attach;
    cfg.timeout_ms          = warte_;
    cfg.buffer_size         = 1024;
    cfg.buffer_size_tx      = 2048;
    cfg.keep_alive_enable   = true;

    c_ = esp_http_client_init(&cfg);
    if (c_ == nullptr) return ESP_FAIL;

    // Die Kopfzeilen bleiben ueber alle Anfragen hinweg stehen; esp_http_client
    // raeumt sie nur auf Zuruf weg.
    snprintf(auth_, sizeof(auth_), "Bearer %s", key);
    esp_http_client_set_header(c_, "Content-Type", "application/json");
    esp_http_client_set_header(c_, "Authorization", auth_);
    if (accept_ != nullptr) esp_http_client_set_header(c_, "Accept", accept_);

    return ESP_OK;
}

void Verbindung::vorwaermen()
{
    if (c_ == nullptr || offen_ || waerm_ == nullptr) return;

    const int64_t t0 = esp_timer_get_time();
    if (t0 < sperre_bis_) return;

    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        < kMindestHeap) {
        sperre_bis_ = t0 + 2000000;
        return;
    }

    esp_http_client_set_url(c_, waerm_);
    esp_http_client_set_method(c_, HTTP_METHOD_GET);

    bool gut = false;
    if (esp_http_client_open(c_, 0) == ESP_OK
        && esp_http_client_fetch_headers(c_) >= 0) {
        // Der Rumpf interessiert nicht, aber er muss weg: eine halb gelesene
        // Antwort macht die Verbindung fuer die naechste Anfrage unbrauchbar.
        char weg[128];
        while (esp_http_client_read(c_, weg, sizeof(weg)) > 0) { }
        gut = esp_http_client_is_complete_data_received(c_)
              && esp_http_client_is_persistent_connection(c_);
    }

    if (gut) {
        offen_ = true;
        ESP_LOGI(TAG, "Vorgewaermt in %d ms.",
                 (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        esp_http_client_close(c_);
        offen_      = false;
        sperre_bis_ = esp_timer_get_time() + 5000000;
    }
}

int Verbindung::senden(const char *rumpf, int laenge)
{
    if (c_ == nullptr || rumpf == nullptr) return 0;

    // Zwei Durchgaenge, aber nur, wenn der erste auf einer wiederverwendeten
    // Verbindung gescheitert ist. Eine frisch aufgebaute Verbindung, die
    // nicht traegt, traegt auch beim zweiten Mal nicht.
    for (int versuch = 0; versuch < 2; versuch++) {
        const bool war_offen = offen_;

        esp_http_client_set_url(c_, url_);
        esp_http_client_set_method(c_, HTTP_METHOD_POST);

        esp_err_t err = esp_http_client_open(c_, laenge);
        if (err == ESP_OK && esp_http_client_write(c_, rumpf, laenge) != laenge) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK && esp_http_client_fetch_headers(c_) < 0) {
            err = ESP_FAIL;
        }

        if (err == ESP_OK) {
            offen_ = true;
            return esp_http_client_get_status_code(c_);
        }

        esp_http_client_close(c_);
        offen_ = false;

        if (!war_offen) {
            ESP_LOGE(TAG, "Verbindung fehlgeschlagen: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGW(TAG, "Stehende Verbindung war tot, neuer Aufbau.");
    }
    return 0;
}

int Verbindung::lesen(char *dst, int max)
{
    if (c_ == nullptr) return -1;
    return esp_http_client_read(c_, dst, max);
}

bool Verbindung::vollstaendig() const
{
    if (c_ == nullptr) return false;
    return esp_http_client_is_complete_data_received(c_);
}

void Verbindung::leerlesen(int frist_ms)
{
    if (c_ == nullptr || vollstaendig()) return;

    esp_http_client_set_timeout_ms(c_, frist_ms);
    char weg[128];
    for (int i = 0; i < 8; i++) {
        if (esp_http_client_read(c_, weg, sizeof(weg)) <= 0) break;
        if (vollstaendig()) break;
    }
    esp_http_client_set_timeout_ms(c_, warte_);
}

void Verbindung::abschluss(bool sauber)
{
    if (c_ == nullptr) return;

    if (sauber && vollstaendig() && esp_http_client_is_persistent_connection(c_)) {
        offen_ = true;   // stehen lassen, der naechste open() spart den Aufbau
        return;
    }

    esp_http_client_close(c_);
    offen_ = false;
}
