#include "gfx.h"

Canvas::Canvas(DisplayPort &display, int width, int height)
    : d_(display), width_(width), height_(height)
{
}

void Canvas::clear(uint8_t color)
{
    d_.RLCD_ColorClear(color);
}

void Canvas::flush()
{
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
