#include "stt.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include "net.h"

static const char *TAG = "stt";

static const char *kUri = "wss://api.openai.com/v1/realtime?intent=transcription";

// Base64 blaeht auf 4/3 auf; plus Abschluss und etwas Luft fuer die Huelle.
static const size_t kB64Bytes  = ((2400 * 2 + 2) / 3) * 4 + 8;
static const size_t kJsonBytes = kB64Bytes + 128;

// Wie lange nach dem Loslassen auf den Endtext gewartet wird, bevor die
// Verbindung ohnehin geschlossen wird.
static const int64_t kFinalWaitUs = 8 * 1000000;

// Wie lange nach dem Loslassen noch auf eine Sitzung gewartet wird, die beim
// Tastendruck aufgebaut wurde und bis zum Loslassen nicht fertig geworden
// ist. Gemessen dauert der Aufbau rund 1,9 s — 0,8 s TLS, der Rest
// WebSocket-Aufstieg und die Antwort auf transcription_session.update. Wer
// kurz drueckt, laesst frueher los als das; die Aufnahme liegt dann
// vollstaendig im PSRAM und wird nachgereicht, statt weggeworfen zu werden.
static const int64_t kSitzungWarteUs = 9 * 1000000;

// Nach einem gescheiterten oder verlorenen Aufbau nicht sofort wieder
// anklopfen. Ein falscher Schluessel soll nicht im Sekundentakt gegen die
// Gegenstelle laufen.
static const int64_t kAufbauPauseUs = 10 * 1000000;

// "language" allein hat nicht gereicht: "Okay, cool" kam einmal als
// koreanische Schrift zurueck, obwohl "de" gesetzt war. Kurze Aeusserungen mit
// englischen Lehnwoertern geben dem Modell zu wenig, um die Sprache selbst zu
// sehen. Der Hinweis gibt ihm den Zusammenhang, den die Aufnahme nicht hat.
static const char *kHinweis =
    "Gespraech mit einem Sprachassistenten auf Deutsch, gelegentlich mit "
    "englischen Woertern wie okay oder cool.";

// Die Kehrseite: bekommt das Modell fast nur Rauschen, gibt es den Hinweis
// selbst als Text zurueck. Beobachtet nach einem Geraeusch in der Nachfrage —
// und der Chat beantwortete ihn brav als Frage. Erkannt an einem Stueck, das
// in keiner echten Frage vorkommt; Satzzeichen und Gross/klein darf das
// Modell dabei aendern.
static const char *kHinweisKern = "prachassistenten auf Deutsch";

esp_err_t Stt::begin(Listener *quelle, uint32_t sample_rate,
                     const char *key, const char *model, const char *language)
{
    quelle_ = quelle;
    rate_   = sample_rate;
    key_    = key;
    model_  = model;
    lang_   = language;

    text_lock_ = xSemaphoreCreateMutex();
    if (text_lock_ == nullptr) return ESP_ERR_NO_MEM;
    senden_lock_ = xSemaphoreCreateRecursiveMutex();
    if (senden_lock_ == nullptr) return ESP_ERR_NO_MEM;

    if (key == nullptr || key[0] == '\0') {
        phase_ = (int32_t)Phase::Aus;
        return ESP_ERR_INVALID_ARG;
    }

    b64_  = (uint8_t *)heap_caps_malloc(kB64Bytes, MALLOC_CAP_SPIRAM);
    json_ = (char *)heap_caps_malloc(kJsonBytes, MALLOC_CAP_SPIRAM);
    if (b64_ == nullptr || json_ == nullptr) {
        phase_ = (int32_t)Phase::Fehler;
        return ESP_ERR_NO_MEM;
    }

    phase_ = (int32_t)Phase::Bereit;

    // Eigener Task: weder die Aufnahme noch die Anzeige duerfen auf das Netz
    // warten. Kern 0, damit der Anzeigetask auf Kern 1 ungestoert bleibt.
    if (xTaskCreatePinnedToCore(task_trampolin, "stt", 6144, this, 3, nullptr, 0)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Stt::task_trampolin(void *self)
{
    ((Stt *)self)->run();
}

const char *Stt::phase_text() const
{
    switch ((Phase)phase_) {
        case Phase::Aus:       return net::connected() ? "KEIN SCHLUESSEL" : "OFFLINE";
        case Phase::Bereit:    return "BEREIT";
        case Phase::Verbindet: return "VERBINDET";
        case Phase::Hoert:     return "HOERT ZU";
        case Phase::Wartet:    return "SCHLIESST AB";
        default:               return "FEHLER";
    }
}

void Stt::copy_text(char *out, size_t n) const
{
    if (out == nullptr || n == 0) return;
    out[0] = '\0';
    if (text_lock_ == nullptr) return;

    if (xSemaphoreTake(text_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        strncpy(out, text_, n - 1);
        out[n - 1] = '\0';
        xSemaphoreGive(text_lock_);
    }
}

void Stt::copy_final(char *out, size_t n) const
{
    if (out == nullptr || n == 0) return;

    xSemaphoreTake(text_lock_, portMAX_DELAY);
    snprintf(out, n, "%s", final_);
    xSemaphoreGive(text_lock_);
}

void Stt::set_text(const char *s)
{
    xSemaphoreTake(text_lock_, portMAX_DELAY);
    text_len_ = 0;
    text_[0]  = '\0';
    if (s != nullptr) {
        strncpy(text_, s, kMaxText - 1);
        text_[kMaxText - 1] = '\0';
        text_len_           = strlen(text_);
    }
    xSemaphoreGive(text_lock_);
}

void Stt::append_text(const char *s)
{
    if (s == nullptr || *s == '\0') return;

    xSemaphoreTake(text_lock_, portMAX_DELAY);
    const size_t frei = (kMaxText - 1) - text_len_;
    if (frei > 0) {
        strncat(text_, s, frei);
        text_len_ = strlen(text_);
    }
    xSemaphoreGive(text_lock_);
}

// --- WebSocket ------------------------------------------------------------

void Stt::ws_event(void *args, esp_event_base_t, int32_t id, void *event_data)
{
    Stt        *self = (Stt *)args;
    const auto *d    = (const esp_websocket_event_data_t *)event_data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->verbunden_ = 1;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            // Wartet, bis ein laufender Schreibversuch durch ist, siehe
            // send_json(). Danach sieht jeder weitere verbunden_ == 0.
            //
            // Rekursiv, weil das Ereignis auch aus dem Schreibversuch selbst
            // kommen kann: scheitert er, baut der Client die Verbindung im
            // Task des Schreibers ab und meldet es dort. Mit einer einfachen
            // Sperre wartete der Task dann auf sich selbst — so stand die
            // Erkennung einmal fuer immer still, und die Anzeige blieb bei
            // der Aufnahme stehen.
            xSemaphoreTakeRecursive(self->senden_lock_, portMAX_DELAY);
            self->verbunden_  = 0;
            xSemaphoreGiveRecursive(self->senden_lock_);
            self->sitzung_ok_ = 0;
            self->ws_fehler_  = 1;
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WebSocket-Fehler.");
            self->ws_fehler_ = 1;
            break;

        case WEBSOCKET_EVENT_DATA:
            // op_code 1 ist Text. Lange Nachrichten kommen in Stuecken; nur
            // ein vollstaendig angekommenes Stueck laesst sich auswerten.
            if (d != nullptr && d->op_code == 0x01 && d->data_len > 0
                && d->payload_offset == 0 && d->data_len == d->payload_len) {
                self->on_message(d->data_ptr, d->data_len);
            } else if (d != nullptr && d->op_code == 0x01
                       && d->payload_len > d->data_len) {
                // Sollte bei diesen Nachrichten nicht vorkommen; wenn doch,
                // lieber melden als still einen halben Text auswerten.
                ESP_LOGW(TAG, "Nachricht zerteilt (%d von %d), verworfen.",
                         d->data_len, d->payload_len);
            }
            break;

        default:
            break;
    }
}

void Stt::on_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == nullptr) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring != nullptr) {
        const char *t = type->valuestring;

        if (strcmp(t, "conversation.item.input_audio_transcription.delta") == 0) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "delta");
            if (cJSON_IsString(d)) append_text(d->valuestring);

        } else if (strcmp(t, "conversation.item.input_audio_transcription.completed") == 0) {
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(root, "transcript");
            const bool echo = cJSON_IsString(tr) && tr->valuestring != nullptr
                              && strstr(tr->valuestring, kHinweisKern) != nullptr;
            if (echo) {
                ESP_LOGW(TAG, "Endtext ist der Hinweis selbst, verworfen: %s",
                         tr->valuestring);
                set_text("");
            } else if (cJSON_IsString(tr) && pruefung_ && pruefer_ != nullptr
                       && !pruefer_(tr->valuestring)) {
                ESP_LOGI(TAG, "Pruefaufnahme verworfen, die Antwort laeuft weiter: %s",
                         tr->valuestring);
                set_text("");
            } else if (cJSON_IsString(tr)) {
                set_text(tr->valuestring);
            }

            // Erst den Endtext sichern, dann den Zaehler hochsetzen: wer auf
            // den Zaehler wartet, findet den Text dann in jedem Fall schon
            // vollstaendig vor.
            xSemaphoreTake(text_lock_, portMAX_DELAY);
            snprintf(final_, sizeof(final_), "%s", text_);
            xSemaphoreGive(text_lock_);
            final_seq_++;

            endtext_ = 1;
            if (!echo) {
                ESP_LOGI(TAG, "Endtext: %s",
                         cJSON_IsString(tr) ? tr->valuestring : "(leer)");
            }

        } else if (strcmp(t, "session.updated") == 0
                   || strcmp(t, "transcription_session.updated") == 0) {
            sitzung_ok_ = 1;
            ESP_LOGI(TAG, "Sitzung eingerichtet.");

        } else if (strcmp(t, "error") == 0) {
            const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
            const cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message")
                                   : nullptr;
            ESP_LOGE(TAG, "Dienstfehler: %s",
                     cJSON_IsString(msg) ? msg->valuestring : "(ohne Text)");
            phase_ = (int32_t)Phase::Fehler;
        }
    }

    cJSON_Delete(root);
}

bool Stt::send_json(const char *json)
{
    if (client_ == nullptr) return false;

    // Der Client baut eine Verbindung, die die Gegenseite schliesst, in
    // seinem eigenen Task ab — beim Schliessen durch die Gegenseite sogar
    // ohne seine Sperre. Wer in dem Moment schreibt, schreibt in einen
    // freigegebenen TLS-Kontext: so stuerzte das Geraet ab, als eine
    // Pruefaufnahme Ton schickte (LoadProhibited in ssl_check_ctr_renegotiate,
    // aus send_audio). Das Ereignis dazu kommt vor dem Abbau und im selben
    // Task an, und ws_event() wartet dort auf diese Sperre.
    //
    // Haelt der Client beim Abbau seine eigene Sperre, wartet dieser
    // Schreibversuch darauf und gibt nach der Frist von 2 s auf — dann erst
    // kommt das Ereignis durch. Das kostet im seltenen Fall zwei Sekunden,
    // aber keinen Absturz.
    xSemaphoreTakeRecursive(senden_lock_, portMAX_DELAY);
    int       ret = -1;
    const int len = (int)strlen(json);
    if (verbunden_) {
        ret = esp_websocket_client_send_text(
            (esp_websocket_client_handle_t)client_, json, len, pdMS_TO_TICKS(2000));
    }
    xSemaphoreGiveRecursive(senden_lock_);
    return ret == len;
}

bool Stt::send_config()
{
    // turn_detection bleibt aus: Anfang und Ende bestimmt die Taste, nicht
    // eine Stimmerkennung auf der Gegenseite.
    snprintf(json_, kJsonBytes,
             "{\"type\":\"session.update\",\"session\":{"
             "\"type\":\"transcription\",\"audio\":{\"input\":{"
             "\"format\":{\"type\":\"audio/pcm\",\"rate\":%u},"
             "\"transcription\":{\"model\":\"%s\",\"language\":\"%s\","
             "\"prompt\":\"%s\"},"
             "\"turn_detection\":null}}}}",
             (unsigned)rate_, model_, lang_, kHinweis);
    return send_json(json_);
}

bool Stt::send_audio(const int16_t *pcm, size_t frames)
{
    size_t olen = 0;
    if (mbedtls_base64_encode(b64_, kB64Bytes, &olen,
                              (const unsigned char *)pcm, frames * 2) != 0) {
        return false;
    }
    b64_[olen] = '\0';

    const int n = snprintf(json_, kJsonBytes,
                           "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                           (const char *)b64_);
    if (n <= 0 || (size_t)n >= kJsonBytes) return false;
    return send_json(json_);
}

esp_err_t Stt::open_session()
{
    char header[256];
    // Kein OpenAI-Beta-Header: die Beta-Fassung der Realtime-API ist
    // abgeschaltet, und der Header ist nicht bloss ueberfluessig, sondern
    // der Grund fuer die Ablehnung — "The Realtime Beta API is no longer
    // supported. Please use /v1/realtime for the GA API."
    snprintf(header, sizeof(header), "Authorization: Bearer %s\r\n", key_);

    esp_websocket_client_config_t cfg = {};
    cfg.uri                     = kUri;
    cfg.headers                 = header;
    cfg.crt_bundle_attach       = esp_crt_bundle_attach;
    cfg.buffer_size             = 4096;
    cfg.task_stack              = 6144;

    // Ohne diese Zeile legt esp_websocket_client seinen Task auf Prioritaet 5
    // — ueber den Spieler (4) und ueber alles andere, was hier mit dem Netz
    // redet (3). Waehrend seines TLS-Handschlags ist das eine Sekunde
    // Rechenarbeit, die niemand unterbricht, und er teilt sich mit Mikrofon
    // und Lautsprecher den I2C-Bus. Dessen Sperre ist in der IDF ein
    // Binaersemaphor ohne Prioritaetsvererbung: wer sie haelt und verdraengt
    // wird, haelt sie weiter. So brauchte das Aufwachen des ES7210 statt 50 ms
    // ploetzlich 766, und die ersten drei Silben fehlten.
    cfg.task_prio               = 3;
    cfg.disable_auto_reconnect  = true;
    cfg.network_timeout_ms      = 10000;
    cfg.reconnect_timeout_ms    = 5000;

    client_ = esp_websocket_client_init(&cfg);
    if (client_ == nullptr) return ESP_FAIL;

    esp_websocket_register_events((esp_websocket_client_handle_t)client_,
                                  WEBSOCKET_EVENT_ANY, ws_event, this);

    verbunden_  = 0;
    sitzung_ok_ = 0;
    endtext_    = 0;
    ws_fehler_  = 0;

    return esp_websocket_client_start((esp_websocket_client_handle_t)client_);
}

void Stt::close_session(const char *grund)
{
    if (client_ == nullptr) return;

    ESP_LOGI(TAG, "Sitzung beendet (%s).", grund);
    esp_websocket_client_close((esp_websocket_client_handle_t)client_,
                               pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy((esp_websocket_client_handle_t)client_);
    client_     = nullptr;
    verbunden_  = 0;
    sitzung_ok_ = 0;
    ws_fehler_  = 0;
}

// --- Ablauf ---------------------------------------------------------------

void Stt::run()
{
    bool    war_aktiv = false;
    bool    konfiguriert = false;
    int64_t warte_seit = 0;

    // Die Taste ist los, aber der Ton ist noch nicht drueben.
    bool    abschluss_offen = false;
    int64_t abschluss_frist = 0;

    // Fruehestens dann wieder aufbauen, und seit wann der laufende Aufbau
    // laeuft.
    int64_t naechster_aufbau = 0;
    int64_t aufbau_seit      = 0;

    while (true) {
        const bool aktiv = quelle_->listening();

        // --- Die Sitzung vorhalten ---
        //
        // Der Aufbau kostet 1,9 s, und er kostete bisher mehr als Zeit: er
        // fiel genau in die Aufnahme. Gemessen ueber drei Runden verlor jede
        // 300 bis 900 ms Ton, immer am Anfang, immer waehrend des
        // TLS-Handschlags — wer sofort nach dem Tastendruck sprach, verlor
        // den Satzanfang. Am Aufnahmetask lag es nicht: der wird nie
        // verdraengt, er bekam schlicht keine Daten.
        //
        // Deshalb steht die Sitzung, bevor jemand drueckt, und sie bleibt
        // ueber die Runden hinweg stehen. Faellt sie weg, wird sie hier neu
        // aufgebaut — im Leerlauf, wo es niemanden kostet.
        if (!aktiv && client_ == nullptr && net::connected()
            && phase_ != (int32_t)Phase::Fehler
            && esp_timer_get_time() >= naechster_aufbau) {
            konfiguriert = false;
            if (open_session() == ESP_OK) {
                aufbau_seit = esp_timer_get_time();
            } else {
                naechster_aufbau = esp_timer_get_time() + kAufbauPauseUs;
            }
        }

        if (sitzung_ok_ && aufbau_seit != 0) {
            ESP_LOGI(TAG, "Sitzung steht nach %d ms und bleibt stehen.",
                     (int)((esp_timer_get_time() - aufbau_seit) / 1000));
            aufbau_seit = 0;
        }

        // --- Tastendruck ---
        if (aktiv && !war_aktiv) {
            gesendet_       = 0;
            abschluss_offen = false;
            endtext_        = 0;
            pruefung_       = quelle_->pruefung() ? 1 : 0;
            set_text("");

            if (client_ != nullptr && verbunden_ && sitzung_ok_) {
                // Der Normalfall: es ist nichts aufzubauen. Nur den
                // Eingangspuffer der Gegenseite leeren, falls von einer
                // abgebrochenen Runde noch etwas darin liegt.
                send_json("{\"type\":\"input_audio_buffer.clear\"}");
                konfiguriert = true;
                phase_       = (int32_t)Phase::Hoert;
            } else {
                konfiguriert = false;

                // Wer sofort nachfragt, drueckt die Taste, bevor die vorige
                // Sitzung ihren Endtext hatte. Ohne diese Zeile ueberschriebe
                // open_session() den alten Griff und liesse eine Sitzung
                // offen, deren Ereignisse weiterhin hier hereinkaemen.
                if (client_ != nullptr) close_session("ueberholt");

                if (!net::connected()) {
                    phase_ = (int32_t)Phase::Aus;
                } else if (open_session() == ESP_OK) {
                    aufbau_seit = esp_timer_get_time();
                    phase_      = (int32_t)Phase::Verbindet;
                } else {
                    phase_ = (int32_t)Phase::Fehler;
                }
            }
        }

        // --- Stirbt die Sitzung mitten in der Aufnahme, sofort neu ---
        //
        // Die Aufnahme ist dabei nicht verloren: sie liegt vollstaendig im
        // PSRAM. Frueher lief in diesem Fall die Frist von fuenf Sekunden ab,
        // ohne dass jemand eine neue Sitzung aufgebaut haette, und die ganze
        // Aeusserung verschwand — ohne Antwort und ohne ein Zeichen, dass
        // ueberhaupt etwas angekommen war. Beobachtet, als nebenan eine
        // zweite TLS-Verbindung vorgewaermt wurde und der Schreibversuch auf
        // dieser hier an zu wenig Speicher scheiterte.
        //
        // Der Neuaufbau kostet 1,9 s und passt damit in die Frist; der
        // Nachschub-Block darunter holt den Rueckstand auf, sobald die neue
        // Sitzung steht.
        if ((aktiv || abschluss_offen) && client_ != nullptr && ws_fehler_) {
            close_session("Verbindung weg, neuer Versuch");
            konfiguriert = false;
            gesendet_    = 0;   // die neue Sitzung kennt nichts von vorher
            if (net::connected() && open_session() == ESP_OK) {
                aufbau_seit = esp_timer_get_time();
                phase_      = (int32_t)Phase::Verbindet;
            } else {
                naechster_aufbau = esp_timer_get_time() + kAufbauPauseUs;
                phase_           = (int32_t)Phase::Fehler;
            }
        }

        // --- Sitzung einrichten, sobald die Verbindung steht ---
        if (client_ != nullptr && verbunden_ && !konfiguriert) {
            konfiguriert = send_config();
        }

        // --- Ton nachschicken ---
        // Waehrend des Verbindungsaufbaus laeuft die Aufnahme schon. Der
        // Listener haelt alles im PSRAM, also geht nichts verloren: sobald
        // die Sitzung steht, wird der Rueckstand aufgeholt.
        if (client_ != nullptr && sitzung_ok_ && (aktiv || abschluss_offen)) {
            if (phase_ == (int32_t)Phase::Verbindet) phase_ = (int32_t)Phase::Hoert;

            const int16_t *pcm  = quelle_->samples();
            const size_t   have = aktiv ? quelle_->live_count()
                                        : quelle_->sample_count();

            while (pcm != nullptr && gesendet_ < have) {
                size_t n = have - gesendet_;
                if (n > kChunkFrames) n = kChunkFrames;
                // Solange noch aufgenommen wird, nur volle Stuecke schicken —
                // das haelt die Zahl der Nachrichten klein.
                if (aktiv && n < kChunkFrames) break;

                if (!send_audio(&pcm[gesendet_], n)) break;
                gesendet_ += n;
            }
        }

        // --- Taste los: abschliessen ---
        //
        // Nicht sofort, sondern sobald der Ton tatsaechlich drueben ist. Der
        // Aufbau einer Sitzung dauert rund 1,9 s, und wer kurz nachfragt,
        // laesst vorher los. Frueher endete das hier mit
        //
        //   stt: Sitzung beendet (ohne Sitzung)
        //
        // und die anderthalb Sekunden Aufnahme, die sauber im PSRAM lagen,
        // waren weg — ohne Antwort und ohne ein Zeichen, dass ueberhaupt
        // etwas angekommen war. Der Nachschub-Block darueber holt den
        // Rueckstand ohnehin auf, sobald die Sitzung steht; hier wird nur
        // gewartet, bis er durch ist.
        if (!aktiv && war_aktiv) {
            // Eine leere Aufnahme hat nichts abzuschliessen. Solange eine
            // Taste die Aufnahme startete, gab es sie praktisch nicht — wer
            // drueckt, sagt auch etwas. Das Weckwort loest dagegen von selbst
            // aus, und es loest gelegentlich ins Leere aus: dann steht die
            // Aufnahme drei Sekunden offen und endet ohne ein Wort darin. Die
            // Gegenseite beantwortet einen Puffer unter 100 ms mit einem
            // Fehler, und der saehe im Log aus, als sei die Erkennung gestoert.
            const size_t haben = quelle_->sample_count();
            if (haben < (size_t)rate_ / 5) {
                ESP_LOGI(TAG, "Nichts gesagt (%u Frames) — nichts zu erkennen.",
                         (unsigned)haben);
                if (client_ != nullptr && sitzung_ok_) {
                    send_json("{\"type\":\"input_audio_buffer.clear\"}");
                }
                gesendet_ = 0;
                phase_    = (int32_t)Phase::Bereit;
            } else {
                abschluss_offen = true;
                abschluss_frist = esp_timer_get_time() + kSitzungWarteUs;
            }
        }

        if (abschluss_offen) {
            const bool alles_drueben = client_ != nullptr && sitzung_ok_
                                       && gesendet_ >= quelle_->sample_count();
            if (alles_drueben) {
                send_json("{\"type\":\"input_audio_buffer.commit\"}");
                phase_          = (int32_t)Phase::Wartet;
                warte_seit      = esp_timer_get_time();
                abschluss_offen = false;
            } else if (client_ == nullptr
                       || esp_timer_get_time() > abschluss_frist) {
                close_session("ohne Sitzung");
                naechster_aufbau = esp_timer_get_time() + kAufbauPauseUs;
                phase_           = (int32_t)Phase::Bereit;
                abschluss_offen  = false;
            }
        }

        // --- Endtext abwarten ---
        if (phase_ == (int32_t)Phase::Wartet) {
            const bool zu_lang = (esp_timer_get_time() - warte_seit) > kFinalWaitUs;
            if (endtext_) {
                // Nicht schliessen. Die Sitzung nimmt die naechste Aeusserung
                // auf derselben Verbindung an, und genau darum geht es.
                phase_ = (int32_t)Phase::Bereit;
            } else if (zu_lang || !verbunden_) {
                close_session(zu_lang ? "Zeit abgelaufen" : "Verbindung weg");
                naechster_aufbau = esp_timer_get_time() + kAufbauPauseUs;
                phase_           = (int32_t)Phase::Bereit;
            }
        }

        war_aktiv = aktiv;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
