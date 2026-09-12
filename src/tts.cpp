#include "tts.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "net.h"

static const char *TAG = "tts";

static const char *kUrl = "https://api.openai.com/v1/audio/speech";

// Der Hinweis an das Modell steuert Betonung und Tempo, nicht den Inhalt.
// Deutsch ausdruecklich genannt, weil der Text zwar deutsch ist, die
// Aussprache aber sonst gelegentlich ins Englische kippt — gerade bei kurzen
// Antworten, in denen zu wenig Sprache steht, um es zu erkennen.
static const char *kAnweisung =
    "Sprich Deutsch, ruhig und natuerlich, in normalem Tempo.";

// Grosszuegig: die Verbindung steht waehrend der gesamten Ausgabe offen.
static const int kWarteMs = 30000;

// Stille zum Schluss. Der Verstaerker wird danach abgeschaltet, und ein
// Wandler, der mitten im Signal stehenbleibt, tut das mit einem Knacks.
static const size_t kAusklangFrames = 24000 / 50;   // 20 ms

esp_err_t Tts::begin(Chat *quelle, SpeakerOutput *aus, Listener *taste,
                     const char *key, const char *model, const char *voice)
{
    quelle_ = quelle;
    aus_    = aus;
    taste_  = taste;
    key_    = key;
    model_  = (model != nullptr && model[0] != '\0') ? model : "gpt-4o-mini-tts";
    voice_  = (voice != nullptr && voice[0] != '\0') ? voice : "alloy";

    if (quelle == nullptr || aus == nullptr || !aus->ready()
        || key == nullptr || key[0] == '\0') {
        phase_ = (int32_t)Phase::Aus;
        return ESP_ERR_INVALID_ARG;
    }

    lese_ = (uint8_t *)heap_caps_malloc(kLeseBytes, MALLOC_CAP_SPIRAM);
    ring_ = (int16_t *)heap_caps_malloc(kRingFrames * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    if (lese_ == nullptr || ring_ == nullptr) {
        phase_ = (int32_t)Phase::Fehler;
        return ESP_ERR_NO_MEM;
    }

    // Was vor dem Start schon dastand, wird nicht nachtraeglich vorgelesen.
    gesehen_ = quelle_->antwort_seq();
    phase_   = (int32_t)Phase::Bereit;

    // Der Spieler zuerst: er muss stehen, bevor der Holer ihm Arbeit gibt.
    // Hoehere Prioritaet als der Holer, weil ein verspaeteter Schreibvorgang
    // hoerbar ist und ein verspaeteter Lesevorgang nur den Ring leert.
    if (xTaskCreatePinnedToCore(spiel_trampolin, "tts_spiel", 4096, this, 4,
                                nullptr, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(hol_trampolin, "tts_hol", 8192, this, 3,
                                nullptr, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Tts::hol_trampolin(void *self)   { ((Tts *)self)->holen(); }
void Tts::spiel_trampolin(void *self) { ((Tts *)self)->spielen(); }

const char *Tts::phase_text() const
{
    switch ((Phase)phase_) {
    case Phase::Aus:     return "STUMM";
    case Phase::Bereit:  return "BEREIT";
    case Phase::Holt:    return "HOLT STIMME";
    case Phase::Spricht: return "SPRICHT";
    default:             return "AUSGABE GESTOERT";
    }
}

// --- Ring -----------------------------------------------------------------

void Tts::ring_schreiben(const int16_t *pcm, size_t frames)
{
    while (frames > 0 && !abbruch_) {
        const uint32_t frei = kRingFrames - (uint32_t)ring_belegt();
        if (frei == 0) {
            // Voll heisst: die Gegenseite liefert schneller, als gesprochen
            // wird. Warten ist hier genau richtig — es bremst den Holer auf
            // die Geschwindigkeit des Lautsprechers.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const uint32_t off      = kopf_ % kRingFrames;
        const uint32_t bis_ende = kRingFrames - off;

        uint32_t n = (frames < frei) ? (uint32_t)frames : frei;
        if (n > bis_ende) n = bis_ende;

        memcpy(ring_ + off, pcm, n * sizeof(int16_t));
        kopf_ += n;
        pcm   += n;
        frames -= n;
    }
}

// --- Task 2: Ring in den Lautsprecher -------------------------------------

void Tts::spielen()
{
    static int16_t stille[kAusklangFrames] = {};

    while (true) {
        if (!auftrag_) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Vorlauf abwarten — aber nicht ueber das Ende hinaus: eine kurze
        // Antwort ist schon ganz da, bevor zwei Sekunden zusammenkommen.
        while (!fertig_ && !abbruch_ && ring_belegt() < kVorlaufFrames) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        bool laeuft = false;

        while (!abbruch_) {
            const size_t belegt = ring_belegt();
            if (belegt == 0) {
                if (fertig_) break;
                // Der Ring ist leer, obwohl noch etwas kommt: genau der
                // Fall, den der Vorlauf verhindern soll. Kurz warten und
                // weiter — hoerbar ist es trotzdem.
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            if (!laeuft) {
                if (aus_->start() != ESP_OK) break;
                laeuft    = true;
                first_ms_ = (int32_t)((esp_timer_get_time() - start_us_) / 1000);
                phase_    = (int32_t)Phase::Spricht;
            }

            const uint32_t off      = schwanz_ % kRingFrames;
            const uint32_t bis_ende = kRingFrames - off;

            uint32_t n = (belegt < SpeakerOutput::kMaxFrames)
                             ? (uint32_t)belegt
                             : SpeakerOutput::kMaxFrames;
            if (n > bis_ende) n = bis_ende;

            if (aus_->write_mono(ring_ + off, n) != ESP_OK) {
                ESP_LOGW(TAG, "Ausgabe abgebrochen (Schreibfehler).");
                break;
            }
            schwanz_ += n;
            spoken_ms_ += (int32_t)((int64_t)n * 1000 / kRate);

            // Wer die Taste drueckt, will sprechen und nicht zuhoeren.
            if (taste_ != nullptr && taste_->listening()) {
                ESP_LOGI(TAG, "Ausgabe abgebrochen (Taste gedrueckt).");
                abbruch_ = 1;
                break;
            }
        }

        if (laeuft) {
            aus_->write_mono(stille, kAusklangFrames);
            aus_->stop();
        }

        // Der Holer wartet darauf, dass der Auftrag abgearbeitet ist; erst
        // danach darf er den Ring fuer die naechste Antwort zuruecksetzen.
        auftrag_ = 0;
    }
}

// --- Task 1: Netz in den Ring ---------------------------------------------

void Tts::sprechen(const char *text)
{
    phase_     = (int32_t)Phase::Holt;
    first_ms_  = 0;
    spoken_ms_ = 0;
    hat_rest_  = false;
    kopf_      = 0;
    schwanz_   = 0;
    fertig_    = 0;
    abbruch_   = 0;

    const int64_t t0 = esp_timer_get_time();
    start_us_        = t0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model_);
    cJSON_AddStringToObject(root, "voice", voice_);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "instructions", kAnweisung);
    cJSON_AddStringToObject(root, "response_format", "pcm");

    char *rumpf = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rumpf == nullptr) {
        phase_ = (int32_t)Phase::Fehler;
        return;
    }

    esp_http_client_config_t cfg = {};
    cfg.url               = kUrl;
    cfg.method            = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = kWarteMs;
    cfg.buffer_size       = 1024;
    cfg.buffer_size_tx    = 2048;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == nullptr) {
        free(rumpf);
        phase_ = (int32_t)Phase::Fehler;
        return;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", key_);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    const int laenge = (int)strlen(rumpf);
    esp_err_t err    = esp_http_client_open(c, laenge);

    if (err == ESP_OK && esp_http_client_write(c, rumpf, laenge) != laenge) {
        err = ESP_FAIL;
    }
    free(rumpf);

    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;

    const int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : 0;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Verbindung fehlgeschlagen: %s", esp_err_to_name(err));
        phase_ = (int32_t)Phase::Fehler;

    } else if (status != 200) {
        // Der Fehlerrumpf ist JSON mit Klartext darin — bei unbekanntem
        // Modell oder unbekannter Stimme steht dort genau, was fehlt.
        const int r = esp_http_client_read(c, (char *)lese_, kLeseBytes - 1);
        lese_[(r > 0) ? r : 0] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %s", status, (const char *)lese_);
        phase_ = (int32_t)Phase::Fehler;

    } else {
        auftrag_ = 1;   // ab hier darf der Spieler mitlesen

        size_t gesamt_f = 0;

        while (!abbruch_) {
            // Ein halbes Wort vom letzten Block steht vorn, der Rest wird
            // dahintergelesen.
            const size_t vorn = hat_rest_ ? 1 : 0;
            if (vorn) lese_[0] = rest_;

            const int r = esp_http_client_read(c, (char *)lese_ + vorn,
                                               (int)(kLeseBytes - vorn));
            if (r <= 0) break;

            size_t bytes = (size_t)r + vorn;

            hat_rest_ = (bytes % 2) != 0;
            if (hat_rest_) {
                bytes--;
                rest_ = lese_[bytes];
            }
            if (bytes == 0) continue;

            ring_schreiben((const int16_t *)lese_, bytes / 2);
            gesamt_f += bytes / 2;
        }

        fertig_ = 1;

        // Warten, bis der Ring leer gespielt ist. Ohne dieses Warten stuende
        // die naechste Antwort schon im Ring, waehrend die vorige noch
        // laeuft — und beide klaengen ineinander.
        while (auftrag_) vTaskDelay(pdMS_TO_TICKS(10));

        if (gesamt_f > 0) {
            ESP_LOGI(TAG, "Gesprochen: %d ms Ton, erster Ton nach %d ms.",
                     (int)((int64_t)gesamt_f * 1000 / kRate), (int)first_ms_);
            phase_ = (int32_t)Phase::Bereit;
        } else {
            ESP_LOGW(TAG, "Kein Ton empfangen.");
            phase_ = (int32_t)Phase::Fehler;
        }
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

void Tts::holen()
{
    while (true) {
        const uint32_t jetzt = quelle_->antwort_seq();

        if (jetzt != gesehen_) {
            gesehen_ = jetzt;

            char text[Chat::kMaxAntwort];
            quelle_->copy_antwort(text, sizeof(text));

            if (text[0] == '\0') {
                // Nichts zu sagen ist kein Fehler.
            } else if (!net::connected()) {
                ESP_LOGW(TAG, "Kein Netz, Antwort bleibt stumm.");
                phase_ = (int32_t)Phase::Fehler;
            } else {
                sprechen(text);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
