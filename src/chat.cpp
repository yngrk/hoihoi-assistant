#include "chat.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "net.h"

static const char *TAG = "chat";

static const char *kUrl = "https://api.openai.com/v1/chat/completions";

// Der Systemhinweis ist hier kein Beiwerk, sondern Geraetekunde: der Font
// kennt ASCII plus ae, oe, ue und ss, sonst nichts. Eine Antwort mit
// Aufzaehlungszeichen, Anfuehrungsstrichen aus dem Schriftsatz oder einem
// Emoji waere auf diesem Display eine Reihe Fragezeichen. Und drei Saetze,
// weil auf das obere Band bei Schriftgroesse 2 rund 400 Zeichen passen.
static const char *kSystem =
    "Du bist HoiHoi, ein Sprachassistent auf einem kleinen Geraet mit einem "
    "schwarzweissen Display von 400 mal 300 Pixeln. Antworte auf Deutsch, "
    "kurz und in ganzen Saetzen, hoechstens drei. Keine Aufzaehlungen, keine "
    "Ueberschriften, keine Emojis, keine Sternchen und keine Sonderzeichen "
    "ausser deutschen Satzzeichen und Umlauten - der Zeichensatz des Geraets "
    "kann nichts anderes darstellen. Weisst du etwas nicht, sage das in "
    "einem Satz statt zu raten.";

// Reicht fuer den Systemhinweis, drei Wechsel Verlauf und die neue Frage.
static const int kAntwortWarteMs = 30000;

esp_err_t Chat::begin(Stt *quelle, const char *key, const char *model)
{
    quelle_ = quelle;
    key_    = key;
    model_  = (model != nullptr && model[0] != '\0') ? model : "gpt-5.6-luna";

    lock_ = xSemaphoreCreateMutex();
    if (lock_ == nullptr) return ESP_ERR_NO_MEM;

    if (quelle == nullptr || key == nullptr || key[0] == '\0') {
        phase_ = (int32_t)Phase::Aus;
        return ESP_ERR_INVALID_ARG;
    }

    lese_  = (char *)heap_caps_malloc(kLeseBytes, MALLOC_CAP_SPIRAM);
    zeile_ = (char *)heap_caps_malloc(kZeileBytes, MALLOC_CAP_SPIRAM);
    if (lese_ == nullptr || zeile_ == nullptr) {
        phase_ = (int32_t)Phase::Fehler;
        return ESP_ERR_NO_MEM;
    }

    // Was vor dem Start schon im Stt stand, ist keine neue Aeusserung. Ohne
    // diese Zeile beantwortete das Geraet nach jedem Neustart die letzte
    // Frage von vorher noch einmal.
    // Vorwaerm-Adresse aus dem Modellnamen: derselbe Host, eine kleine
    // Antwort, kein Verbrauch. Selbst ein unbekanntes Modell ist recht — eine
    // 404 haelt die Verbindung genauso offen wie eine 200, und gebraucht wird
    // hier nur der Socket darunter.
    snprintf(waerm_, sizeof(waerm_), "https://api.openai.com/v1/models/%s", model_);

    const esp_err_t werr = weg_.begin(kUrl, waerm_, key_, "text/event-stream",
                                      kAntwortWarteMs);
    if (werr != ESP_OK) {
        phase_ = (int32_t)Phase::Fehler;
        return werr;
    }

    gesehen_ = quelle_->final_seq();
    phase_   = (int32_t)Phase::Bereit;

    // Kern 0 wie der Stt-Task: Kern 1 bleibt der Anzeige und der Aufnahme.
    // 8192 Byte, weil in diesem Task der TLS-Handschlag stattfindet — im
    // PSRAM, siehe Stt::begin().
    if (xTaskCreatePinnedToCoreWithCaps(task_trampolin, "chat", 8192, this, 3, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Chat::task_trampolin(void *self)
{
    ((Chat *)self)->run();
}

const char *Chat::phase_text() const
{
    switch ((Phase)phase_) {
    case Phase::Aus:       return "CHAT AUS";
    case Phase::Bereit:    return "BEREIT";
    case Phase::Fragt:     return "FRAGT NACH";
    case Phase::Antwortet: return "ANTWORTET";
    default:               return "CHAT GESTOERT";
    }
}

void Chat::copy_frage(char *out, size_t n) const
{
    if (out == nullptr || n == 0) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    snprintf(out, n, "%s", frage_);
    xSemaphoreGive(lock_);
}

void Chat::copy_antwort(char *out, size_t n) const
{
    if (out == nullptr || n == 0) return;
    xSemaphoreTake(lock_, portMAX_DELAY);
    snprintf(out, n, "%s", antwort_);
    xSemaphoreGive(lock_);
}

void Chat::set_frage(const char *s)
{
    xSemaphoreTake(lock_, portMAX_DELAY);
    snprintf(frage_, sizeof(frage_), "%s", (s != nullptr) ? s : "");
    antwort_[0]  = '\0';
    antwort_len_ = 0;
    fertig_bis_  = 0;
    xSemaphoreGive(lock_);

    // Erst danach hochzaehlen: wer auf den Wechsel wartet, soll den neuen
    // Stand schon vorfinden und nicht den halben alten.
    runde_seq_++;
}

size_t Chat::fertig_bis() const
{
    xSemaphoreTake(lock_, portMAX_DELAY);
    const size_t n = fertig_bis_;
    xSemaphoreGive(lock_);
    return n;
}

void Chat::ausschnitt(size_t von, size_t bis, char *out, size_t n) const
{
    if (out == nullptr || n == 0) return;
    out[0] = '\0';

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (bis > antwort_len_) bis = antwort_len_;
    if (von < bis) {
        size_t m = bis - von;
        if (m > n - 1) m = n - 1;
        memcpy(out, antwort_ + von, m);
        out[m] = '\0';
    }
    xSemaphoreGive(lock_);
}

// Satzgrenzen der laufenden Antwort. Laeuft unter lock_.
//
// Ein Satzzeichen zaehlt nur dann als Ende, wenn ein Leerzeichen folgt — und
// das zuletzt empfangene Zeichen zaehlt nie, denn was danach kommt, weiss man
// noch nicht. So bleibt der Punkt in "3. Mai" kein Satzende, solange die
// Ziffer noch allein dasteht.
void Chat::grenzen_nachfuehren(bool schluss)
{
    if (schluss) {
        fertig_bis_ = antwort_len_;
        return;
    }

    for (size_t i = fertig_bis_; i + 1 < antwort_len_; i++) {
        const char c = antwort_[i];
        if (c != '.' && c != '!' && c != '?') continue;

        const char n = antwort_[i + 1];
        if (n != ' ' && n != '\n') continue;
        if (i + 1 - fertig_bis_ < kMinSatz) continue;

        fertig_bis_ = i + 1;
    }

    if (fertig_bis_ > 0 && satz1_ms_ == 0 && runde_us_ != 0) {
        satz1_ms_ = (int32_t)((esp_timer_get_time() - runde_us_) / 1000);
    }
}

void Chat::append_antwort(const char *s)
{
    if (s == nullptr || s[0] == '\0') return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    const size_t platz = sizeof(antwort_) - 1 - antwort_len_;
    if (platz > 0) {
        const size_t n = strnlen(s, platz);
        memcpy(antwort_ + antwort_len_, s, n);
        antwort_len_ += n;
        antwort_[antwort_len_] = '\0';
        grenzen_nachfuehren(false);
    }
    xSemaphoreGive(lock_);
}

void Chat::verlauf_anfuegen(const char *frage, const char *antwort)
{
    if (frage == nullptr || antwort == nullptr || antwort[0] == '\0') return;

    if (hist_n_ >= kVerlauf) {
        // Der aelteste Wechsel faellt heraus, der Rest rutscht auf.
        for (int i = 0; i + 1 < kVerlauf; i++) {
            memcpy(hist_frage_[i], hist_frage_[i + 1], sizeof(hist_frage_[0]));
            memcpy(hist_antwort_[i], hist_antwort_[i + 1], sizeof(hist_antwort_[0]));
        }
        hist_n_ = kVerlauf - 1;
    }

    snprintf(hist_frage_[hist_n_], sizeof(hist_frage_[0]), "%s", frage);
    snprintf(hist_antwort_[hist_n_], sizeof(hist_antwort_[0]), "%s", antwort);
    hist_n_++;
}

// --- Server-Sent Events ---------------------------------------------------
//
// Die Antwort kommt als Strom von Zeilen der Form "data: {...}", eine je
// Textstueck, abgeschlossen mit "data: [DONE]". Die Stuecke passen nicht
// zwangslaeufig zu den TCP-Paketen, deshalb wird zeilenweise gesammelt und
// nicht paketweise ausgewertet.

void Chat::sse_feed(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        const char c = data[i];

        if (c == '\n') {
            zeile_[zeile_n_] = '\0';
            sse_line(zeile_);
            zeile_n_ = 0;
            continue;
        }
        if (c == '\r') continue;

        // Eine Zeile laenger als der Puffer kann nur eine kaputte sein; sie
        // wegzuwerfen ist besser, als den Rest des Stroms zu verschieben.
        if (zeile_n_ + 1 >= kZeileBytes) { zeile_n_ = 0; continue; }
        zeile_[zeile_n_++] = c;
    }
}

void Chat::sse_line(const char *line)
{
    if (strncmp(line, "data:", 5) != 0) return;

    const char *nutz = line + 5;
    while (*nutz == ' ') nutz++;

    if (strcmp(nutz, "[DONE]") == 0) { sse_done_ = true; return; }

    cJSON *root = cJSON_Parse(nutz);
    if (root == nullptr) return;

    const cJSON *fehler = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(fehler)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(fehler, "message");
        ESP_LOGE(TAG, "Dienstfehler: %s",
                 cJSON_IsString(msg) ? msg->valuestring : "(ohne Text)");
        phase_ = (int32_t)Phase::Fehler;
        cJSON_Delete(root);
        return;
    }

    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *c0 = cJSON_IsArray(ch) ? cJSON_GetArrayItem(ch, 0) : nullptr;
    const cJSON *dl = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "delta") : nullptr;
    const cJSON *ct = dl ? cJSON_GetObjectItemCaseSensitive(dl, "content") : nullptr;

    if (cJSON_IsString(ct) && ct->valuestring != nullptr) {
        if (phase_ == (int32_t)Phase::Fragt) phase_ = (int32_t)Phase::Antwortet;
        append_antwort(ct->valuestring);
        snprintf(letztes_, sizeof(letztes_), "%s", ct->valuestring);
    }

    cJSON_Delete(root);
}

// --- Eine Runde -----------------------------------------------------------

void Chat::frage_stellen(const char *frage)
{
    set_frage(frage);
    phase_     = (int32_t)Phase::Fragt;
    sse_done_  = false;
    letztes_[0] = '\0';
    zeile_n_   = 0;
    satz1_ms_  = 0;

    const int64_t t0 = esp_timer_get_time();
    runde_us_        = t0;

    // Rumpf ueber cJSON und nicht ueber snprintf: im erkannten Satz koennen
    // Anfuehrungszeichen stehen, und ein selbst gebautes Maskieren waere
    // genau die Sorte Code, die erst beim seltenen Fall auffaellt.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model_);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", kSystem);
    cJSON_AddItemToArray(msgs, sys);

    for (int i = 0; i < hist_n_; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", hist_frage_[i]);
        cJSON_AddItemToArray(msgs, u);

        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "role", "assistant");
        cJSON_AddStringToObject(a, "content", hist_antwort_[i]);
        cJSON_AddItemToArray(msgs, a);
    }

    cJSON *neu = cJSON_CreateObject();
    cJSON_AddStringToObject(neu, "role", "user");
    cJSON_AddStringToObject(neu, "content", frage);
    cJSON_AddItemToArray(msgs, neu);

    char *rumpf = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rumpf == nullptr) {
        phase_ = (int32_t)Phase::Fehler;
        return;
    }

    const int laenge = (int)strlen(rumpf);
    const int status = weg_.senden(rumpf, laenge);
    free(rumpf);

    // Sauber heisst: der Rumpf ist bis zum Ende gelesen. Nur dann darf die
    // Verbindung fuer die naechste Frage stehen bleiben.
    bool sauber = false;

    if (status == 200) {
        // Nicht auf den Verbindungsabbau warten: bei einem Ereignisstrom
        // steht [DONE] vor dem Schluss, und wer auf das Ende des Sockets
        // wartet, verschenkt die letzte Sekunde.
        while (!sse_done_ && !verworfen()) {
            const int r = weg_.lesen(lese_, kLeseBytes);
            if (r <= 0) break;
            sse_feed(lese_, r);
        }
        // Nach [DONE] steht nur noch der Schlusschunk aus. Er liegt in aller
        // Regel schon im selben Paket; kommt er nicht, ist die Verbindung
        // eben weg, und das kostet weniger als darauf zu warten.
        if (sse_done_ && !verworfen()) {
            weg_.leerlesen(300);
            sauber = weg_.vollstaendig();
        }
    } else if (status != 0) {
        // Der Fehlerrumpf ist JSON mit Klartext darin — bei falschem
        // Schluessel oder unbekanntem Modell steht dort genau, was fehlt.
        const int r = weg_.lesen(lese_, kLeseBytes - 1);
        if (r > 0) lese_[r] = '\0'; else lese_[0] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %s", status, lese_);
        weg_.leerlesen(300);
        sauber = weg_.vollstaendig();
    }

    // Ein abgebrochener Strom steckt noch halb im Socket, die Verbindung
    // wird also nicht wiederverwendet (sauber bleibt false).
    weg_.abschluss(sauber);

    if (verworfen()) {
        // Kein Satz mehr fuer die Stimme, und nichts davon in den Verlauf.
        xSemaphoreTake(lock_, portMAX_DELAY);
        antwort_[0]  = '\0';
        antwort_len_ = 0;
        fertig_bis_  = 0;
        xSemaphoreGive(lock_);
        last_ms_ = (int32_t)((esp_timer_get_time() - t0) / 1000);
        phase_   = (int32_t)Phase::Bereit;
        ESP_LOGI(TAG, "Antwort abgebrochen (Taste) nach %d ms.", (int)last_ms_);
        return;
    }

    // Was noch kein ganzer Satz war, ist jetzt einer: hier endet die Antwort,
    // und der Rest muss gesprochen werden, auch ohne Punkt am Schluss.
    xSemaphoreTake(lock_, portMAX_DELAY);
    grenzen_nachfuehren(true);
    xSemaphoreGive(lock_);

    last_ms_ = (int32_t)((esp_timer_get_time() - t0) / 1000);

    char antwort[kMaxAntwort];
    copy_antwort(antwort, sizeof(antwort));

    if (antwort[0] != '\0') {
        antwort_seq_++;
        phase_ = (int32_t)Phase::Bereit;
        ESP_LOGI(TAG, "Antwort nach %d ms (erster Satz nach %d ms, %d Wechsel "
                      "Verlauf): %s",
                 (int)last_ms_, (int)satz1_ms_, hist_n_, antwort);

        // Endet die Antwort nicht auf einem Satzzeichen, haengt hinten ein
        // Bruchstueck. Mit dem letzten Teilstueck daneben laesst sich sehen,
        // ob es als eigenes delta.content kam.
        const size_t al = strlen(antwort);
        const char   e  = (al > 0) ? antwort[al - 1] : '.';
        if (e != '.' && e != '!' && e != '?' && e != ':') {
            ESP_LOGW(TAG, "Antwort endet ohne Satzzeichen, letztes "
                          "Teilstueck war \"%s\".", letztes_);
        }

        verlauf_anfuegen(frage, antwort);
    } else {
        phase_ = (int32_t)Phase::Fehler;
        ESP_LOGW(TAG, "Keine Antwort nach %d ms.", (int)last_ms_);
    }
}

void Chat::run()
{
    while (true) {
        // Vorgewaermt wird, sobald das Netz steht — nicht erst, wenn jemand
        // spricht. Der Handschlag kostet auf diesem Geraet ueber eine
        // Sekunde, und wenn Chat und Stimme gleichzeitig aufbauen, ueber
        // vier: beim ersten Tastendruck nach dem Einschalten war er damit
        // noch nicht fertig und verzoegerte genau die Frage, die er
        // beschleunigen sollte. Beim Hochfahren hat dieser Task nichts zu
        // tun, also gehoert er dorthin.
        //
        // Danach kostet der Aufruf nichts: vorwaermen() kehrt sofort
        // zurueck, solange die Verbindung als stehend gilt, und hat nach
        // einem Fehlschlag eine Sperrzeit.
        if (net::connected()) weg_.vorwaermen();

        const uint32_t jetzt = quelle_->final_seq();

        if (jetzt != gesehen_) {
            gesehen_ = jetzt;

            char frage[kMaxFrage];
            quelle_->copy_final(frage, sizeof(frage));

            if (frage[0] == '\0') {
                ESP_LOGI(TAG, "Leere Aeusserung, keine Anfrage.");
            } else if (verworfen_ == (int32_t)jetzt) {
                ESP_LOGI(TAG, "Frage per Taste verworfen: %s", frage);
            } else if (!net::connected()) {
                ESP_LOGW(TAG, "Kein Netz, Frage faellt aus.");
                phase_ = (int32_t)Phase::Fehler;
            } else {
                ESP_LOGI(TAG, "Frage: %s", frage);
                frage_seq_ = (int32_t)jetzt;
                frage_stellen(frage);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
