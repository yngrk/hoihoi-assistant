#include "gfx.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_timer.h>

#include "font5x7.h"

Canvas::Canvas(SyncDisplay &display, int width, int height)
    : d_(display), width_(width), height_(height)
{
}

void Canvas::clear(uint8_t color)
{
    d_.RLCD_ColorClear(color);
}

void IRAM_ATTR Canvas::te_isr(void *arg)
{
    Canvas       *self = (Canvas *)arg;
    const int64_t now  = esp_timer_get_time();

    if (self->te_last_us_ != 0) {
        self->te_period_us_ = (uint32_t)(now - self->te_last_us_);
    }
    self->te_last_us_ = now;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->te_sem_, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t Canvas::enable_tearing_sync(int te_gpio)
{
    te_sem_ = xSemaphoreCreateBinary();
    if (te_sem_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << te_gpio);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_POSEDGE;

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    // Der Dienst kann bereits laufen, wenn ihn jemand anders installiert hat —
    // das ist kein Fehler.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = gpio_isr_handler_add((gpio_num_t)te_gpio, te_isr, this);
    if (err != ESP_OK) return err;

    te_gpio_ = te_gpio;
    return ESP_OK;
}

void Canvas::flush()
{
    if (te_sem_ != nullptr) {
        // Eine eventuell schon anstehende Flanke verwerfen, sonst wuerde auf
        // ein Ereignis synchronisiert, das beim Zeichnen bereits vorbei war.
        xSemaphoreTake(te_sem_, 0);

        // Mit Zeitschranke: faellt das Signal aus, soll die Anzeige langsamer
        // werden, nicht stehen bleiben.
        if (xSemaphoreTake(te_sem_, pdMS_TO_TICKS(100)) != pdTRUE) {
            te_timeouts_++;
        }
    }

    // Kehrt erst zurueck, wenn das DMA den Puffer fertig gelesen hat. Sonst
    // wuerde der Aufrufer waehrend der laufenden Uebertragung schon wieder
    // hineinzeichnen; siehe display_sync.h.
    d_.send_and_wait();
}

void Canvas::pixel(int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    d_.punkt(x, y, color);
}

// Die Linienfunktionen clippen einmal vorab statt sich auf pixel() zu
// verlassen. Bei einem Visualizer, der das Bild mehrmals pro Sekunde neu
// aufbaut, spart das den Zweig je Pixel und begrenzt zugleich die Laufzeit,
// wenn eine Koordinate weit ausserhalb liegt.

void Canvas::hline(int x0, int x1, int y, uint8_t color)
{
    if (y < 0 || y >= height_) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= width_) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= width_) x1 = width_ - 1;

    for (int x = x0; x <= x1; x++) {
        d_.punkt(x, y, color);
    }
}

void Canvas::vline(int x, int y0, int y1, uint8_t color)
{
    if (x < 0 || x >= width_) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < 0 || y0 >= height_) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= height_) y1 = height_ - 1;

    for (int y = y0; y <= y1; y++) {
        d_.punkt(x, y, color);
    }
}

// Bresenham. Clippt ueber pixel(), weil eine vorherige Streckenkuerzung hier
// die Steigung veraendern wuerde.
void Canvas::line(int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy > 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::rect(int x0, int y0, int x1, int y1, uint8_t color)
{
    hline(x0, x1, y0, color);
    hline(x0, x1, y1, color);
    vline(x0, y0, y1, color);
    vline(x1, y0, y1, color);
}

void Canvas::fill_rect(int x0, int y0, int x1, int y1, uint8_t color)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= height_) y1 = height_ - 1;

    for (int y = y0; y <= y1; y++) {
        hline(x0, x1, y, color);
    }
}

void Canvas::bitmap(int x, int y, int w, int h, const uint8_t *bits)
{
    if (x == 0 && y == 0 && w == width_ && h == height_ && d_.vollbild(bits)) return;

    const int bytes_je_zeile = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        const uint8_t *zeile = bits + j * bytes_je_zeile;
        for (int i = 0; i < w; i++) {
            const bool schwarz = (zeile[i / 8] & (0x80 >> (i % 8))) != 0;
            pixel(x + i, y + j, schwarz ? ColorBlack : ColorWhite);
        }
    }
}

// Kreise ueber die Bedingung dx^2 + dy^2 <= r^2, Zeile fuer Zeile. Der
// Mittelpunktalgorithmus waere schneller, aber bei Radien um zehn Pixel geht
// es hier um einige hundert Vergleiche je Bild — dafuer lohnt kein Verfahren,
// das man beim Lesen erst nachvollziehen muss.
void Canvas::fill_circle(int cx, int cy, int r, uint8_t color)
{
    if (r < 0) return;

    const int rr = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = r;
        while (dx > 0 && dx * dx + dy * dy > rr) dx--;
        hline(cx - dx, cx + dx, cy + dy, color);
    }
}

void Canvas::circle(int cx, int cy, int r, uint8_t color)
{
    if (r < 0) return;

    const int rr = r * r;
    int       vorher = -1;
    for (int dy = -r; dy <= r; dy++) {
        int dx = r;
        while (dx > 0 && dx * dx + dy * dy > rr) dx--;

        // Ein Pixel je Zeilenende genuegt nur, solange die Kontur steil
        // verlaeuft. Oben und unten springt dx um mehrere Spalten — dort
        // muss die Luecke zur vorigen Zeile gefuellt werden, sonst zerfaellt
        // der Kreis in einzelne Punkte.
        if (vorher >= 0 && dx > vorher + 1) {
            hline(cx + vorher, cx + dx, cy + dy, color);
            hline(cx - dx, cx - vorher, cy + dy, color);
        } else if (vorher > dx + 1) {
            hline(cx + dx, cx + vorher, cy + dy - 1, color);
            hline(cx - vorher, cx - dx, cy + dy - 1, color);
            pixel(cx + dx, cy + dy, color);
            pixel(cx - dx, cy + dy, color);
        } else {
            pixel(cx + dx, cy + dy, color);
            pixel(cx - dx, cy + dy, color);
        }
        vorher = dx;
    }
}

// --- UTF-8 --------------------------------------------------------------
//
// Der Text aus der Transkription kommt als UTF-8 herein, der Font kennt aber
// nur ASCII plus vier deutsche Sonderzeichen. next_glyphs() macht aus der
// naechsten Zeichenfolge einen oder zwei Glyphenindizes und setzt p weiter.
// Indizes unter 0x100 sind ASCII, darueber Sonderzeichen.

static const uint8_t *glyph_data(int g)
{
    if (g >= 0x100) {
        const int i = g - 0x100;
        if (i < 0 || i >= kFontExtraCount) return &kFont5x7[('?' - 0x20) * kFontWidth];
        return &kFont5x7Extra[i * kFontWidth];
    }
    if (g < 0x20 || g > 0x7F) g = '?';
    return &kFont5x7[(g - 0x20) * kFontWidth];
}

static int next_glyphs(const unsigned char **p, int *out)
{
    const unsigned char c = **p;

    if (c < 0x80) {
        (*p)++;
        out[0] = (c < 0x20) ? '?' : c;
        return 1;
    }

    if (c == 0xC3 && (*p)[1] != 0) {
        const unsigned char d = (*p)[1];
        *p += 2;
        switch (d) {
            case 0xA4: out[0] = 0x100 + kGlyphAe; return 1;   // ae
            case 0xB6: out[0] = 0x100 + kGlyphOe; return 1;   // oe
            case 0xBC: out[0] = 0x100 + kGlyphUe; return 1;   // ue
            case 0x9F: out[0] = 0x100 + kGlyphSz; return 1;   // sz
            // Grosse Umlaute passen nicht in sieben Zeilen, siehe font5x7.h.
            case 0x84: out[0] = 'A'; out[1] = 'e'; return 2;
            case 0x96: out[0] = 'O'; out[1] = 'e'; return 2;
            case 0x9C: out[0] = 'U'; out[1] = 'e'; return 2;
            default:   out[0] = '?'; return 1;
        }
    }

    // Unbekannte Mehrbytefolge: Kopf- und Folgebytes zusammen ueberspringen,
    // sonst stuende je Byte ein Fragezeichen auf der Anzeige.
    (*p)++;
    while ((**p & 0xC0) == 0x80) (*p)++;
    out[0] = '?';
    return 1;
}

int Canvas::text_width(const char *s, int scale)
{
    if (s == nullptr) return 0;
    if (scale < 1) scale = 1;

    int                  n = 0;
    int                  g[2];
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0') n += next_glyphs(&p, g);

    if (n == 0) return 0;
    // Der Abstand hinter dem letzten Zeichen zaehlt nicht zur Textbreite.
    return n * kFontAdvance * scale - scale;
}

int Canvas::text_height(int scale)
{
    if (scale < 1) scale = 1;
    return kFontHeight * scale;
}

void Canvas::draw_range(int x, int y, const char *begin, const char *end,
                        uint8_t color, int scale)
{
    if (begin == nullptr) return;
    if (scale < 1) scale = 1;

    int                  cx = x;
    int                  g[2];
    const unsigned char *p = (const unsigned char *)begin;
    const unsigned char *e = (const unsigned char *)end;

    while (*p != '\0' && (e == nullptr || p < e)) {
        const int n = next_glyphs(&p, g);

        for (int k = 0; k < n; k++) {
            // Frueher Ausstieg, sobald das Zeichen rechts herausgelaufen ist.
            // Ohne das wuerde eine zu lange Zeichenkette die volle Laufzeit
            // kosten und nur von pixel() verworfen.
            if (cx >= width_) return;

            const uint8_t *glyph = glyph_data(g[k]);
            for (int col = 0; col < kFontWidth; col++) {
                const uint8_t bits = glyph[col];
                if (bits == 0) continue;
                for (int row = 0; row < kFontHeight; row++) {
                    if ((bits & (1 << row)) == 0) continue;
                    if (scale == 1) {
                        pixel(cx + col, y + row, color);
                    } else {
                        const int px = cx + col * scale;
                        const int py = y + row * scale;
                        fill_rect(px, py, px + scale - 1, py + scale - 1, color);
                    }
                }
            }
            cx += kFontAdvance * scale;
        }
    }
}

void Canvas::text(int x, int y, const char *s, uint8_t color, int scale)
{
    draw_range(x, y, s, nullptr, color, scale);
}

int Canvas::text_wrapped(int x, int y, int w, const char *s, uint8_t color,
                         int scale, int line_step, int max_lines)
{
    if (s == nullptr || *s == '\0' || max_lines < 1) return 0;
    if (scale < 1) scale = 1;

    const int adv  = kFontAdvance * scale;
    const int cols = (w + scale) / adv;   // Glyphen je Zeile
    if (cols < 1) return 0;

    // Erst umbrechen, dann zeichnen: bei einem laufenden Transkript ist das
    // Ende das Interessante, also muss die Gesamtzahl der Zeilen bekannt sein,
    // bevor die erste gezeichnet wird.
    static const int      kMaxLines = 40;
    const unsigned char  *anf[kMaxLines];
    const unsigned char  *ende[kMaxLines];
    int                   n = 0;

    const unsigned char *p          = (const unsigned char *)s;
    const unsigned char *zeilenanf  = p;
    const unsigned char *letztes_lz = nullptr;
    int                  belegt     = 0;
    int                  g[2];

    while (*p != '\0' && n < kMaxLines) {
        const unsigned char *vorher = p;
        if (*p == ' ') letztes_lz = p;

        const int k = next_glyphs(&p, g);
        if (belegt + k <= cols) {
            belegt += k;
            continue;
        }

        // Umbruch: bevorzugt am letzten Leerzeichen, sonst hart. Steht das
        // Zeichen allein und passt trotzdem nicht, wird es mitgenommen —
        // sonst kaeme die Schleife nicht voran.
        const unsigned char *brk = (letztes_lz != nullptr && letztes_lz > zeilenanf)
                                       ? letztes_lz
                                       : (vorher > zeilenanf ? vorher : p);

        anf[n]  = zeilenanf;
        ende[n] = brk;
        n++;

        zeilenanf = brk;
        while (*zeilenanf == ' ') zeilenanf++;
        p          = zeilenanf;
        letztes_lz = nullptr;
        belegt     = 0;
    }

    if (n < kMaxLines && *zeilenanf != '\0') {
        anf[n]  = zeilenanf;
        ende[n] = p;
        n++;
    }

    const int erste = (n > max_lines) ? (n - max_lines) : 0;
    int       zeile = 0;
    for (int i = erste; i < n; i++, zeile++) {
        draw_range(x, y + zeile * line_step, (const char *)anf[i],
                   (const char *)ende[i], color, scale);
    }
    return zeile;
}
