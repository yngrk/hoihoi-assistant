#pragma once

// ---------------------------------------------------------------------------
// Aus dem erkannten Satz eine Antwort machen.
//
// Zweiter von drei Schritten: Sprache zu Text (stt.h), Text zu Antwort (hier),
// Antwort zu Sprache (tts.h). Drei getrennte Aufrufe statt einer
// Sprache-zu-Sprache-Sitzung, und das aus drei Gruenden: Frage und Antwort
// liegen unterwegs als Text vor und koennen damit auf dem Display stehen, eine
// Runde kostet Bruchteile eines Cents statt eines Vielfachen davon, und faellt
// etwas aus, sagt die Phase genau welcher Schritt.
//
// Protokoll, knapp:
//   POST https://api.openai.com/v1/chat/completions
//   Authorization: Bearer <key>,  Content-Type: application/json
//   -> {"model":..., "stream":true, "messages":[...]}
//   <- data: {"choices":[{"delta":{"content":"Teil"}}]}   je Stueck eine Zeile
//   <- data: [DONE]
//
// stream: true, weil der Text sonst erst nach der vollstaendigen Antwort
// erschiene — zwei bis drei Sekunden, in denen das Display nichts zu zeigen
// haette. So waechst er, waehrend er entsteht, genauso wie das Transkript.
//
// Die Klasse haengt am Stt und merkt selbst, wann eine Aeusserung fertig ist.
// Eigener Task, damit weder Aufnahme noch Anzeige auf das Netz warten.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "stt.h"

class Chat {
  public:
    // Die Antwort darf laenger sein als die Frage, aber nicht viel: auf das
    // Band passen bei Schriftgroesse 2 rund 400 Zeichen, und der
    // Systemhinweis bittet ohnehin um drei Saetze.
    static const size_t kMaxFrage   = 512;
    static const size_t kMaxAntwort = 1024;

    enum class Phase {
        Aus,         // kein Schluessel oder kein WLAN
        Bereit,      // wartet auf die naechste fertige Aeusserung
        Fragt,       // Anfrage ist raus, das erste Stueck steht aus
        Antwortet,   // Text laeuft ein
        Fehler
    };

    esp_err_t begin(Stt *quelle, const char *key, const char *model);

    Phase       phase() const { return (Phase)phase_; }
    const char *phase_text() const;

    // Frage und Antwort der laufenden Runde. Eigene Kopien, weil beide aus
    // dem Chat-Task wachsen, waehrend der Anzeigetask sie zeichnet.
    void copy_frage(char *out, size_t n) const;
    void copy_antwort(char *out, size_t n) const;

    // Zaehlt jede fertige Antwort hoch — der Anknuepfungspunkt fuer die
    // Sprachausgabe, genau wie Stt::final_seq() es fuer diese Klasse ist.
    uint32_t antwort_seq() const { return (uint32_t)antwort_seq_; }

    // Wie lange die letzte Runde vom Absenden bis zum letzten Stueck
    // gebraucht hat. Steht auf dem Display, weil Wartezeit die Groesse ist,
    // an der sich diese Kette messen lassen muss.
    int32_t last_ms() const { return last_ms_; }

  private:
    static void task_trampolin(void *self);

    void run();
    void frage_stellen(const char *frage);
    void sse_feed(const char *data, int len);
    void sse_line(const char *line);

    void set_frage(const char *s);
    void append_antwort(const char *s);

    // Ein abgeschlossener Wechsel wandert in den Verlauf, damit die naechste
    // Frage im Zusammenhang steht ("und wie lange dauert das?"). Mehr als
    // drei Wechsel braucht ein Geraet mit einer Sprechtaste nicht, und jeder
    // weitere kostet bei jeder Anfrage erneut.
    static const int kVerlauf = 3;
    void verlauf_anfuegen(const char *frage, const char *antwort);

    Stt *quelle_ = nullptr;

    const char *key_   = nullptr;
    const char *model_ = nullptr;

    volatile int32_t phase_       = 0;   // Phase
    volatile int32_t antwort_seq_ = 0;
    volatile int32_t last_ms_     = 0;

    uint32_t gesehen_ = 0;   // zuletzt verarbeitete Stt::final_seq()

    char              frage_[kMaxFrage]     = {0};
    char              antwort_[kMaxAntwort] = {0};
    size_t            antwort_len_          = 0;
    SemaphoreHandle_t lock_                 = nullptr;

    char hist_frage_[kVerlauf][256]   = {};
    char hist_antwort_[kVerlauf][384] = {};
    int  hist_n_                      = 0;

    // Arbeitspuffer, einmal angelegt statt je Anfrage auf dem Stapel: der
    // Task macht nebenbei einen TLS-Handschlag, und der braucht den Platz
    // dringender.
    static const size_t kLeseBytes = 1024;
    static const size_t kZeileBytes = 2048;
    char  *lese_    = nullptr;
    char  *zeile_   = nullptr;
    size_t zeile_n_ = 0;
    bool   sse_done_ = false;
};
