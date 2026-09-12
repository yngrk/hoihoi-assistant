#include "gfx.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_timer.h>

#include "font5x7.h"

Canvas::Canvas(DisplayPort &display, int width, int height)
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
    d_.RLCD_Display();
}

void Canvas::pixel(int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    d_.RLCD_SetPixel((uint16_t)x, (uint16_t)y, color);
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
        d_.RLCD_SetPixel((uint16_t)x, (uint16_t)y, color);
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
        d_.RLCD_SetPixel((uint16_t)x, (uint16_t)y, color);
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

int Canvas::text_width(const char *s, int scale)
{
    if (s == nullptr) return 0;
    if (scale < 1) scale = 1;
    const int n = (int)strlen(s);
    if (n == 0) return 0;
    // Der Abstand hinter dem letzten Zeichen zaehlt nicht zur Textbreite.
    return n * kFontAdvance * scale - scale;
}

int Canvas::text_height(int scale)
{
    if (scale < 1) scale = 1;
    return kFontHeight * scale;
}

void Canvas::text(int x, int y, const char *s, uint8_t color, int scale)
{
    if (s == nullptr) return;
    if (scale < 1) scale = 1;

    int cx = x;
    for (const char *p = s; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7F) c = '?';
        const uint8_t *glyph = &kFont5x7[(c - 0x20) * kFontWidth];

        // Frueher Ausstieg, sobald das Zeichen rechts herausgelaufen ist. Ohne
        // das wuerde eine zu lange Zeichenkette die volle Laufzeit kosten und
        // nur von pixel() verworfen.
        if (cx >= width_) return;

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
