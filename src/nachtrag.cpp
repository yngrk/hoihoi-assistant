#include "nachtrag.h"

#include <stdarg.h>
#include <stdio.h>

#include <esp_log.h>

namespace {

// Vier Zeilen reichen: der Aufnahmetask meldet einmal pro Sekunde, der Leser
// schaut zehnmal so oft nach.
const int kZeilen = 4;
const int kBreite = 240;

char             s_text[kZeilen][kBreite];
char             s_stufe[kZeilen];
const char      *s_tag[kZeilen];
volatile int32_t s_schreib = 0;
volatile int32_t s_lies    = 0;

}  // namespace

void nachtrag::schreiben(char stufe, const char *tag, const char *fmt, ...)
{
    const int32_t w = s_schreib;
    if (w - s_lies >= kZeilen) return;   // voll, lieber die Meldung verlieren

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_text[w % kZeilen], kBreite, fmt, ap);
    va_end(ap);

    s_stufe[w % kZeilen] = stufe;
    s_tag[w % kZeilen]   = tag;

    // Erst der Inhalt, dann der Zeiger: der Leser sieht die Zeile nie halb.
    s_schreib = w + 1;
}

void nachtrag::ausgeben()
{
    while (s_lies != s_schreib) {
        const int i = s_lies % kZeilen;
        if (s_stufe[i] == 'W') {
            ESP_LOGW(s_tag[i], "%s", s_text[i]);
        } else {
            ESP_LOGI(s_tag[i], "%s", s_text[i]);
        }
        s_lies++;
    }
}
