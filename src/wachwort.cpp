#include "wachwort.h"

#include <math.h>

#include <esp_timer.h>

#include "nachtrag.h"

static const char *TAG = "wach";

namespace {

// Ein Block sind 20 ms. Alle Zeiten hier in Bloecken, damit nichts von der
// Abtastrate abhaengt.
const int32_t kBlockMs = 20;

// Zwei Bloecke ueber der Schwelle gelten als Anfang. Ein einzelner reicht
// nicht: ein Tastenknacks oder ein Stuhlruecken ist genau einer.
const int32_t kStartBloecke = 2;

// Nach dem letzten lauten Block wird 240 ms weitergehoert. Kuerzer, und
// "HoiHoi" zerfiele an der Pause in der Mitte in zwei Kandidaten.
const int32_t kEndeBloecke = 12;

// Ein Weckwort ist kein Huesteln und kein Satz.
const int32_t kMinMs = 200;
const int32_t kMaxMs = 1600;

// Sprache faengt an, wo der Pegel das Vierfache der Ruhe erreicht, aber nie
// unter einem festen Boden — in echter Stille ginge die Ruhe sonst gegen null
// und jedes Rauschen waere das Vierfache davon.
const int32_t kFaktor = 4;
const int32_t kBoden  = 60;

int32_t  s_ruhe      = kBoden;   // nachgefuehrter Ruhepegel
int32_t  s_laut      = 0;        // Bloecke am Stueck ueber der Schwelle
int32_t  s_still     = 0;        // Bloecke am Stueck darunter, im Wort
bool     s_im_wort   = false;
int32_t  s_wort_blk  = 0;        // Laenge des laufenden Worts in Bloecken
int32_t  s_wort_pk   = 0;        // Spitze darin
int32_t  s_wort_rms  = 0;        // groesster Blockmittelwert darin
uint32_t s_woerter   = 0;

void wort_abschliessen()
{
    // Die Nachlaufzeit gehoert nicht zum Wort.
    const int32_t ms = (s_wort_blk - kEndeBloecke) * kBlockMs;

    if (ms >= kMinMs && ms <= kMaxMs) {
        s_woerter++;
        nachtrag::schreiben('I', TAG,
                            "Wort %u: %d ms, Spitze %d, Pegel %d, Ruhe %d.",
                            (unsigned)s_woerter, (int)ms, (int)s_wort_pk,
                            (int)s_wort_rms, (int)s_ruhe);
    } else {
        // Verworfen ist hier keine Nebensache: zu lang heisst, zwei Woerter
        // sind zusammengelaufen, zu kurz heisst, eines ist auseinandergefallen.
        // Beides faellt spaeter nur als "erkennt nichts" auf, ohne zu sagen,
        // woran es liegt. Die Meldung geht wieder weg, sobald die Abgrenzung
        // steht.
        nachtrag::schreiben('I', TAG, "Verworfen (%s): %d ms, Spitze %d, Ruhe %d.",
                            (ms > kMaxMs) ? "zu lang" : "zu kurz",
                            (int)ms, (int)s_wort_pk, (int)s_ruhe);
    }

    s_im_wort  = false;
    s_wort_blk = 0;
    s_wort_pk  = 0;
    s_wort_rms = 0;
    s_still    = 0;
    s_laut     = 0;
}

}  // namespace

void wachwort::feed(const int16_t *pcm, size_t frames, uint32_t rate)
{
    if (pcm == nullptr || frames == 0 || rate == 0) return;

    int64_t summe = 0;
    int32_t pk    = 0;
    for (size_t i = 0; i < frames; i++) {
        const int32_t s = pcm[i];
        summe += (int64_t)s * s;
        const int32_t a = (s < 0) ? -s : s;
        if (a > pk) pk = a;
    }
    const int32_t rms = (int32_t)sqrt((double)(summe / (int64_t)frames));

    const int32_t schwelle = s_ruhe * kFaktor + kBoden;
    const bool    laut     = rms > schwelle;

    if (!s_im_wort) {
        // Ruhepegel nur nachfuehren, solange niemand spricht. Traege nach
        // oben, damit ein einzelner Huster ihn nicht hochzieht.
        if (!laut) s_ruhe = (s_ruhe * 15 + rms) / 16;
        if (s_ruhe < 1) s_ruhe = 1;

        s_laut = laut ? (s_laut + 1) : 0;
        if (s_laut >= kStartBloecke) {
            s_im_wort  = true;
            s_wort_blk = s_laut;   // die Bloecke seit dem Anfang zaehlen mit
            s_wort_pk  = pk;
            s_wort_rms = rms;
            s_still    = 0;
        }
        return;
    }

    s_wort_blk++;
    if (pk > s_wort_pk)   s_wort_pk  = pk;
    if (rms > s_wort_rms) s_wort_rms = rms;

    s_still = laut ? 0 : (s_still + 1);
    if (s_still >= kEndeBloecke) {
        wort_abschliessen();
        return;
    }

    // Auch ein Wort, das nicht aufhoert, muss aufhoeren.
    if (s_wort_blk * kBlockMs > kMaxMs + kEndeBloecke * kBlockMs) {
        wort_abschliessen();
    }
}

void wachwort::ruhe()
{
    s_im_wort  = false;
    s_wort_blk = 0;
    s_wort_pk  = 0;
    s_wort_rms = 0;
    s_laut     = 0;
    s_still    = 0;
    s_ruhe     = kBoden;
}

uint32_t wachwort::woerter() { return s_woerter; }
