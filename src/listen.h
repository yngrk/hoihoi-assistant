#pragma once

// ---------------------------------------------------------------------------
// Zuhoeren, nachdem das Weckwort gefallen ist.
//
// Frueher war die KEY-Taste eine Sprechtaste: druecken und halten nahm auf,
// loslassen beendete. Das war bequem zu bauen, weil Anfang und Ende beide von
// aussen kamen und keiner davon geraten werden musste. Jetzt kommt der Anfang
// vom Weckwort — und das Ende hat damit niemanden mehr, der es sagt.
//
// Es kommt deshalb aus dem Pegel. Nach dem Weckwort laeuft eine Frist, in der
// das erste Wort fallen muss; danach beendet eine Pause von kStilleMs die
// Aufnahme. Beide Zahlen sind Kompromisse und beide fallen auf, wenn sie
// falsch sitzen: zu kurz schneidet mitten im Satz ab, zu lang laesst das
// Geraet nach jeder Frage herumstehen.
//
// Der Bezugswert dafuer kommt nicht von hier, sondern vom Weckwort: das hat
// den Ruhepegel die ganze Zeit ueber nachgefuehrt, waehrend hier gerade
// gesprochen wird. Wer in diesem Augenblick anfinge zu messen, maesse die
// Stimme und nicht den Raum.
//
// Die Zeitschranke kMaxSeconds bleibt daneben stehen, denn der Puffer ist
// endlich. Sie ist ein Netz, kein Bedienelement.
//
// Was aufgenommen wurde, bleibt nach dem Ende im Puffer stehen — dort holt es
// die Erkennung ab.
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
    // Gemessen, Spitze je 20-ms-Block direkt nach dem Einschalten des
    // Mikrofons:
    //
    //   aus der Ruhe       44    88   2360    263    210    814
    //   nach Wiedergabe  30432 32767  16851  13575   5920   2885
    //
    // Der "Einschwinger" ist also keine Eigenschaft des ES7210, sondern das
    // Nachklingen des Lautsprechers im Mikrofon — er entsteht nur, wenn die
    // Taste eine laufende Antwort unterbricht. Aus der Ruhe heraus gibt es
    // nichts abzuschneiden, und die 120 ms, die frueher pauschal wegfielen,
    // waren das erste Wort.
    //
    // Nach einer Wiedergabe wird deshalb geschnitten, solange der Nachklang
    // anliegt: mindestens kSkipMinMs, hoechstens kSkipMs, dazwischen bis die
    // Blockspitze auf ein Achtel der ersten gefallen ist. 120 ms waren dafuer
    // auch zu wenig — bei 2885 gegen 4347 Spitze im Nutzsignal lag der Rest
    // noch in derselben Groessenordnung wie die Sprache.
    static const int32_t kSkipMs    = 240;
    static const int32_t kSkipMinMs = 60;

    // Fensterbreite fuer die Schaetzung des Grundrauschens. Der leiseste
    // Abschnitt einer Aufnahme ist eine Sprechpause, und dessen Effektivwert
    // ist der Rauschteppich. Robuster als eine eigene Stille-Aufnahme: ein
    // einzelner Nadelimpuls verdirbt hoechstens ein Fenster, nicht die Messung.
    static const int32_t kNoiseMs = 100;

    // Am hinteren Ende wurde frueher ebenfalls geschnitten: das Loslassen der
    // Taste knackte, und in den Messungen lag die Spitze jeder Aufnahme gut
    // 20 ms vor Schluss. Ohne Taste gibt es diesen Knacks nicht mehr — eine
    // Aufnahme endet jetzt in kStilleMs Stille, und darin ist nichts
    // abzuschneiden.

    // So lange bleibt nach dem Weckwort Zeit, bis das erste Wort kommt. Wer
    // "HoiHoi" sagt und dann ueberlegt, soll nicht ins Leere laufen; wer es
    // versehentlich ausgeloest hat, soll nicht die vollen zehn Sekunden
    // abwarten muessen.
    static const int32_t kWartenMs = 3000;

    // Und so lange Stille beendet den Satz. Eine Denkpause mitten in einem
    // Satz ist selten laenger; eine Pause zwischen zwei Saetzen ist es fast
    // immer. Wo die Grenze wirklich liegt, sagt erst der Gebrauch.
    static const int32_t kStilleMs = 900;

    // Dieselbe Regel wie bei der Wortabgrenzung des Weckworts: das Vierfache
    // der Ruhe, aber nie unter einem festen Boden.
    static const int32_t kFaktor = 4;
    static const int32_t kBoden  = 60;

    // Legt den Aufnahmepuffer im PSRAM an. Schlaegt das fehl, bleibt der
    // Zustandswechsel trotzdem benutzbar, nur ohne Mitschnitt.
    esp_err_t begin(uint32_t sample_rate);

    // Das Weckwort ist gefallen: ab jetzt wird aufgenommen. ruhe ist der
    // Pegel, den die Weckwort-Erkennung zuletzt als Stille gemessen hat.
    // Ein zweiter Aufruf waehrend einer laufenden Aufnahme tut nichts.
    void wecken(int32_t ruhe);

    // PCM aus dem Aufnahmetask. Schreibt nur mit, solange zugehoert wird.
    void feed(const int16_t *pcm, size_t frames);

    // Meldet, dass diese Aufnahme eine Wiedergabe unterbrochen hat und der
    // Lautsprecher deshalb noch nachklingt. Muss vor dem ersten feed()
    // kommen; ohne den Aufruf wird vorne nichts abgeschnitten.
    void nachklang_erwarten();

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

    // Entscheidet je Block, ob der Satz zu Ende ist. Bekommt den Ton so, wie
    // er hereinkam — vor dem Abschneiden am Anfang, denn gemessen wird der
    // Raum und nicht der Mitschnitt.
    void ende_pruefen(const int16_t *pcm, size_t frames);

    int16_t *buf_      = nullptr;
    size_t   capacity_ = 0;      // Frames
    uint32_t rate_     = 16000;

    size_t  fill_     = 0;       // nur im Aufnahmetask angefasst
    size_t  skip_     = 0;       // hoechstens noch zu verwerfende Frames
    size_t  skip_min_ = 0;       // davon in jedem Fall zu verwerfende
    int32_t nachklang_ = 0;      // Spitze des ersten verworfenen Blocks

    // Spitzenwerte der verworfenen Bloecke, je 20 ms. Damit steht im Log,
    // wie lange der Einschwinger des ES7210 tatsaechlich anliegt — kSkipMs
    // war bisher geschaetzt, und jede Millisekunde davon fehlt vorne am
    // gesprochenen Wort.
    static const int kEinschwingBloecke = 8;
    int32_t einschwing_[kEinschwingBloecke] = {0};
    int     einschwing_n_ = 0;

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

    // Zustand der Abbruchentscheidung. Nur im Aufnahmetask angefasst.
    int32_t schwelle_ = 0;       // ab hier gilt ein Block als Sprache
    bool    sprach_   = false;   // seit dem Weckwort ist etwas gesagt worden
    int32_t still_ms_ = 0;       // Stille am Stueck
};
