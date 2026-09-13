#include "vergleich.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include "merkmal.h"

static const char *TAG = "wach";

namespace {

using merkmal::kKoeff;

const char *kArtName[vergleich::kArten] = { "Wort", "Fest" };

// --- Vorlagen ---------------------------------------------------------------

float  *s_roh = nullptr;                   // kVorlagen * kMaxRahmen * kKoeff, roh
int32_t s_laenge[vergleich::kVorlagen];    // Rahmen je Vorlage
int32_t s_ms[vergleich::kVorlagen];
int     s_anzahl = 0;

// Neue Vorlagen landen erst hier. Die alten bleiben gueltig, bis alle
// neuen stehen — wer das Einlernen abbricht, steht nicht mit einem halben
// Satz da.
float  *s_neu = nullptr;
int32_t s_neu_laenge[vergleich::kVorlagen];
int32_t s_neu_ms[vergleich::kVorlagen];
volatile int32_t s_neu_anzahl = 0;
volatile int32_t s_lernt      = 0;

// Das Mittel ueber alle Rahmen der Vorlagen einer Lage, fuer die Art "Fest".
float s_mittel[vergleich::kLagen][kKoeff];
bool  s_lage_da[vergleich::kLagen];

int lage_von(int v) { return v / vergleich::kJeLage; }

// Die Vorlagen, fertig vorbereitet je Art: kArten * kVorlagen * kMaxRahmen * kKoeff.
float *s_vor = nullptr;

// Zum Packen und Entpacken fuer das NVS: eine Vorlage als 16-Bit-Zahlen.
int16_t *s_packen = nullptr;

// Der Kandidat, vorbereitet: einmal fuer "Wort", und fuer "Fest" einmal je
// Lage, mit deren Mittel. (1 + kLagen) * kMaxRahmen * kKoeff.
float *s_kand = nullptr;

int kand_index(int art, int lage) { return (art == vergleich::kFest) ? 1 + lage : 0; }

float *kand(int art, int lage)
{
    return s_kand + (size_t)kand_index(art, lage) * vergleich::kMaxRahmen * kKoeff;
}

// --- Uebergabe aus dem Aufnahmetask ---------------------------------------
//
// Ein Schreiber, ein Leser, ein Platz: der Aufnahmetask schreibt nur, wenn
// s_eingang_voll null ist, und setzt es danach; der Vergleichstask setzt es
// erst zurueck, wenn er fertig ist. Mehr als ein Kandidat zur Zeit kommt
// nicht vor — zwischen zwei Woertern liegen mindestens 240 ms Stille.
float           *s_eingang      = nullptr;
volatile int32_t s_eingang_n    = 0;
volatile int32_t s_eingang_ms   = 0;
volatile int32_t s_eingang_voll = 0;
TaskHandle_t     s_task         = nullptr;

volatile int32_t s_geweckt = 0;

// --- Fuer die Anzeige --------------------------------------------------------

volatile int32_t  s_letzt_abstand = -1;
volatile int32_t  s_letzt_ms      = 0;
volatile int32_t  s_letzt_treffer = 0;
volatile uint32_t s_bewertet      = 0;
volatile uint32_t s_treffer       = 0;

// --- Testbetrieb, siehe vergleich.h -----------------------------------------

const char *kMarkeName[vergleich::kMarken] = { "HOIHOI", "HA HA", "HE HE", "HO HO" };

volatile int32_t  s_marke = vergleich::kHoi;
volatile int32_t  s_leeren = 0;   // Anzeige/Aufnahme bitten, der Vergleich leert

volatile int32_t  s_min[vergleich::kArten][vergleich::kMarken];
volatile int32_t  s_max[vergleich::kArten][vergleich::kMarken];
volatile uint32_t s_gesagt[vergleich::kMarken];
volatile uint32_t s_wach[vergleich::kMarken];
volatile int32_t  s_letzt[vergleich::kArten] = { -1, -1 };
volatile int32_t  s_letzt_marke = -1;

void statistik_nullen()
{
    for (int a = 0; a < vergleich::kArten; a++) {
        for (int m = 0; m < vergleich::kMarken; m++) {
            s_min[a][m] = -1;
            s_max[a][m] = -1;
        }
        s_letzt[a] = -1;
    }
    for (int m = 0; m < vergleich::kMarken; m++) {
        s_gesagt[m] = 0;
        s_wach[m]   = 0;
    }
    s_letzt_marke   = -1;
    s_bewertet      = 0;
    s_treffer       = 0;
    s_letzt_abstand = -1;
}

// Zwei Zeilen reichen: die Tafel wird spaltenweise abgeraeumt und nur die
// letzte Spalte wird noch gebraucht. Nur der Vergleichstask rechnet, und der
// Selbsttest laeuft, bevor es ihn gibt.
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
    float *vor   = s_zeile_a;
    float *jetzt = s_zeile_b;

    vor[0] = 0.0f;
    for (int32_t j = 1; j <= n; j++) vor[j] = kWeit;

    for (int32_t i = 1; i <= m; i++) {
        jetzt[0] = kWeit;
        const float *ai = a + (i - 1) * kKoeff;
        for (int32_t j = 1; j <= n; j++) {
            float best = vor[j - 1];
            if (vor[j]       < best) best = vor[j];
            if (jetzt[j - 1] < best) best = jetzt[j - 1];
            jetzt[j] = best + rahmenabstand(ai, b + (j - 1) * kKoeff);
        }
        float *t = vor; vor = jetzt; jetzt = t;
    }

    return vor[n] / (float)(m + n);
}

// Eine rohe Folge in die Form einer Art bringen: die zwoelf Koeffizienten,
// befreit vom Mittel des Worts oder vom festen Mittel aller Vorlagen.
void vorbereiten(const float *roh, int32_t n, int art, int lage, float *aus)
{
    float mittel[kKoeff];

    if (art == vergleich::kFest) {
        memcpy(mittel, s_mittel[lage], sizeof(mittel));
    } else {
        for (int k = 0; k < kKoeff; k++) {
            float s = 0.0f;
            for (int32_t i = 0; i < n; i++) s += roh[(size_t)i * kKoeff + k];
            mittel[k] = s / (float)n;
        }
    }

    for (int32_t i = 0; i < n; i++) {
        for (int k = 0; k < kKoeff; k++) {
            aus[(size_t)i * kKoeff + k] = roh[(size_t)i * kKoeff + k] - mittel[k];
        }
    }
}

float *vorlage_art(int art, int v)
{
    return s_vor + (size_t)art * vergleich::kVorlagen * vergleich::kMaxRahmen * kKoeff
                 + (size_t)v * vergleich::kMaxRahmen * kKoeff;
}

// Nach dem Einlernen und nach dem Laden: Mittel bestimmen, alle Vorlagen fuer
// alle Arten vorbereiten. Einmal gerechnet, nicht bei jedem Kandidaten.
void vorlagen_vorbereiten()
{
    for (int lage = 0; lage < vergleich::kLagen; lage++) {
        double  summe[kKoeff] = {0};
        int64_t rahmen        = 0;
        for (int v = 0; v < s_anzahl; v++) {
            if (lage_von(v) != lage) continue;
            const float *r = s_roh + (size_t)v * vergleich::kMaxRahmen * kKoeff;
            for (int32_t i = 0; i < s_laenge[v]; i++) {
                for (int k = 0; k < kKoeff; k++) summe[k] += r[(size_t)i * kKoeff + k];
            }
            rahmen += s_laenge[v];
        }
        s_lage_da[lage] = (rahmen > 0);
        for (int k = 0; k < kKoeff; k++) {
            s_mittel[lage][k] = (rahmen > 0) ? (float)(summe[k] / (double)rahmen) : 0.0f;
        }
    }

    for (int v = 0; v < s_anzahl; v++) {
        const float *r = s_roh + (size_t)v * vergleich::kMaxRahmen * kKoeff;
        for (int art = 0; art < vergleich::kArten; art++) {
            vorbereiten(r, s_laenge[v], art, lage_von(v), vorlage_art(art, v));
        }
    }
}

// --- NVS -----------------------------------------------------------------------

const char *kNamensraum = "wach";

// Was an den Merkmalen haengt. Aendert sich eine dieser Zahlen, passen die
// gespeicherten Koeffizienten nicht mehr zu dem, was gerechnet wird — und ein
// Vergleich damit sieht aus wie ein funktionierender, nur mit falschen
// Abstaenden.
struct Kopf {
    uint16_t format;
    uint16_t koeff;
    uint16_t fft;
    uint16_t hop;
    uint32_t rate;
    int32_t  anzahl;
    int32_t  laenge[vergleich::kVorlagen];
    int32_t  ms[vergleich::kVorlagen];
    float    faktor[vergleich::kVorlagen];   // gespeicherte Zahl * faktor = Wert
};

const uint16_t kFormat = 2;

// Der Stand davor: vier Vorlagen als float. Wird noch gelesen, damit nach dem
// Aufspielen nicht erst eingelernt werden muss, bevor das Geraet ueberhaupt
// wieder hoert — geschrieben wird nur noch das neue Format.
const int      kAltVorlagen = 4;
const uint16_t kAltFormat   = 1;
struct KopfAlt {
    uint16_t format;
    uint16_t koeff;
    uint16_t fft;
    uint16_t hop;
    uint32_t rate;
    int32_t  anzahl;
    int32_t  laenge[kAltVorlagen];
    int32_t  ms[kAltVorlagen];
};

void speichern()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamensraum, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Vorlagen nicht gespeichert: nvs_open %s.", esp_err_to_name(err));
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    size_t bytes = 0;

    // Erst die Folgen, dann der Kopf: faellt der Strom dazwischen, passt der
    // alte Kopf nicht zu den neuen Laengen, und das Laden verwirft den Stand,
    // statt ihn falsch zu lesen.
    float faktor[vergleich::kVorlagen] = {0};

    for (int v = 0; v < s_anzahl && err == ESP_OK; v++) {
        const float  *r    = s_roh + (size_t)v * vergleich::kMaxRahmen * kKoeff;
        const size_t  zahl = (size_t)s_laenge[v] * kKoeff;

        float groesste = 0.0f;
        for (size_t i = 0; i < zahl; i++) {
            if (fabsf(r[i]) > groesste) groesste = fabsf(r[i]);
        }
        faktor[v] = (groesste > 0.0f) ? groesste / 32767.0f : 1.0f;
        for (size_t i = 0; i < zahl; i++) {
            s_packen[i] = (int16_t)lrintf(r[i] / faktor[v]);
        }

        char key[8];
        snprintf(key, sizeof(key), "v%d", v);
        const size_t n = zahl * sizeof(int16_t);
        err = nvs_set_blob(h, key, s_packen, n);
        bytes += n;
    }

    if (err == ESP_OK) {
        Kopf k = {};
        k.format = kFormat;
        k.koeff  = kKoeff;
        k.fft    = merkmal::kFft;
        k.hop    = merkmal::kHop;
        k.rate   = merkmal::kRate;
        k.anzahl = s_anzahl;
        for (int v = 0; v < vergleich::kVorlagen; v++) {
            k.laenge[v] = s_laenge[v];
            k.ms[v]     = s_ms[v];
            k.faktor[v] = faktor[v];
        }
        err = nvs_set_blob(h, "kopf", &k, sizeof(k));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    nvs_stats_t st = {};
    nvs_get_stats(nullptr, &st);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Vorlagen nicht gespeichert: %s (%u Eintraege frei).",
                 esp_err_to_name(err), (unsigned)st.free_entries);
        return;
    }
    ESP_LOGI(TAG, "Vorlagen gespeichert: %u Byte in %d ms, im NVS %u von %u Eintraegen frei.",
             (unsigned)bytes, (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)st.free_entries, (unsigned)st.total_entries);
}

// Kopf lesen, alt oder neu, und in die neue Form bringen. Ob alt oder neu,
// entscheidet die Groesse: der alte Kopf ist kuerzer.
bool kopf_lesen(nvs_handle_t h, Kopf &k, bool &alt)
{
    size_t n = 0;
    if (nvs_get_blob(h, "kopf", nullptr, &n) != ESP_OK) return false;

    k   = {};
    alt = (n == sizeof(KopfAlt));

    if (alt) {
        KopfAlt ka = {};
        if (nvs_get_blob(h, "kopf", &ka, &n) != ESP_OK || ka.format != kAltFormat) {
            return false;
        }
        k.format = kFormat;
        k.koeff  = ka.koeff;
        k.fft    = ka.fft;
        k.hop    = ka.hop;
        k.rate   = ka.rate;
        k.anzahl = (ka.anzahl <= kAltVorlagen) ? ka.anzahl : -1;
        for (int v = 0; v < kAltVorlagen; v++) {
            k.laenge[v] = ka.laenge[v];
            k.ms[v]     = ka.ms[v];
        }
        return true;
    }

    return n == sizeof(Kopf) && nvs_get_blob(h, "kopf", &k, &n) == ESP_OK;
}

void laden()
{
    nvs_handle_t h;
    if (nvs_open(kNamensraum, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "Keine Vorlagen im NVS.");
        return;
    }

    Kopf k   = {};
    bool alt = false;
    if (!kopf_lesen(h, k, alt)) {
        ESP_LOGI(TAG, "Keine Vorlagen im NVS.");
        nvs_close(h);
        return;
    }

    if (k.format != kFormat || k.koeff != kKoeff || k.fft != merkmal::kFft
        || k.hop != merkmal::kHop || k.rate != (uint32_t)merkmal::kRate
        || k.anzahl < 1 || k.anzahl > vergleich::kVorlagen) {
        ESP_LOGW(TAG, "Vorlagen im NVS passen nicht zu den Merkmalen "
                      "(Format %u, %u Koeff., %u/%u) — neu einlernen.",
                 k.format, k.koeff, k.fft, k.hop);
        nvs_close(h);
        return;
    }

    for (int v = 0; v < k.anzahl; v++) {
        if (k.laenge[v] < 8 || k.laenge[v] > vergleich::kMaxRahmen) {
            ESP_LOGW(TAG, "Vorlage %d im NVS hat %d Rahmen — neu einlernen.",
                     v + 1, (int)k.laenge[v]);
            nvs_close(h);
            return;
        }
        char key[8];
        snprintf(key, sizeof(key), "v%d", v);

        float *const  r    = s_roh + (size_t)v * vergleich::kMaxRahmen * kKoeff;
        const size_t  zahl = (size_t)k.laenge[v] * kKoeff;
        const size_t  soll = zahl * (alt ? sizeof(float) : sizeof(int16_t));
        size_t        ist  = soll;

        const esp_err_t err = alt ? nvs_get_blob(h, key, r, &ist)
                                  : nvs_get_blob(h, key, s_packen, &ist);
        if (err != ESP_OK || ist != soll) {
            ESP_LOGW(TAG, "Vorlage %d im NVS unvollstaendig — neu einlernen.", v + 1);
            nvs_close(h);
            return;
        }
        if (!alt) {
            for (size_t i = 0; i < zahl; i++) r[i] = (float)s_packen[i] * k.faktor[v];
        }
        s_laenge[v] = k.laenge[v];
        s_ms[v]     = k.ms[v];
    }
    nvs_close(h);

    s_anzahl = k.anzahl;
    vorlagen_vorbereiten();

    char zeile[96];
    int  z = 0;
    for (int v = 0; v < s_anzahl && z < (int)sizeof(zeile) - 8; v++) {
        z += snprintf(&zeile[z], sizeof(zeile) - z, "%s%d", v ? "/" : "", (int)s_ms[v]);
    }
    ESP_LOGI(TAG, "%d Vorlagen aus dem NVS%s: %s ms.", s_anzahl,
             alt ? " (altes Format)" : "", zeile);
}

// --- Der Vergleich ------------------------------------------------------------

void lernen(const float *roh, int32_t n, int32_t ms)
{
    const int i = s_neu_anzahl;
    if (i >= vergleich::kVorlagen) return;

    if (ms < vergleich::kMinLernMs) {
        ESP_LOGI(TAG, "Vorlage %d von %d: %d ms sind zu kurz fuer ein ganzes Wort, "
                      "bitte nochmal.",
                 i + 1, (int)vergleich::kVorlagen, (int)ms);
        return;
    }

    memcpy(s_neu + (size_t)i * vergleich::kMaxRahmen * kKoeff, roh,
           (size_t)n * kKoeff * sizeof(float));
    s_neu_laenge[i] = n;
    s_neu_ms[i]     = ms;
    s_neu_anzahl    = i + 1;

    ESP_LOGI(TAG, "Vorlage %d von %d: %d ms, %d Rahmen.%s",
             i + 1, (int)vergleich::kVorlagen, (int)ms, (int)n,
             (i + 1 == vergleich::kJeLage) ? "  Jetzt aus etwa 2 m Abstand." : "");

    if (s_neu_anzahl < vergleich::kVorlagen) return;

    memcpy(s_roh, s_neu, (size_t)vergleich::kVorlagen * vergleich::kMaxRahmen * kKoeff
                             * sizeof(float));
    memcpy(s_laenge, s_neu_laenge, sizeof(s_laenge));
    memcpy(s_ms, s_neu_ms, sizeof(s_ms));
    s_anzahl = vergleich::kVorlagen;
    vorlagen_vorbereiten();
    s_lernt = 0;

    // Alte Abstaende gehoeren zu alten Vorlagen.
    statistik_nullen();

    speichern();
}

void bewerten(const float *roh, int32_t n, int32_t ms)
{
    const int64_t t0 = esp_timer_get_time();

    vorbereiten(roh, n, vergleich::kWort, 0, kand(vergleich::kWort, 0));
    for (int lage = 0; lage < vergleich::kLagen; lage++) {
        if (s_lage_da[lage]) vorbereiten(roh, n, vergleich::kFest, lage, kand(vergleich::kFest, lage));
    }

    float beste[vergleich::kArten];
    int   welche[vergleich::kArten];
    for (int art = 0; art < vergleich::kArten; art++) {
        beste[art]  = kWeit;
        welche[art] = -1;
    }

    for (int v = 0; v < s_anzahl; v++) {
        // Wer um mehr als die Haelfte daneben liegt, ist nicht dasselbe Wort.
        // Das spart nicht nur Rechnerei, es verhindert auch, dass DTW ein
        // kurzes Geraeusch auf ein langes Wort dehnt und dabei billig wegkommt.
        const int32_t l = s_laenge[v];
        if (n * 2 < l || l * 2 < n) continue;

        for (int art = 0; art < vergleich::kArten; art++) {
            const float d = dtw(vorlage_art(art, v), l, kand(art, lage_von(v)), n);
            if (d < beste[art]) { beste[art] = d; welche[art] = v; }
        }
    }

    const int dauer = (int)((esp_timer_get_time() - t0) / 1000);

    const int marke = s_marke;
    s_letzt_marke   = marke;
    s_gesagt[marke] = s_gesagt[marke] + 1;

    if (welche[vergleich::kWort] < 0) {
        s_letzt_abstand = -1;
        s_letzt_ms      = ms;
        s_letzt_treffer = 0;
        s_bewertet      = s_bewertet + 1;
        for (int art = 0; art < vergleich::kArten; art++) s_letzt[art] = -1;
        ESP_LOGI(TAG, "  [%s] %d ms, %d Rahmen: Laenge passt zu keiner Vorlage.",
                 kMarkeName[marke], (int)ms, (int)n);
        return;
    }

    for (int art = 0; art < vergleich::kArten; art++) {
        const int32_t d = (int32_t)(beste[art] * 100.0f);
        s_letzt[art] = d;
        if (s_min[art][marke] < 0 || d < s_min[art][marke]) s_min[art][marke] = d;
        if (s_max[art][marke] < 0 || d > s_max[art][marke]) s_max[art][marke] = d;
    }

    const bool treffer = (beste[vergleich::kAusloeser] < vergleich::kSchwelle);
    if (treffer) s_wach[marke] = s_wach[marke] + 1;

    s_letzt_abstand = (int32_t)(beste[vergleich::kAusloeser] * 100.0f);
    s_letzt_ms      = ms;
    s_letzt_treffer = treffer ? 1 : 0;
    s_bewertet      = s_bewertet + 1;
    if (treffer) {
        s_treffer = s_treffer + 1;
        s_geweckt = 1;
    }

    // Beide Abstaende in einer Zeile, damit sie sich spaeter als Spalten
    // auswerten lassen. Die Arten haben eigene Massstaebe; verglichen wird
    // innerhalb einer Spalte, nie ueber Spalten hinweg.
    // Dahinter, welche Vorlage am naechsten lag: 1 bis 4 nah, 5 bis 8 fern.
    // Nur so ist zu sehen, ob die zweite Lage ueberhaupt etwas beitraegt.
    ESP_LOGI(TAG, "  [%s] %d ms, %d Rahmen: %s %.2f (V%d) | %s %.2f (V%d)  (%d ms).%s",
             kMarkeName[marke], (int)ms, (int)n,
             kArtName[0], beste[0], welche[0] + 1, kArtName[1], beste[1], welche[1] + 1,
             dauer, treffer ? "  WECKWORT." : "");
}

void task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Geleert wird hier und nicht beim Tastendruck: nur dieser Task
        // schreibt die Statistik, und so bleibt es auch.
        if (s_leeren) {
            s_leeren = 0;
            statistik_nullen();
            ESP_LOGI(TAG, "Statistik geleert.");
        }

        if (s_eingang_voll == 0) continue;

        if (s_lernt) {
            lernen(s_eingang, s_eingang_n, s_eingang_ms);
        } else if (s_anzahl > 0) {
            bewerten(s_eingang, s_eingang_n, s_eingang_ms);
        }

        s_eingang_voll = 0;
    }
}

}  // namespace

esp_err_t vergleich::bereit()
{
    if (s_task != nullptr) return ESP_OK;

    const size_t roh  = (size_t)kVorlagen * kMaxRahmen * kKoeff * sizeof(float);
    const size_t vor  = (size_t)kArten * kVorlagen * kMaxRahmen * kKoeff * sizeof(float);
    const size_t kand = (size_t)(1 + kLagen) * kMaxRahmen * kKoeff * sizeof(float);
    const size_t ein  = (size_t)kMaxRahmen * kKoeff * sizeof(float);
    const size_t pack = (size_t)kMaxRahmen * kKoeff * sizeof(int16_t);

    s_roh     = (float *)heap_caps_malloc(roh, MALLOC_CAP_SPIRAM);
    s_neu     = (float *)heap_caps_malloc(roh, MALLOC_CAP_SPIRAM);
    s_vor     = (float *)heap_caps_malloc(vor, MALLOC_CAP_SPIRAM);
    s_kand    = (float *)heap_caps_malloc(kand, MALLOC_CAP_SPIRAM);
    s_eingang = (float *)heap_caps_malloc(ein, MALLOC_CAP_SPIRAM);
    s_packen  = (int16_t *)heap_caps_malloc(pack, MALLOC_CAP_SPIRAM);
    if (s_roh == nullptr || s_neu == nullptr || s_vor == nullptr
        || s_kand == nullptr || s_eingang == nullptr || s_packen == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_laenge, 0, sizeof(s_laenge));
    memset(s_ms, 0, sizeof(s_ms));
    s_anzahl = 0;
    statistik_nullen();

    laden();

    // Kern 0, unter allem, was das Netz anfasst: ein Weckwort, das 50 ms
    // spaeter erkannt wird, merkt niemand, ein stockender Handschlag schon.
    // Kern 1 bleibt dem Aufnahmetakt und der Anzeige.
    if (xTaskCreatePinnedToCore(task, "vergleich", 6144, nullptr, 2, &s_task, 0) != pdPASS) {
        s_task = nullptr;
        return ESP_FAIL;
    }
    return ESP_OK;
}

int  vergleich::eingelernt() { return s_lernt ? (int)s_neu_anzahl : s_anzahl; }
bool vergleich::lernt()      { return s_lernt != 0; }

int32_t  vergleich::letzter_abstand() { return s_letzt_abstand; }
int32_t  vergleich::letzte_dauer()    { return s_letzt_ms; }
bool     vergleich::letzter_treffer() { return s_letzt_treffer != 0; }
uint32_t vergleich::bewertet()        { return s_bewertet; }
uint32_t vergleich::treffer()         { return s_treffer; }

const char *vergleich::marke_name(int m)
{
    return (m >= 0 && m < kMarken) ? kMarkeName[m] : "-";
}

int  vergleich::marke()        { return s_marke; }
void vergleich::marke_weiter() { s_marke = (s_marke + 1) % kMarken; }

void vergleich::statistik_leeren()
{
    s_leeren = 1;
    if (s_task != nullptr) xTaskNotifyGive(s_task);
}

int32_t  vergleich::letzt_art(int art)  { return s_letzt[art]; }
int      vergleich::letzt_marke()       { return s_letzt_marke; }
uint32_t vergleich::gesagt(int m)       { return s_gesagt[m]; }
uint32_t vergleich::aufgewacht(int m)   { return s_wach[m]; }
int32_t  vergleich::hoi_max(int art)    { return s_max[art][kHoi]; }

int32_t vergleich::fremd_min(int art)
{
    int32_t best = -1;
    for (int m = kHa; m < kMarken; m++) {
        const int32_t v = s_min[art][m];
        if (v >= 0 && (best < 0 || v < best)) best = v;
    }
    return best;
}

bool vergleich::geweckt()
{
    if (s_geweckt == 0) return false;
    s_geweckt = 0;
    return true;
}

void vergleich::einlernen()
{
    if (s_task == nullptr) return;
    s_neu_anzahl = 0;
    s_lernt      = 1;

    // Aus dem Anzeigetask und direkt ins Log: nachtrag gehoert allein dem
    // Aufnahmetask. Ein paar ausgelassene Bilder beim Tastendruck fallen
    // nicht auf.
    ESP_LOGI(TAG, "Einlernen: das Weckwort bitte %d mal nah am Geraet sagen, "
                  "dann %d mal aus etwa 2 m, mit Pausen.",
             (int)kJeLage, (int)kJeLage);
}

bool vergleich::einreichen(const float *roh, int32_t rahmen, int32_t ms)
{
    if (s_task == nullptr || rahmen < 8) return true;
    if (s_eingang_voll != 0) return false;
    if (rahmen > kMaxRahmen) rahmen = kMaxRahmen;

    memcpy(s_eingang, roh, (size_t)rahmen * kKoeff * sizeof(float));
    s_eingang_n    = rahmen;
    s_eingang_ms   = ms;
    s_eingang_voll = 1;
    xTaskNotifyGive(s_task);
    return true;
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
