#include "logview.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

char s_ring[logview::kLines][logview::kCols + 1];
int  s_head  = 0;    // naechster Schreibplatz
int  s_count = 0;

// Die gerade entstehende Zeile. Ein printf ohne Zeilenumbruch — und der
// Bring-up hat einige davon — waere sonst erst dann zu sehen, wenn die
// naechste Meldung sie zufaellig abschliesst.
char s_pend[logview::kCols + 1];
int  s_pend_n = 0;

SemaphoreHandle_t s_lock = nullptr;
vprintf_like_t    s_orig = nullptr;

const int kMaxMute = 4;
char      s_mute[kMaxMute][32];
int       s_mute_n = 0;

bool stumm(const char *zeile)
{
    for (int i = 0; i < s_mute_n; i++) {
        if (strstr(zeile, s_mute[i]) != nullptr) return true;
    }
    return false;
}

void commit()
{
    s_pend[s_pend_n] = '\0';
    if (stumm(s_pend)) { s_pend_n = 0; return; }

    memcpy(s_ring[s_head], s_pend, (size_t)s_pend_n + 1);
    s_head = (s_head + 1) % logview::kLines;
    if (s_count < logview::kLines) s_count++;
    s_pend_n = 0;
}

// Zeichen fuer Zeichen, weil unterwegs zweierlei wegfaellt: die
// Farbsteuerzeichen, die ESP-IDF vor und hinter jede Zeile setzt (auf einem
// monochromen Panel waeren sie nur Buchstabensalat), und alles, was der
// 5x7-Font ohnehin nicht darstellen kann.
void absorb(const char *s)
{
    bool escape = false;

    for (; *s != '\0'; s++) {
        const unsigned char c = (unsigned char)*s;

        if (escape) {
            // Eine ANSI-Folge endet mit einem Buchstaben, in der Praxis 'm'.
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) escape = false;
            continue;
        }
        if (c == 0x1B) { escape = true; continue; }

        if (c == '\n') { commit(); continue; }
        if (c == '\r') continue;

        // Tabulator ausschreiben: der Font kennt ihn nicht, und ein
        // Fragezeichen mitten in einer Ausgabe waere irrefuehrend.
        if (c == '\t') {
            for (int i = 0; i < 4 && s_pend_n < logview::kCols; i++) {
                s_pend[s_pend_n++] = ' ';
            }
            continue;
        }

        if (s_pend_n >= logview::kCols) commit();
        s_pend[s_pend_n++] = (c < 0x20) ? ' ' : (char)c;
    }
}

int hook(const char *fmt, va_list ap)
{
    // Erst die Originalausgabe, und zwar unveraendert: was am Rechner
    // ankommt, soll sich durch den Mitschnitt nicht aendern. va_list ist
    // danach verbraucht, deshalb vorher die Kopie.
    va_list ap2;
    va_copy(ap2, ap);
    const int n = (s_orig != nullptr) ? s_orig(fmt, ap) : vprintf(fmt, ap);

    // Der Puffer liegt auf dem Stapel des aufrufenden Tasks, und der kann
    // klein sein — der WLAN-Event-Task hat 2304 Byte. Deshalb knapp
    // bemessen; laengere Meldungen werden hier abgeschnitten, im seriellen
    // Log stehen sie trotzdem vollstaendig.
    char tmp[160];
    const int m = vsnprintf(tmp, sizeof(tmp), fmt, ap2);
    va_end(ap2);
    if (m <= 0) return n;

    // Aus einem Interrupt heraus darf kein Mutex genommen werden. Solche
    // Meldungen gehen dann nur auf die serielle Schnittstelle — das ist
    // besser, als hier eine Ausnahme auszuloesen.
    if (s_lock == nullptr || xPortInIsrContext()) return n;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        absorb(tmp);
        xSemaphoreGive(s_lock);
    }
    return n;
}

}  // namespace

esp_err_t logview::begin()
{
    if (s_lock != nullptr) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == nullptr) return ESP_ERR_NO_MEM;

    s_orig = esp_log_set_vprintf(&hook);
    return ESP_OK;
}

esp_err_t logview::mute(const char *fragment)
{
    if (fragment == nullptr || *fragment == 0) return ESP_ERR_INVALID_ARG;
    if (s_mute_n >= kMaxMute) return ESP_ERR_NO_MEM;

    snprintf(s_mute[s_mute_n], sizeof(s_mute[0]), "%s", fragment);
    s_mute_n++;
    return ESP_OK;
}

int logview::snapshot(char *dst, int max_lines)
{
    if (dst == nullptr || max_lines <= 0 || s_lock == nullptr) return 0;

    const size_t breite = (size_t)kCols + 1;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Ist eine Zeile im Entstehen, bekommt sie den letzten Platz. Sie ist
    // die juengste Meldung und damit die, auf die es gerade ankommt.
    const int reserviert = (s_pend_n > 0) ? 1 : 0;

    int n = s_count;
    if (n > max_lines - reserviert) n = max_lines - reserviert;
    if (n < 0) n = 0;

    int idx = (s_head - n + 2 * kLines) % kLines;
    for (int i = 0; i < n; i++) {
        memcpy(dst + (size_t)i * breite, s_ring[idx], breite);
        idx = (idx + 1) % kLines;
    }

    if (reserviert) {
        char *ziel = dst + (size_t)n * breite;
        memcpy(ziel, s_pend, (size_t)s_pend_n);
        ziel[s_pend_n] = '\0';
        n++;
    }

    xSemaphoreGive(s_lock);
    return n;
}
