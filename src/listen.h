#pragma once

// ---------------------------------------------------------------------------
// Zuhoeren auf Tastendruck.
//
// Die KEY-Taste schaltet um: einmal druecken startet die Aufnahme, noch einmal
// druecken beendet sie. Kein Halten — wer spricht, soll die Hand frei haben.
// Damit das Geraet nicht unbemerkt weiterlaeuft, endet die Aufnahme in jedem
// Fall nach kMaxSeconds.
//
// Was aufgenommen wurde, bleibt nach dem Ende im Puffer stehen. Das ist die
// Stelle, an der spaeter die Worterkennung ansetzt: sie bekommt einen sauber
// abgegrenzten Abschnitt statt eines endlosen Stroms, und sie muss nicht
// selbst entscheiden, wann eine Aeusserung beginnt.
//
// Bewusst kein Interrupt an der Taste: der Aufnahmetask ruft poll_key() alle
// 20 ms auf, und ein Abtastabstand von 20 ms ist zugleich die Entprellung.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

class Listener {
  public:
    // Laenger als das hier wird nicht aufgenommen, und so gross ist der Puffer.
    static const int kMaxSeconds = 10;

    // Kuerzester Abstand zwischen zwei angenommenen Tastendruecken.
    static const int32_t kDebounceMs = 200;

    // Legt den Aufnahmepuffer im PSRAM an. Schlaegt das fehl, bleibt der
    // Zustandswechsel trotzdem benutzbar, nur ohne Mitschnitt.
    esp_err_t begin(uint32_t sample_rate);

    // Tastenzustand im Aufnahmetakt, true heisst gedrueckt (die Taste ist
    // active low, das Umdrehen passiert beim Aufrufer).
    void poll_key(bool pressed);

    // PCM aus dem Aufnahmetask. Schreibt nur mit, solange zugehoert wird.
    void feed(const int16_t *pcm, size_t frames);

    bool listening() const { return listening_ != 0; }

    // Dauer der laufenden Aufnahme; 0, wenn gerade nicht zugehoert wird.
    int32_t elapsed_ms() const;

    // Dauer und Spitzenpegel der zuletzt abgeschlossenen Aufnahme.
    int32_t last_ms() const { return last_ms_; }
    int32_t last_peak() const { return last_peak_; }

    // Die zuletzt abgeschlossene Aufnahme. Gueltig, bis die naechste beginnt.
    const int16_t *samples() const { return buf_; }
    size_t         sample_count() const { return (size_t)last_frames_; }

  private:
    void start();
    void stop(const char *grund);

    int16_t *buf_      = nullptr;
    size_t   capacity_ = 0;      // Frames
    uint32_t rate_     = 16000;

    size_t  fill_ = 0;           // nur im Aufnahmetask angefasst
    int32_t peak_ = 0;

    // Zustand, den auch der Anzeigetask liest. Ausgerichtete 32-Bit-Worte
    // ohne Mutex, wie bei den Umweltwerten in main.cpp: Schreiben und Lesen
    // sind dort unteilbar, und ein Bild Verzoegerung faellt nicht auf.
    volatile int32_t listening_   = 0;
    volatile int32_t started_ms_  = 0;
    volatile int32_t last_ms_     = 0;
    volatile int32_t last_peak_   = 0;
    volatile int32_t last_frames_ = 0;

    bool    key_was_pressed_ = false;
    int32_t last_edge_ms_    = 0;
};
