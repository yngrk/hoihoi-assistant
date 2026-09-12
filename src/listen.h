#pragma once

// ---------------------------------------------------------------------------
// Zuhoeren, solange die Taste gedrueckt ist.
//
// Die KEY-Taste wirkt wie eine Sprechtaste: druecken und halten nimmt auf,
// loslassen beendet. Damit bestimmt der Sprecher Anfang und Ende selbst, und
// es kann kein Zustand offen stehen bleiben, den niemand bemerkt hat.
//
// Die Zeitschranke kMaxSeconds bleibt trotzdem, denn der Puffer ist endlich.
// Sie ist ein Netz, kein Bedienelement: greift sie, endet die Aufnahme, und
// die Taste wird erst nach dem Loslassen wieder scharf.
//
// Was aufgenommen wurde, bleibt nach dem Ende im Puffer stehen. Das ist die
// Stelle, an der spaeter die Worterkennung ansetzt: sie bekommt einen sauber
// abgegrenzten Abschnitt statt eines endlosen Stroms, und sie muss nicht
// selbst entscheiden, wann eine Aeusserung beginnt.
//
// Bewusst kein Interrupt an der Taste: der Aufnahmetask ruft poll_key() alle
// 20 ms auf, wenn ohnehin ein Audioblock vorliegt.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

class Listener {
  public:
    // Obergrenze einer Aufnahme, und danach ist der Puffer bemessen.
    static const int kMaxSeconds = 10;

    // Die ersten Millisekunden nach dem Einschalten des Wandlers werden
    // verworfen. Der ES7210 gibt beim Oeffnen einen Einschwinger ab, der als
    // Knacks im Mitschnitt steht — und als Spitzenwert am Vollausschlag, was
    // jede Messung darueber unbrauchbar macht. Gesprochen wird in dieser
    // Zeit ohnehin nicht, die Taste ist gerade erst heruntergegangen.
    static const int32_t kSkipMs = 120;

    // Fensterbreite fuer die Schaetzung des Grundrauschens. Der leiseste
    // Abschnitt einer Aufnahme ist eine Sprechpause, und dessen Effektivwert
    // ist der Rauschteppich. Robuster als eine eigene Stille-Aufnahme: ein
    // einzelner Nadelimpuls verdirbt hoechstens ein Fenster, nicht die Messung.
    static const int32_t kNoiseMs = 100;

    // Und dasselbe am hinteren Ende: das Loslassen der Taste knackt genauso
    // wie das Einschalten des Wandlers, in den Messungen lag die Spitze jeder
    // Aufnahme gut 20 ms vor Schluss. Gesprochen wird hier nicht mehr, die
    // Taste geht ja gerade hoch.
    static const int32_t kTailMs = 60;

    // So viele Abfragen (je 20 ms) muss die Taste offen sein, bevor die
    // Aufnahme endet. Ein einzelner Prellimpuls beim Halten wuerde sonst
    // mitten im Wort abschneiden.
    static const int32_t kReleasePolls = 2;

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
    int32_t last_rms() const { return last_rms_; }
    int32_t last_noise() const { return last_noise_; }

    // Die zuletzt abgeschlossene Aufnahme. Gueltig, bis die naechste beginnt.
    const int16_t *samples() const { return buf_; }
    size_t         sample_count() const { return (size_t)last_frames_; }

    // Stand der laufenden Aufnahme. Damit kann ein Zweiter mitlesen, waehrend
    // noch aufgenommen wird, ohne den Aufnahmetask zu beruehren.
    size_t live_count() const { return (size_t)live_frames_; }

  private:
    void start();
    void stop(const char *grund);

    int16_t *buf_      = nullptr;
    size_t   capacity_ = 0;      // Frames
    uint32_t rate_     = 16000;

    size_t  fill_ = 0;           // nur im Aufnahmetask angefasst
    size_t  skip_ = 0;           // noch zu verwerfende Frames

    // Alle Kennzahlen entstehen in einem Durchgang in stop(), nicht mitlaufend
    // in feed(). Erst dort steht fest, wo die Aufnahme endet — und ohne dieses
    // Ende laesst sich der Knacks am Schluss nicht aus der Rechnung halten.
    // feed() bleibt damit auch das, was es sein soll: Kopieren.
    void measure();

    // Zustand, den auch der Anzeigetask liest. Ausgerichtete 32-Bit-Worte
    // ohne Mutex, wie bei den Umweltwerten in main.cpp: Schreiben und Lesen
    // sind dort unteilbar, und ein Bild Verzoegerung faellt nicht auf.
    volatile int32_t listening_   = 0;
    volatile int32_t started_ms_  = 0;
    volatile int32_t last_ms_     = 0;
    volatile int32_t last_peak_   = 0;
    volatile int32_t last_rms_    = 0;
    volatile int32_t last_noise_  = 0;
    volatile int32_t last_frames_ = 0;
    volatile int32_t live_frames_ = 0;

    int32_t released_ = kReleasePolls;   // Aufnahmen starten erst nach einem
    bool    gesperrt_ = false;           // sauberen Loslassen der Taste
};
