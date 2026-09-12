#include "wachwort.h"

#include <math.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "merkmal.h"
#include "nachtrag.h"
#include "vergleich.h"

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
bool     s_geweckt   = false;    // Treffer, noch nicht abgeholt

// --- Merkmale --------------------------------------------------------------
//
// MFCC wird durchgehend gerechnet und nicht erst, wenn ein Wort fertig ist.
// Das kostet gleichmaessig wenig — gut neunzig Rahmen je Sekunde —, waehrend
// der andere Weg, Rohton puffern und am Wortende alles auf einmal rechnen,
// genau dann eine Spitze erzeugt, wenn ohnehin am meisten los ist. Der
// Aufnahmetask darf nirgends laenger stehen als 60 ms, sonst laeuft der
// DMA-Ring ueber.
//
// Der Ring haelt 2,4 Sekunden. Ein Wort ist hoechstens 1,6 s lang und wird
// 240 ms nach seinem Ende abgeschlossen, es steht also immer noch vollstaendig
// darin, wenn es gebraucht wird.
const int32_t kRing = 224;

float  *s_ring   = nullptr;   // kRing * kKoeff
float  *s_folge  = nullptr;   // der herausgeschnittene Kandidat
int32_t s_rahmen_nr = 0;      // monoton, nicht der Index im Ring

int16_t s_puffer[merkmal::kFft + 512];
int32_t s_fuell    = 0;
int64_t s_abtast   = 0;       // Abtastwerte seit dem Start
int32_t s_wort_von = 0;       // erster Rahmen des laufenden Worts

// Aus den Abtastwerten wird Rahmen fuer Rahmen MFCC, mit halber Ueberlappung.
void rahmen_fuettern(const int16_t *pcm, size_t frames)
{
    if (s_ring == nullptr) return;

    const int32_t platz = (int32_t)(sizeof(s_puffer) / sizeof(s_puffer[0]));
    if (s_fuell + (int32_t)frames > platz) s_fuell = 0;

    memcpy(&s_puffer[s_fuell], pcm, frames * sizeof(int16_t));
    s_fuell += (int32_t)frames;

    while (s_fuell >= merkmal::kFft) {
        merkmal::rahmen(s_puffer,
                        s_ring + (size_t)(s_rahmen_nr % kRing) * merkmal::kKoeff);
        s_rahmen_nr++;
        s_fuell -= merkmal::kHop;
        memmove(s_puffer, s_puffer + merkmal::kHop, s_fuell * sizeof(int16_t));
    }
}

// Ein Wort liegt zwischen zwei Rahmennummern. Gerechnet wird ueber den Zaehler
// der Abtastwerte und nicht ueber die Bloecke: 20 ms sind keine ganze Zahl von
// Rahmen, und der Fehler summierte sich sonst auf.
int32_t rahmen_bei(int64_t abtast)
{
    int64_t r = abtast / merkmal::kHop;
    if (r < 0) r = 0;
    return (int32_t)r;
}

// Der Kandidat wird mittelwertbefreit: von jedem Koeffizienten wird sein
// Mittel ueber das Wort abgezogen. Das nimmt heraus, was ueber das ganze Wort
// gleich bleibt — Mikrofon, Abstand zum Mund, Raum — und uebrig bleibt, wie
// sich der Klang veraendert. Genau darauf kommt es an.
bool kandidat_pruefen(int32_t von, int32_t bis, int32_t ms)
{
    if (s_ring == nullptr || s_folge == nullptr) return false;

    if (von < s_rahmen_nr - kRing + 1) von = s_rahmen_nr - kRing + 1;
    if (von < 0) von = 0;

    const int32_t n = bis - von;
    if (n < 8 || n > vergleich::kMaxRahmen) return false;

    for (int32_t i = 0; i < n; i++) {
        memcpy(s_folge + (size_t)i * merkmal::kKoeff,
               s_ring + (size_t)((von + i) % kRing) * merkmal::kKoeff,
               merkmal::kKoeff * sizeof(float));
    }

    for (int k = 0; k < merkmal::kKoeff; k++) {
        float s = 0.0f;
        for (int32_t i = 0; i < n; i++) s += s_folge[(size_t)i * merkmal::kKoeff + k];
        const float m = s / (float)n;
        for (int32_t i = 0; i < n; i++) s_folge[(size_t)i * merkmal::kKoeff + k] -= m;
    }

    return vergleich::kandidat(s_folge, n, ms);
}

void wort_abschliessen(int32_t blockframes)
{
    // Die Nachlaufzeit gehoert nicht zum Wort.
    const int32_t ms = (s_wort_blk - kEndeBloecke) * kBlockMs;

    if (ms >= kMinMs && ms <= kMaxMs) {
        s_woerter++;
        nachtrag::schreiben('I', TAG,
                            "Wort %u: %d ms, Spitze %d, Pegel %d, Ruhe %d.",
                            (unsigned)s_woerter, (int)ms, (int)s_wort_pk,
                            (int)s_wort_rms, (int)s_ruhe);

        const int32_t bis = rahmen_bei(s_abtast - (int64_t)kEndeBloecke * blockframes);
        if (kandidat_pruefen(s_wort_von, bis, ms)) s_geweckt = true;
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

esp_err_t wachwort::bereit()
{
    if (s_ring != nullptr) return ESP_OK;

    esp_err_t err = merkmal::bereit();
    if (err != ESP_OK) return err;

    err = vergleich::bereit();
    if (err != ESP_OK) return err;

    s_ring  = (float *)heap_caps_malloc((size_t)kRing * merkmal::kKoeff * sizeof(float),
                                        MALLOC_CAP_SPIRAM);
    s_folge = (float *)heap_caps_malloc((size_t)vergleich::kMaxRahmen * merkmal::kKoeff
                                            * sizeof(float),
                                        MALLOC_CAP_SPIRAM);
    if (s_ring == nullptr || s_folge == nullptr) {
        heap_caps_free(s_ring);
        heap_caps_free(s_folge);
        s_ring  = nullptr;
        s_folge = nullptr;
        return ESP_ERR_NO_MEM;
    }

    // Einmal nachmessen, was ein Rahmen kostet. Gut neunzig davon laufen je
    // Sekunde im Aufnahmetask, und der darf nirgends 60 ms stehen — die Zahl
    // gehoert ins Log, bevor jemand sie schaetzt.
    int16_t *probe = (int16_t *)heap_caps_malloc(merkmal::kFft * sizeof(int16_t),
                                                 MALLOC_CAP_INTERNAL);
    if (probe != nullptr) {
        for (int i = 0; i < merkmal::kFft; i++) {
            probe[i] = (int16_t)(3000.0f * sinf(i * 0.31f));
        }
        float weg[merkmal::kKoeff];
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 32; i++) merkmal::rahmen(probe, weg);
        const int us = (int)((esp_timer_get_time() - t0) / 32);
        heap_caps_free(probe);

        // 24000 / 256 = 93,75 Rahmen je Sekunde.
        const int promille = us * 9375 / 100000;
        ESP_LOGI(TAG, "Ein Rahmen kostet %d us, 94 je Sekunde sind %d.%d %% Rechenzeit.",
                 us, promille / 10, promille % 10);
    }

    vergleich::selbsttest();

    return ESP_OK;
}

void wachwort::feed(const int16_t *pcm, size_t frames, uint32_t rate)
{
    if (pcm == nullptr || frames == 0 || rate == 0) return;

    rahmen_fuettern(pcm, frames);
    s_abtast += (int64_t)frames;

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
            s_wort_von = rahmen_bei(s_abtast - (int64_t)kStartBloecke * (int64_t)frames);
        }
        return;
    }

    s_wort_blk++;
    if (pk > s_wort_pk)   s_wort_pk  = pk;
    if (rms > s_wort_rms) s_wort_rms = rms;

    s_still = laut ? 0 : (s_still + 1);
    if (s_still >= kEndeBloecke) {
        wort_abschliessen((int32_t)frames);
        return;
    }

    // Auch ein Wort, das nicht aufhoert, muss aufhoeren.
    if (s_wort_blk * kBlockMs > kMaxMs + kEndeBloecke * kBlockMs) {
        wort_abschliessen((int32_t)frames);
    }
}

bool wachwort::geweckt()
{
    const bool g = s_geweckt;
    s_geweckt = false;
    return g;
}

int32_t wachwort::ruhepegel() { return s_ruhe; }
int32_t wachwort::schwelle()  { return s_ruhe * kFaktor + kBoden; }
bool    wachwort::im_wort()   { return s_im_wort; }

void wachwort::ruhe()
{
    s_im_wort  = false;
    s_wort_blk = 0;
    s_wort_pk  = 0;
    s_wort_rms = 0;
    s_laut     = 0;
    s_still    = 0;
    s_ruhe     = kBoden;
    s_geweckt  = false;

    // Was vor der Pause halb im Puffer stand, gehoert zu keinem Rahmen mehr.
    s_fuell = 0;
}

uint32_t wachwort::woerter() { return s_woerter; }
