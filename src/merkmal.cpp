#include "merkmal.h"

#include <math.h>
#include <string.h>

#include <esp_dsp.h>
#include <esp_log.h>

static const char *TAG = "merkmal";

namespace {

using namespace merkmal;

const int kBins = kFft / 2;   // 0 .. 11953 Hz, in 46,9-Hz-Schritten

// Hann. Ohne Fenster streut jeder Sprung an den Raendern des Ausschnitts ueber
// das ganze Spektrum und verdeckt die leisen Baender.
float s_fenster[kFft];

// Arbeitsfeld: erst kFft reale Werte, nach der FFT kBins komplexe Paare,
// danach kBins Leistungen. Das Umschreiben auf die Leistung geht an derselben
// Stelle: p[k] liest s_arbeit[2k] und [2k+1] und schreibt nach s_arbeit[k],
// und k liegt nie hinter 2k.
//
// Die Ausrichtung ist Pflicht und nicht Geschmackssache. esp-dsp waehlt auf
// dem S3 dsps_fft2r_fc32_aes3, die Fassung mit den 128-Bit-Befehlen, und die
// verlangt das Feld an einer 16-Byte-Grenze. Ohne sie schreibt sie neben das
// Feld: der Uebersetzer hatte s_arbeit vier Byte hinter s_von gelegt, und aus
// den letzten beiden Mel-Baendern wurde bei jedem Rahmen NaN. Auffallen tat
// das erst ganz am Ende, als jeder Abstand zu jeder Vorlage NaN war und
// deshalb kein einziger Vergleich mehr zu einem Ergebnis fuehrte.
float s_arbeit[kFft] __attribute__((aligned(16)));

// Filterbank in schmaler Form. Ein Dreieck deckt nur wenige Bins ab, und jedes
// Bin gehoert zu hoechstens zwei Dreiecken — eine volle Matrix waere 26 KB
// fuer fast lauter Nullen.
int16_t s_von[kBand];
int16_t s_len[kBand];
float   s_gewicht[512];
int     s_gewichte = 0;

float s_dct[kKoeff][kBand];
bool  s_bereit = false;

float mel(float f)     { return 2595.0f * log10f(1.0f + f / 700.0f); }
float mel_zurueck(float m) { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

}  // namespace

esp_err_t merkmal::bereit()
{
    if (s_bereit) return ESP_OK;

    esp_err_t err = dsps_fft2r_init_fc32(nullptr, kBins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT laesst sich nicht anlegen: %s", esp_err_to_name(err));
        return err;
    }

    // Nicht ueberfluessig, auch wenn hier nur Radix-2 gerechnet wird:
    // dsps_cplx2real_fc32 entwirrt mit der Drehfaktortabelle von Radix-4 und
    // gibt ohne sie ESP_ERR_DSP_UNINITIALIZED zurueck.
    err = dsps_fft4r_init_fc32(nullptr, kBins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Tabelle fuer das Entwirren fehlt: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < kFft; i++) {
        s_fenster[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (kFft - 1));
    }

    // kBand Dreiecke zwischen 100 und 8000 Hz, gleichmaessig auf der Mel-Skala.
    // Ueber 8000 Hz steht bei Sprache nichts, was ein Wort unterscheidet, und
    // das Mikrofon hat dort ohnehin wenig zu bieten.
    const float m_unten = mel(100.0f);
    const float m_oben  = mel(8000.0f);

    float kante[kBand + 2];
    for (int i = 0; i < kBand + 2; i++) {
        const float m = m_unten + (m_oben - m_unten) * i / (kBand + 1);
        kante[i] = mel_zurueck(m) * kFft / (float)kRate;   // in Bins
    }

    s_gewichte = 0;
    for (int b = 0; b < kBand; b++) {
        const float links  = kante[b];
        const float mitte  = kante[b + 1];
        const float rechts = kante[b + 2];

        int von = (int)ceilf(links);
        int bis = (int)floorf(rechts);
        if (von < 0)      von = 0;
        if (bis > kBins - 1) bis = kBins - 1;
        if (bis < von)    bis = von - 1;

        s_von[b] = (int16_t)von;
        s_len[b] = (int16_t)(bis - von + 1);

        for (int k = von; k <= bis; k++) {
            const float w = (k < mitte) ? (k - links) / (mitte - links)
                                        : (rechts - k) / (rechts - mitte);
            s_gewicht[s_gewichte++] = (w > 0.0f) ? w : 0.0f;
        }
    }

    // Kosinustransformation, ohne c0: der erste Koeffizient ist die
    // Gesamtlautstaerke und stoert beim Vergleich nur.
    for (int i = 0; i < kKoeff; i++) {
        for (int b = 0; b < kBand; b++) {
            s_dct[i][b] = cosf((float)M_PI * (i + 1) * (b + 0.5f) / kBand);
        }
    }

    ESP_LOGI(TAG, "MFCC bereit: %d Bins, %d Baender, %d Gewichte, %d Koeffizienten.",
             kBins, kBand, s_gewichte, kKoeff);
    s_bereit = true;
    return ESP_OK;
}

void merkmal::rahmen(const int16_t *x, float *aus)
{
    if (!s_bereit) {
        memset(aus, 0, sizeof(float) * kKoeff);
        return;
    }

    for (int i = 0; i < kFft; i++) {
        s_arbeit[i] = (float)x[i] * s_fenster[i] * (1.0f / 32768.0f);
    }

    // kFft reale Werte als kBins komplexe Paare auffassen, komplex
    // transformieren und danach entwirren — das kostet halb so viel wie eine
    // FFT ueber kFft Punkte mit lauter Nullen im Imaginaerteil.
    dsps_fft2r_fc32(s_arbeit, kBins);
    dsps_bit_rev_fc32(s_arbeit, kBins);
    dsps_cplx2real_fc32(s_arbeit, kBins);

    // Bin 0 ist ein Sonderfall: dort steht im Imaginaerteil nicht null, sondern
    // das oberste Bin. Das stoert hier nicht, weil das unterste Band erst bei
    // 100 Hz anfaengt und Bin 0 nie anfasst.
    for (int k = 0; k < kBins; k++) {
        const float re = s_arbeit[2 * k];
        const float im = s_arbeit[2 * k + 1];
        s_arbeit[k] = re * re + im * im;
    }

    float log_e[kBand];
    const float *w = s_gewicht;
    for (int b = 0; b < kBand; b++) {
        float e = 0.0f;
        const int von = s_von[b];
        const int len = s_len[b];
        for (int i = 0; i < len; i++) e += s_arbeit[von + i] * w[i];
        w += len;

        // Der Boden ist kein Schoenheitsfehler: ohne ihn wird aus einem leeren
        // Band minus unendlich, und ein einziger solcher Rahmen reisst jeden
        // spaeteren Abstand auseinander.
        log_e[b] = logf(e + 1e-10f);
    }

    for (int i = 0; i < kKoeff; i++) {
        float s = 0.0f;
        for (int b = 0; b < kBand; b++) s += s_dct[i][b] * log_e[b];
        aus[i] = s;
    }
}
