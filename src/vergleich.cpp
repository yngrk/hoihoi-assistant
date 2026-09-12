#include "vergleich.h"

#include <math.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "merkmal.h"
#include "nachtrag.h"

static const char *TAG = "wach";

namespace {

using merkmal::kKoeff;

float  *s_vorlage = nullptr;              // kVorlagen * kMaxRahmen * kKoeff
int32_t s_laenge[vergleich::kVorlagen];   // Rahmen je Vorlage
int32_t s_ms[vergleich::kVorlagen];
int     s_anzahl = 0;
bool    s_lernt  = false;

// Was zuletzt herauskam. Nur fuer die Anzeige, siehe vergleich.h.
volatile int32_t  s_letzt_abstand = -1;
volatile int32_t  s_letzt_ms      = 0;
volatile int32_t  s_letzt_treffer = 0;
volatile uint32_t s_bewertet      = 0;
volatile uint32_t s_treffer       = 0;

// Zwei Zeilen reichen: die Tafel wird spaltenweise abgeraeumt und nur die
// letzte Spalte wird noch gebraucht.
float s_zeile_a[vergleich::kMaxRahmen + 1];
float s_zeile_b[vergleich::kMaxRahmen + 1];

const float kWeit = 1e9f;

float rahmenabstand(const float *a, const float *b)
{
    float s = 0.0f;
    for (int i = 0; i < kKoeff; i++) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return sqrtf(s);
}

// Abstand je Rahmen, also durch die Weglaenge geteilt. Ohne das waere ein
// langes Wort immer weiter weg als ein kurzes, egal wie gut es passt.
float dtw(const float *a, int32_t m, const float *b, int32_t n)
{
    float *vor  = s_zeile_a;
    float *jetzt = s_zeile_b;

    vor[0] = 0.0f;
    for (int32_t j = 1; j <= n; j++) vor[j] = kWeit;

    for (int32_t i = 1; i <= m; i++) {
        jetzt[0] = kWeit;
        const float *ai = a + (i - 1) * kKoeff;
        for (int32_t j = 1; j <= n; j++) {
            float best = vor[j - 1];
            if (vor[j]   < best) best = vor[j];
            if (jetzt[j - 1] < best) best = jetzt[j - 1];
            jetzt[j] = best + rahmenabstand(ai, b + (j - 1) * kKoeff);
        }
        float *t = vor; vor = jetzt; jetzt = t;
    }

    return vor[n] / (float)(m + n);
}

}  // namespace

esp_err_t vergleich::bereit()
{
    if (s_vorlage != nullptr) return ESP_OK;

    const size_t gross = (size_t)kVorlagen * kMaxRahmen * kKoeff * sizeof(float);
    s_vorlage = (float *)heap_caps_malloc(gross, MALLOC_CAP_SPIRAM);
    if (s_vorlage == nullptr) return ESP_ERR_NO_MEM;

    memset(s_laenge, 0, sizeof(s_laenge));
    memset(s_ms, 0, sizeof(s_ms));
    s_anzahl = 0;
    return ESP_OK;
}

int  vergleich::eingelernt() { return s_anzahl; }
bool vergleich::lernt()      { return s_lernt; }

int32_t  vergleich::letzter_abstand() { return s_letzt_abstand; }
int32_t  vergleich::letzte_dauer()    { return s_letzt_ms; }
bool     vergleich::letzter_treffer() { return s_letzt_treffer != 0; }
uint32_t vergleich::bewertet()        { return s_bewertet; }
uint32_t vergleich::treffer()         { return s_treffer; }

void vergleich::einlernen()
{
    if (s_vorlage == nullptr) return;
    s_anzahl = 0;
    s_lernt  = true;

    // Diese Zeile kommt aus dem Anzeigetask und geht direkt ins Log, nicht
    // ueber nachtrag: dort schreibt nur der Aufnahmetask, und ein zweiter
    // Schreiber braeuchte eine Sperre. Ein paar ausgelassene Bilder beim
    // Tastendruck fallen nicht auf.
    ESP_LOGI(TAG, "Einlernen: das Weckwort bitte %d mal sagen, mit Pausen.",
             (int)kVorlagen);
}

void vergleich::selbsttest()
{
    const int32_t kN = 40;
    float *a = (float *)heap_caps_malloc((size_t)kN * kKoeff * sizeof(float),
                                         MALLOC_CAP_SPIRAM);
    float *b = (float *)heap_caps_malloc((size_t)kN * kKoeff * sizeof(float),
                                         MALLOC_CAP_SPIRAM);
    int16_t *ton = (int16_t *)heap_caps_malloc(merkmal::kFft * sizeof(int16_t),
                                               MALLOC_CAP_INTERNAL);
    if (a == nullptr || b == nullptr || ton == nullptr) {
        heap_caps_free(a); heap_caps_free(b); heap_caps_free(ton);
        return;
    }

    for (int32_t r = 0; r < kN; r++) {
        for (int i = 0; i < merkmal::kFft; i++) {
            ton[i] = (int16_t)(4000.0f * sinf((i + r * merkmal::kHop) * 0.21f));
        }
        merkmal::rahmen(ton, a + (size_t)r * kKoeff);

        for (int i = 0; i < merkmal::kFft; i++) {
            ton[i] = (int16_t)(4000.0f * sinf((i + r * merkmal::kHop) * 0.37f));
        }
        merkmal::rahmen(ton, b + (size_t)r * kKoeff);
    }

    const float daa = dtw(a, kN, a, kN);
    const float dab = dtw(a, kN, b, kN);

    ESP_LOGI(TAG, "Selbsttest: c1..c4 = %d %d %d %d (x100), Abstand zu sich %d, zu anderem %d (x100).",
             (int)(a[0] * 100), (int)(a[1] * 100), (int)(a[2] * 100), (int)(a[3] * 100),
             (int)(daa * 100), (int)(dab * 100));

    heap_caps_free(a); heap_caps_free(b); heap_caps_free(ton);
}

bool vergleich::kandidat(const float *folge, int32_t rahmen, int32_t ms)
{
    if (s_vorlage == nullptr || rahmen < 8) return false;
    if (rahmen > kMaxRahmen) rahmen = kMaxRahmen;

    // Eingelernt wird nur nach ausdruecklichem Anstoss. Dauerhaft abgelegt
    // wird noch nichts: nach dem Einschalten ist das Geraet wieder leer.
    if (s_lernt && s_anzahl < kVorlagen) {
        memcpy(s_vorlage + (size_t)s_anzahl * kMaxRahmen * kKoeff, folge,
               (size_t)rahmen * kKoeff * sizeof(float));
        s_laenge[s_anzahl] = rahmen;
        s_ms[s_anzahl]     = ms;
        s_anzahl++;
        if (s_anzahl >= kVorlagen) s_lernt = false;
        nachtrag::schreiben('I', TAG, "Vorlage %d von %d: %d ms, %d Rahmen.",
                            s_anzahl, (int)kVorlagen, (int)ms, (int)rahmen);
        return false;
    }

    if (s_anzahl == 0) return false;   // nichts eingelernt, nichts zu vergleichen

    const int64_t t0 = esp_timer_get_time();

    float   beste = kWeit;
    int     welche = -1;
    int     geprueft = 0;
    for (int v = 0; v < s_anzahl; v++) {
        // Wer um mehr als die Haelfte daneben liegt, ist nicht dasselbe Wort.
        // Das spart nicht nur Rechnerei, es verhindert auch, dass DTW ein
        // kurzes Geraeusch auf ein langes Wort dehnt und dabei billig wegkommt.
        const int32_t l = s_laenge[v];
        if (rahmen * 2 < l || l * 2 < rahmen) continue;

        geprueft++;
        const float d = dtw(s_vorlage + (size_t)v * kMaxRahmen * kKoeff, l,
                            folge, rahmen);
        if (d < beste) { beste = d; welche = v; }
    }

    const int us = (int)(esp_timer_get_time() - t0);

    if (welche < 0) {
        s_letzt_abstand = -1;
        s_letzt_ms      = ms;
        s_letzt_treffer = 0;
        s_bewertet      = s_bewertet + 1;
        nachtrag::schreiben('I', TAG, "  %d ms, %d Rahmen: Laenge passt zu keiner Vorlage.",
                            (int)ms, (int)rahmen);
        return false;
    }

    // Der Abstand steht auch dann im Log, wenn er nicht reicht. Er ist die
    // einzige Zahl, an der sich spaeter ablesen laesst, ob die Schwelle zu
    // eng oder zu weit sitzt — ein blosses "erkannt / nicht erkannt" liesse
    // genau das offen.
    const bool treffer = (beste < kSchwelle);

    s_letzt_abstand = (int32_t)(beste * 100.0f);
    s_letzt_ms      = ms;
    s_letzt_treffer = treffer ? 1 : 0;
    s_bewertet      = s_bewertet + 1;
    if (treffer) s_treffer = s_treffer + 1;

    nachtrag::schreiben('I', TAG, "  %d ms, %d Rahmen: Abstand %d.%02d zu Vorlage %d (%d von %d, %d us).%s",
                        (int)ms, (int)rahmen,
                        (int)beste, (int)((beste - floorf(beste)) * 100.0f),
                        welche + 1, geprueft, s_anzahl, us,
                        treffer ? "  WECKWORT." : "");
    return treffer;
}
