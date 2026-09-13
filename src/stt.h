#pragma once

// ---------------------------------------------------------------------------
// Sprache zu Text ueber die Realtime-API von OpenAI.
//
// Freie Transkription auf dem ESP32-S3 selbst gibt es nicht: Whisper tiny sind
// rund 39 MB in int8, der Chip hat 8 MB PSRAM, und Espressifs esp-sr kann auf
// diesem Chip nur feste Kommandolisten. Wer freien Text will, braucht einen
// Dienst — und wer ihn waehrend des Sprechens sehen will, einen, der
// Teilergebnisse schickt.
//
// Deshalb die Realtime-API und nicht /v1/audio/transcriptions: letztere nimmt
// die fertige Datei und antwortet einmal, hier kommt der Text stueckweise
// zurueck, waehrend noch gesprochen wird.
//
// Protokoll, knapp:
//   wss://api.openai.com/v1/realtime?intent=transcription
//   Authorization: Bearer <key>,  OpenAI-Beta: realtime=v1
//   -> session.update            Format, Modell, Sprache
//   -> input_audio_buffer.append Base64-PCM16
//   -> input_audio_buffer.commit beim Loslassen der Taste
//   <- conversation.item.input_audio_transcription.delta      Teiltext
//   <- conversation.item.input_audio_transcription.completed  Endtext
//
// Die Klasse haengt sich an den Listener und macht den Rest allein: sie merkt,
// wann eine Aufnahme beginnt, baut die Verbindung auf, schickt nach, was
// waehrend des Verbindungsaufbaus schon aufgenommen wurde, und schliesst nach
// dem Endtext wieder. Ein eigener Task, damit weder Aufnahme noch Anzeige auf
// das Netz warten.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_event_base.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "listen.h"

class Stt {
  public:
    // So viel Text haelt der Puffer. Eine Aeusserung von zehn Sekunden kommt
    // nicht in die Naehe.
    static const size_t kMaxText = 512;

    enum class Phase {
        Aus,         // kein Schluessel oder kein WLAN
        Bereit,      // wartet auf den naechsten Tastendruck
        Verbindet,   // WebSocket und Sitzung werden aufgebaut
        Hoert,       // Ton geht raus, Teiltexte kommen zurueck
        Wartet,      // Taste los, wartet auf den Endtext
        Fehler
    };

    // key/model/language kommen aus secrets.h. Ist key leer, bleibt alles aus
    // und die uebrige Firmware laeuft unveraendert weiter.
    esp_err_t begin(Listener *quelle, uint32_t sample_rate,
                    const char *key, const char *model, const char *language);

    Phase       phase() const { return (Phase)phase_; }
    const char *phase_text() const;

    // Kopiert den bisher erkannten Text. Eigene Kopie, weil der Text aus dem
    // WebSocket-Task waechst, waehrend der Anzeigetask ihn zeichnet.
    void copy_text(char *out, size_t n) const;

    // Zaehlt jede fertige Aeusserung hoch. Wer darauf wartet — der Chat —
    // fragt den Zaehler ab, statt sich einen Rueckruf geben zu lassen: ein
    // Rueckruf liefe im WebSocket-Task, und der soll nichts tun ausser
    // Nachrichten annehmen.
    uint32_t final_seq() const { return (uint32_t)final_seq_; }

    // Der Endtext der letzten Aeusserung. Eigene Kopie, weil die naechste
    // Aufnahme text_ wieder ueberschreibt, final_ aber stehen bleibt, bis
    // tatsaechlich eine neue Aeusserung fertig ist.
    void copy_final(char *out, size_t n) const;

    // Entscheidet ueber den Endtext einer Pruefaufnahme (Listener::pruefung):
    // true heisst, es wurde wirklich etwas gesagt, und der Text geht als
    // Frage weiter. false verwirft ihn — dann bekommt der Chat einen leeren
    // Endtext und fragt nichts. Laeuft im WebSocket-Task.
    using Pruefer = bool (*)(const char *text);
    void pruefer(Pruefer f) { pruefer_ = f; }

  private:
    static void task_trampolin(void *self);
    static void ws_event(void *handler_args, esp_event_base_t base,
                         int32_t id, void *event_data);

    void run();
    void on_message(const char *data, int len);
    void set_text(const char *s);
    void append_text(const char *s);

    esp_err_t open_session();
    void      close_session(const char *grund);
    bool      send_json(const char *json);
    bool      send_audio(const int16_t *pcm, size_t frames);
    bool      send_config();

    Listener *quelle_ = nullptr;
    uint32_t  rate_   = 24000;

    const char *key_   = nullptr;
    const char *model_ = nullptr;
    const char *lang_  = nullptr;

    void *client_ = nullptr;   // esp_websocket_client_handle_t

    volatile int32_t phase_      = 0;   // Phase
    volatile int32_t verbunden_  = 0;
    volatile int32_t sitzung_ok_ = 0;
    volatile int32_t endtext_    = 0;

    // Die Verbindung ist nicht bloss zu, sie ist gescheitert. Der Unterschied
    // zaehlt: waehrend eines Aufbaus ist verbunden_ auch null, und ein
    // Neuversuch mittendrin wuerde den Aufbau abwuergen.
    volatile int32_t ws_fehler_  = 0;

    size_t gesendet_ = 0;      // Frames, die schon draussen sind

    Pruefer          pruefer_  = nullptr;
    volatile int32_t pruefung_ = 0;   // die laufende Aeusserung ist eine Pruefaufnahme

    char              text_[kMaxText] = {0};
    size_t            text_len_       = 0;
    char              final_[kMaxText] = {0};
    volatile int32_t  final_seq_       = 0;
    SemaphoreHandle_t text_lock_      = nullptr;

    // Senden gegen Abbau, siehe send_json().
    SemaphoreHandle_t senden_lock_    = nullptr;

    // Arbeitspuffer fuer eine Sendung: Rohton, Base64 und die JSON-Huelle.
    // Einmal angelegt statt je Block auf dem Stack — 100 ms bei 24 kHz sind
    // 4800 Byte Ton und 6400 Zeichen Base64.
    static const size_t kChunkFrames = 2400;   // 100 ms bei 24 kHz
    uint8_t *b64_  = nullptr;
    char    *json_ = nullptr;
};
