#include "echo.h"

#include "nachtrag.h"

#include <initializer_list>
#include <math.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_aec.h"

static const char *TAG = "echo";

namespace {

// --- Umtakten --------------------------------------------------------------
//
// Ein Mehrphasenfilter fuer ein festes Verhaeltnis hoch:runter. Gedacht wird
// in der gemeinsamen Zwischenrate (48 kHz fuer 24 und 16): hoch-1 Nullen
// zwischen jeden Eingangswert, tiefpassen, jeden runter-ten Wert behalten.
// Gerechnet wird davon nur, was am Ende uebrig bleibt — je Ausgangswert
// je_phase Koeffizienten.
//
// Eine eigene Umsetzung statt esp_audio_effects: es sind vierzig Zeilen, und
// die Grenzfrequenz laesst sich hier genau dahin legen, wo 16 kHz sie
// verlangen.
class Umtakter {
  public:
    bool anlegen(int hoch, int runter, uint32_t rate_ein, float grenze_hz, int je_phase)
    {
        L_ = hoch;
        M_ = runter;
        P_ = je_phase;
        const int N = L_ * P_;

        h_       = (float *)heap_caps_malloc(N * sizeof(float), MALLOC_CAP_INTERNAL);
        verlauf_ = (float *)heap_caps_calloc(2 * P_, sizeof(float), MALLOC_CAP_INTERNAL);
        if (h_ == nullptr || verlauf_ == nullptr) return false;

        // Gefensterter sinc in der Zwischenrate, Blackman-Fenster, danach auf
        // die Gleichverstaerkung L normiert: jede L-te Stelle ist ein echter
        // Wert, der Rest sind eingefuegte Nullen.
        const double fc   = (double)grenze_hz / ((double)rate_ein * L_);
        const double mitte = (N - 1) / 2.0;
        double       summe = 0.0;
        for (int i = 0; i < N; i++) {
            const double t = i - mitte;
            const double s = (t == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (N - 1))
                             + 0.08 * cos(4.0 * M_PI * i / (N - 1));
            h_[i] = (float)(s * w);
            summe += s * w;
        }
        for (int i = 0; i < N; i++) h_[i] = (float)(h_[i] * L_ / summe);
        leeren();
        return true;
    }

    void leeren()
    {
        if (verlauf_ != nullptr) memset(verlauf_, 0, 2 * P_ * sizeof(float));
        w_       = 0;
        zaehler_ = 0;
    }

    // aus muss n * hoch / runter + 2 Werte fassen.
    size_t rechnen(const int16_t *ein, size_t n, int16_t *aus)
    {
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            // Doppelt abgelegt, damit das Fenster ohne Umbruch zu lesen ist:
            // verlauf_[w_+1 .. w_+P_], der neueste Wert steht bei w_+P_.
            w_ = (w_ + 1) % P_;
            verlauf_[w_] = verlauf_[w_ + P_] = (float)ein[i];

            for (int k = 0; k < L_; k++) {
                if (zaehler_ == 0) {
                    const float *x = &verlauf_[w_ + P_];
                    const float *h = &h_[k];
                    float        y = 0.0f;
                    for (int j = 0; j < P_; j++) y += h[j * L_] * x[-j];
                    int32_t v = (int32_t)lrintf(y);
                    if (v > 32767)  v = 32767;
                    if (v < -32768) v = -32768;
                    aus[m++] = (int16_t)v;
                }
                if (++zaehler_ == M_) zaehler_ = 0;
            }
        }
        return m;
    }

  private:
    int    L_ = 1, M_ = 1, P_ = 1;
    float *h_       = nullptr;
    float *verlauf_ = nullptr;
    int    w_       = 0;
    int    zaehler_ = 0;
};

const uint32_t kRateEin = 24000;
const uint32_t kRateAec = 16000;

// 48 Koeffizienten je Phase. Bei 48 kHz Zwischenrate und Blackman-Fenster ist
// der Uebergang damit rund 2,7 kHz (herunter) und 1,8 kHz (herauf) breit; die
// Grenzen liegen so, dass bei 8 kHz kaum noch etwas durchkommt. Sprache hat
// dort ohnehin wenig.
const int   kJePhase     = 48;
const float kGrenzeRunter = 6800.0f;
const float kGrenzeHoch   = 7400.0f;

Umtakter s_runter_mic;
Umtakter s_runter_ref;
Umtakter s_hoch;

aec_handle_t *s_aec   = nullptr;
int           s_chunk = 0;

// Warteschlangen in 16 kHz, bis ein ganzer esp_aec-Block beisammen ist.
int16_t *s_mic16 = nullptr;
int16_t *s_ref16 = nullptr;
int      s_fuell = 0;
int      s_fifo  = 0;

int16_t *s_in   = nullptr;   // ausgerichtet, s_chunk
int16_t *s_ref  = nullptr;
int16_t *s_out  = nullptr;
int16_t *s_hin  = nullptr;   // 24 kHz, s_chunk * 3 / 2 + 2

// Vorspann: Ring aus gereinigtem 24-kHz-Ton.
const uint32_t kVorFrames = kRateEin * echo::kVorspannMs / 1000;
int16_t       *s_vor      = nullptr;
uint32_t       s_vor_kopf = 0;

// Zustand einer Antwort.
int32_t s_schwelle     = 0;
float   s_ausgleich    = 1.0f;
int32_t s_zaehler_ms   = 0;
bool    s_unterbrochen = false;
int32_t s_ref_ms       = -1;    // Ton seit dem ersten Referenzsignal, -1 = noch keins
int64_t s_beginn_us    = 0;

// Bilanz einer Antwort, und dieselbe noch einmal je Sekunde.
struct Bilanz {
    double  mic_q   = 0.0;   // Quadratsumme 16 kHz, nur mit Referenz
    double  aus_q   = 0.0;
    double  ref_q   = 0.0;
    int64_t werte   = 0;
    int32_t bloecke = 0;
    int32_t laut    = 0;
    int32_t zaehler_max = 0;
    int32_t rechen_max_us = 0;
    int32_t aus_max = 0;     // groesster Blockeffektivwert des Ergebnisses
};
Bilanz s_ganz;
Bilanz s_sek;
int32_t s_sek_ms = 0;

int32_t effektiv(const int16_t *x, int n)
{
    int64_t q = 0;
    for (int i = 0; i < n; i++) q += (int64_t)x[i] * x[i];
    return (int32_t)sqrt((double)q / n);
}

double quadrat(const int16_t *x, int n)
{
    double q = 0.0;
    for (int i = 0; i < n; i++) q += (double)x[i] * x[i];
    return q;
}

float dampfung_db(const Bilanz &b)
{
    if (b.aus_q <= 0.0 || b.mic_q <= 0.0) return 0.0f;
    return 10.0f * log10f((float)(b.mic_q / b.aus_q));
}

int32_t rms_aus(double q, int64_t n) { return n > 0 ? (int32_t)sqrt(q / (double)n) : 0; }

void vor_schreiben(const int16_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_vor[s_vor_kopf % kVorFrames] = x[i];
        s_vor_kopf++;
    }
}

// Ein ganzer Block durch esp_aec, danach Pegel, Zaehler, Hochtakten.
size_t block_rechnen(int16_t *aus, size_t aus_max)
{
    memcpy(s_in, s_mic16, s_chunk * sizeof(int16_t));
    memcpy(s_ref, s_ref16, s_chunk * sizeof(int16_t));
    s_fuell -= s_chunk;
    memmove(s_mic16, s_mic16 + s_chunk, s_fuell * sizeof(int16_t));
    memmove(s_ref16, s_ref16 + s_chunk, s_fuell * sizeof(int16_t));

    const int64_t t0 = esp_timer_get_time();
    aec_process(s_aec, s_in, s_ref, s_out);
    const int32_t us = (int32_t)(esp_timer_get_time() - t0);

    const int32_t ms = s_chunk * 1000 / (int32_t)kRateAec;

    // Die Referenz gilt als da, sobald eine Spitze deutlich aus dem Rauschen
    // des Eingangs ragt. Erst ab dann laeuft der Anlauf.
    int32_t ref_pk = 0;
    for (int i = 0; i < s_chunk; i++) {
        const int32_t a = (s_ref[i] < 0) ? -(int32_t)s_ref[i] : (int32_t)s_ref[i];
        if (a > ref_pk) ref_pk = a;
    }
    if (s_ref_ms < 0 && ref_pk > 300) s_ref_ms = 0;
    else if (s_ref_ms >= 0)           s_ref_ms += ms;

    // Auf den gewohnten Pegel zurueck, siehe beginnen().
    for (int i = 0; i < s_chunk; i++) {
        int32_t v = (int32_t)lrintf(s_out[i] * s_ausgleich);
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        s_out[i] = (int16_t)v;
    }

    const int32_t rms  = effektiv(s_out, s_chunk);
    const int32_t ref_rms = effektiv(s_ref, s_chunk);
    const bool    anlauf = s_ref_ms >= 0 && s_ref_ms < echo::kAnlaufMs;
    const bool    laut   = rms > (anlauf ? s_schwelle * echo::kAnlaufFaktor : s_schwelle);
    const bool    zaehlt = true;

    if (zaehlt && laut) {
        s_zaehler_ms += ms;
    } else {
        s_zaehler_ms -= ms / 2;
        if (s_zaehler_ms < 0) s_zaehler_ms = 0;
    }

    for (Bilanz *b : {&s_ganz, &s_sek}) {
        b->bloecke++;
        if (zaehlt && laut) b->laut++;
        if (s_zaehler_ms > b->zaehler_max) b->zaehler_max = s_zaehler_ms;
        if (us > b->rechen_max_us)         b->rechen_max_us = us;
        if (rms > b->aus_max)              b->aus_max = rms;
        if (s_ref_ms >= 0) {
            // Die Mikrofonseite ist hier noch leise gestellt, das Ergebnis
            // schon ausgeglichen — fuer die Daempfung beide auf eine Stufe.
            b->mic_q += quadrat(s_in, s_chunk) * s_ausgleich * s_ausgleich;
            b->aus_q += quadrat(s_out, s_chunk);
            b->ref_q += quadrat(s_ref, s_chunk);
            b->werte += s_chunk;
        }
    }

    if (!s_unterbrochen && s_zaehler_ms >= echo::kUnterbrechMs) {
        s_unterbrochen = true;
        nachtrag::schreiben('I', TAG, "Verdacht nach %d ms Antwort: Block %d ueber "
                                      "Schwelle %d, Referenz seit %d ms, Referenz im "
                                      "Block %d.",
                            (int)((esp_timer_get_time() - s_beginn_us) / 1000),
                            (int)rms, (int)s_schwelle, (int)s_ref_ms, (int)ref_rms);
    }

    size_t m = s_hoch.rechnen(s_out, s_chunk, s_hin);
    vor_schreiben(s_hin, m);
    if (m > aus_max) m = aus_max;
    memcpy(aus, s_hin, m * sizeof(int16_t));

    s_sek_ms += ms;
    if (s_sek_ms >= 1000) {
        s_sek_ms = 0;
        nachtrag::schreiben('I', TAG, "  Ref %d, Mik %d, Ergebnis %d (max %d, Schwelle %d), "
                                      "Daempfung %.1f dB, laut %d/%d, Zaehler bis %d, "
                                      "Rechnen bis %d us.",
                            (int)rms_aus(s_sek.ref_q, s_sek.werte),
                            (int)rms_aus(s_sek.mic_q, s_sek.werte),
                            (int)rms_aus(s_sek.aus_q, s_sek.werte),
                            (int)s_sek.aus_max, (int)s_schwelle, dampfung_db(s_sek),
                            (int)s_sek.laut, (int)s_sek.bloecke, (int)s_sek.zaehler_max,
                            (int)s_sek.rechen_max_us);
        s_sek = Bilanz();
    }
    return m;
}

}  // namespace

esp_err_t echo::bereit()
{
    aec_config_t cfg = {};
    cfg.mic_num       = 1;
    cfg.ref_num       = 1;
    cfg.out_num       = 1;
    cfg.filter_length = 4;
    cfg.sample_rate   = (int)kRateAec;
    cfg.caps          = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.mode          = AEC_MODE_FD_LOW_COST;
    cfg.nlp_level     = AEC_NLP_LEVEL_VERYAGGR;

    const size_t frei_vor = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_aec = aec_create_from_config(&cfg);
    if (s_aec == nullptr) {
        ESP_LOGE(TAG, "esp_aec liess sich nicht anlegen.");
        return ESP_FAIL;
    }
    s_chunk = aec_get_chunksize(s_aec);

    const size_t b16 = s_chunk * sizeof(int16_t);
    s_in  = (int16_t *)heap_caps_aligned_alloc(16, b16, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_ref = (int16_t *)heap_caps_aligned_alloc(16, b16, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_out = (int16_t *)heap_caps_aligned_alloc(16, b16, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_hin = (int16_t *)heap_caps_malloc((s_chunk * 3 / 2 + 4) * sizeof(int16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Ein Aufnahmeblock (bis 1024 Frames bei 24 kHz) ergibt hoechstens 684
    // Werte in 16 kHz; dazu, was vom letzten Mal noch keinen Block ergab.
    s_fifo  = s_chunk + 700;
    s_mic16 = (int16_t *)heap_caps_malloc(s_fifo * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_ref16 = (int16_t *)heap_caps_malloc(s_fifo * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_vor   = (int16_t *)heap_caps_calloc(kVorFrames, sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (s_in == nullptr || s_ref == nullptr || s_out == nullptr || s_hin == nullptr
        || s_mic16 == nullptr || s_ref16 == nullptr || s_vor == nullptr
        || !s_runter_mic.anlegen(2, 3, kRateEin, kGrenzeRunter, kJePhase)
        || !s_runter_ref.anlegen(2, 3, kRateEin, kGrenzeRunter, kJePhase)
        || !s_hoch.anlegen(3, 2, kRateAec, kGrenzeHoch, kJePhase)) {
        ESP_LOGE(TAG, "Kein Speicher fuer die Echounterdrueckung.");
        aec_destroy(s_aec);
        s_aec = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "esp_aec bereit: %s, Block %d Werte (%d ms), %d KB intern belegt.",
             aec_get_config_string(s_aec), s_chunk, s_chunk * 1000 / (int)kRateAec,
             (int)((frei_vor - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
    return ESP_OK;
}

bool echo::aktiv() { return s_aec != nullptr; }

void echo::beginnen(int32_t ruhe, float ausgleich)
{
    if (s_aec == nullptr) return;

    if (ruhe < 1) ruhe = 1;
    s_schwelle     = ruhe * 4 + 60;      // dieselbe Regel wie Listener
    s_ausgleich    = ausgleich;
    s_zaehler_ms   = 0;
    s_unterbrochen = false;
    s_ref_ms       = -1;
    s_beginn_us    = esp_timer_get_time();
    s_fuell        = 0;
    s_vor_kopf     = 0;
    s_sek_ms       = 0;
    s_ganz         = Bilanz();
    s_sek          = Bilanz();
    memset(s_vor, 0, kVorFrames * sizeof(int16_t));

    // Das Filter von esp_aec selbst bleibt eingestellt: Gehaeuse und Raum sind
    // bei der naechsten Antwort dieselben.
    s_runter_mic.leeren();
    s_runter_ref.leeren();
    s_hoch.leeren();
}

size_t echo::verarbeiten(const int16_t *mic, const int16_t *ref, size_t n,
                         int16_t *aus, size_t aus_max)
{
    if (s_aec == nullptr || n == 0) return 0;

    // Beide Umtakter bekommen dieselbe Menge und stehen in derselben Phase,
    // liefern also gleich viele Werte.
    if (s_fuell + (int)(n * 2 / 3) + 2 > s_fifo) {
        s_fuell = 0;   // darf nicht vorkommen; lieber verlieren als ueberschreiben
    }
    const size_t a = s_runter_mic.rechnen(mic, n, s_mic16 + s_fuell);
    s_runter_ref.rechnen(ref, n, s_ref16 + s_fuell);
    s_fuell += (int)a;

    size_t geliefert = 0;
    while (s_fuell >= s_chunk) {
        geliefert += block_rechnen(aus + geliefert, aus_max - geliefert);
    }
    return geliefert;
}

bool echo::unterbrochen() { return s_unterbrochen; }

void echo::weiter()
{
    s_unterbrochen = false;
    s_zaehler_ms   = 0;
}

size_t echo::vorspann(int16_t *aus, size_t max)
{
    if (s_vor == nullptr) return 0;

    size_t n = (s_vor_kopf < kVorFrames) ? s_vor_kopf : kVorFrames;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        aus[i] = s_vor[(s_vor_kopf - n + i) % kVorFrames];
    }
    return n;
}

void echo::beenden()
{
    if (s_aec == nullptr) return;

    nachtrag::schreiben('I', TAG, "Antwort vorbei nach %d ms: Referenz %d, Mikrofon %d, "
                                  "Ergebnis %d, Daempfung %.1f dB, laut %d von %d Bloecken, "
                                  "Zaehler bis %d, %s.",
                        (int)((esp_timer_get_time() - s_beginn_us) / 1000),
                        (int)rms_aus(s_ganz.ref_q, s_ganz.werte),
                        (int)rms_aus(s_ganz.mic_q, s_ganz.werte),
                        (int)rms_aus(s_ganz.aus_q, s_ganz.werte),
                        dampfung_db(s_ganz), (int)s_ganz.laut, (int)s_ganz.bloecke,
                        (int)s_ganz.zaehler_max,
                        s_unterbrochen ? "zuletzt Verdacht" : "kein Verdacht offen");
}
