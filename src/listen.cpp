#include "listen.h"

#include "nachtrag.h"

#include <math.h>
#include <stdio.h>
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
    fill_         = 0;
    live_frames_  = 0;
    einschwing_n_ = 0;
    nachklang_    = 0;
    skip_         = 0;   // nur nachklang_erwarten() schneidet vorne etwas ab
    skip_min_     = 0;
    started_ms_  = now_ms();
    last_frames_ = 0;
    sprach_      = false;
    still_ms_    = 0;
    listening_   = 1;

    // Nicht ESP_LOGI: dieser Aufruf kommt aus dem Aufnahmetask, und zwar in
    // dem Augenblick, in dem das Weckwort erkannt ist. Eine blockierende
    // Ausgabe hier schoebe alles Weitere um Zehntelsekunden nach hinten —
    // genau das erste Wort.
    nachtrag::schreiben('I', TAG, "Zuhoeren gestartet (Schwelle %d).",
                        (int)schwelle_);
}

void Listener::wecken(int32_t ruhe)
{
    if (listening_ != 0) return;

    if (ruhe < 1) ruhe = 1;
    schwelle_ = ruhe * kFaktor + kBoden;
    start();
}

void Listener::nachklang_erwarten()
{
    skip_     = (size_t)rate_ * kSkipMs / 1000;
    skip_min_ = (size_t)rate_ * kSkipMinMs / 1000;
}

void Listener::stop(const char *grund)
{
    // Erst das Ergebnis, dann das Ende: wer auf die fallende Flanke von
    // listening_ wartet, um den Mitschnitt abzuholen, muss ihn zu diesem
    // Zeitpunkt schon vollstaendig vorfinden. Andersherum saehe der
    // Transkriptionstask fuer einen Moment noch last_frames_ == 0 und
    // schickte das Ende der Aeusserung nicht mehr los.
    last_ms_ = now_ms() - started_ms_;

    measure();
    last_frames_ = (int32_t)fill_;
    listening_   = 0;

    // Der Puffer bleibt stehen — hier setzt spaeter die Worterkennung an.
    // Spitze und Effektivwert zusammen, denn allein sagt keiner von beiden
    // genug: ein einzelner Einschaltknacks treibt die Spitze auf Vollausschlag,
    // waehrend die Aufnahme in Wahrheit duenn ist. Erst der Abstand zwischen
    // beiden zeigt, was wirklich anliegt.
    // Die Fundstelle der Spitze dazu: liegt sie gleich am Anfang, ist sie kein
    // Sprachsignal, sondern der Rest des Einschwingers — und dann taugt sie
    // nicht als Bezug fuer irgendeine Verstaerkung.
    ESP_LOGI(TAG, "Zuhoeren beendet (%s): %d ms, %u Frames, "
                  "Spitze %d, Effektivwert %d, Grundrauschen %d.",
             grund, (int)last_ms_, (unsigned)fill_, (int)last_peak_,
             (int)last_rms_, (int)last_noise_);
}

// Ein Durchgang ueber den fertigen Mitschnitt. Spitze und Effektivwert sagen
// einzeln zu wenig: ein Knacks treibt die Spitze hoch, waehrend die Aufnahme
// duenn ist. Das Grundrauschen kommt als leisestes Fenster dazu — das ist eine
// Sprechpause, und der Abstand zum Effektivwert ist der Stoerabstand, die
// einzige Zahl, die fuer die Erkennung wirklich zaehlt.
void Listener::measure()
{
    const size_t fenster = (size_t)rate_ * kNoiseMs / 1000;

    int32_t spitze  = 0;
    int64_t summe   = 0;
    int64_t blk     = 0;
    size_t  blk_n   = 0;
    int32_t rauschen = -1;

    for (size_t i = 0; i < fill_; i++) {
        const int16_t s = buf_[i];
        const int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
        if (a > spitze) spitze = a;

        const int64_t q = (int64_t)s * s;
        summe += q;
        blk   += q;
        if (fenster > 0 && ++blk_n >= fenster) {
            const int32_t r = (int32_t)sqrt((double)(blk / (int64_t)blk_n));
            if (rauschen < 0 || r < rauschen) rauschen = r;
            blk   = 0;
            blk_n = 0;
        }
    }

    last_peak_  = spitze;
    last_rms_   = (fill_ > 0) ? (int32_t)sqrt((double)(summe / (int64_t)fill_)) : 0;
    last_noise_ = (rauschen > 0) ? rauschen : 0;
}

// Wann der Satz zu Ende ist. Die Taste hat das frueher beantwortet, indem
// jemand sie losliess. Jetzt sagt es der Pegel, und zwar nach derselben Regel
// wie bei der Wortabgrenzung des Weckworts — nur mit einer viel laengeren
// Pause, denn hier soll ein ganzer Satz zusammenbleiben und nicht ein Wort.
void Listener::ende_pruefen(const int16_t *pcm, size_t frames)
{
    if (frames == 0 || rate_ == 0) return;

    int64_t summe = 0;
    for (size_t i = 0; i < frames; i++) summe += (int64_t)pcm[i] * pcm[i];

    const int32_t rms = (int32_t)sqrt((double)(summe / (int64_t)frames));
    const int32_t ms  = (int32_t)(frames * 1000 / rate_);

    if (rms > schwelle_) {
        sprach_   = true;
        still_ms_ = 0;
        return;
    }

    still_ms_ += ms;

    // Vor dem ersten Wort gilt die laengere Frist: es darf jemand ueberlegen.
    // Danach beendet die Pause die Aufnahme.
    if (!sprach_) {
        if (still_ms_ >= kWartenMs) stop("nichts gesagt");
        return;
    }

    if (still_ms_ >= kStilleMs) stop("Satz zu Ende");
}

void Listener::feed(const int16_t *pcm, size_t frames)
{
    if (listening_ == 0 || buf_ == nullptr) return;

    // Der Block, wie er hereinkam. Die Abbruchentscheidung unten arbeitet auf
    // ihm und nicht auf dem, was am Ende im Puffer landet: sie fragt, ob es im
    // Raum still ist, und darauf antwortet der ganze Block.
    const int16_t *const roh   = pcm;
    const size_t         roh_n = frames;

    // Den Nachklang des Lautsprechers vorne abschneiden, siehe kSkipMs.
    if (skip_ > 0) {
        const size_t weg = (skip_ < frames) ? skip_ : frames;

        int32_t spitze = 0;
        for (size_t i = 0; i < weg; i++) {
            const int32_t a = (pcm[i] < 0) ? -(int32_t)pcm[i] : (int32_t)pcm[i];
            if (a > spitze) spitze = a;
        }
        if (nachklang_ == 0) nachklang_ = spitze;
        if (einschwing_n_ < kEinschwingBloecke) einschwing_[einschwing_n_++] = spitze;

        // Abgeklungen heisst: ein Achtel der ersten Spitze, also 18 dB
        // darunter. Die Mindestmenge geht in jedem Fall weg, denn die ersten
        // Bloecke koennen zufaellig leise sein.
        const bool weiter = (skip_min_ > 0) || (spitze * 8 > nachklang_);

        if (weiter) {
            skip_     -= weg;
            skip_min_  = (skip_min_ > weg) ? (skip_min_ - weg) : 0;
            pcm       += weg;
            frames    -= weg;
        } else {
            skip_ = 0;
        }

        if (skip_ == 0 && einschwing_n_ > 0) {
            char zeile[96];
            int  n = 0;
            for (int i = 0; i < einschwing_n_ && n < (int)sizeof(zeile) - 8; i++) {
                n += snprintf(&zeile[n], sizeof(zeile) - n, "%s%d",
                              i ? " " : "", (int)einschwing_[i]);
            }
            nachtrag::schreiben('I', TAG, "Nachklang je 20 ms: %s", zeile);
        }

        // Kein vorzeitiges Zurueck mehr, auch wenn vom Block nichts uebrig
        // ist: die Stilleuhr unten laeuft ueber den ganzen Block, und wenn der
        // Nachklang eine Sekunde lang alles auffrisst, ist genau das die
        // Sekunde, die sie zaehlen muss.
    }

    size_t room = capacity_ - fill_;
    if (frames < room) room = frames;

    if (room > 0) {
        memcpy(&buf_[fill_], pcm, room * sizeof(int16_t));
        fill_ += room;
        live_frames_ = (int32_t)fill_;
    }

    // Der Puffer reicht fuer kMaxSeconds, die Zeitschranke unten greift
    // normalerweise zuerst. Trotzdem abfangen, statt still zu verwerfen.
    if (fill_ >= capacity_) {
        stop("Puffer voll");
        return;
    }

    // Das Ende zuletzt, nach dem Mitschreiben: der Block, der die Stille voll
    // macht, gehoert noch zur Aufnahme. Andersherum fehlte am Schluss jedes
    // Mal ein Block, und das faellt genau dann auf, wenn der letzte Laut kurz
    // war.
    ende_pruefen(roh, roh_n);
    if (listening_ == 0) return;

    // Das Netz: der Puffer ist endlich, und ein Geraeusch, das nie leiser
    // wird — ein Luefter, der anspringt — wuerde die Stilleuhr nie erreichen.
    if (now_ms() - started_ms_ >= kMaxSeconds * 1000) {
        stop("Zeit abgelaufen");
    }
}
