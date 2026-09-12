#pragma once

// ---------------------------------------------------------------------------
// 5x7-Bitmapfont, ASCII 0x20 bis 0x7F.
//
// Fuenf Bytes je Zeichen, ein Byte je Spalte, Bit 0 ist die oberste Zeile.
// Gezeichnet wird spaltenweise von links, sechste Spalte bleibt als Abstand
// frei — deshalb ist der Vorschub 6 und nicht 5.
//
// Bewusst ein eigener Font statt LVGL: das Display hat ein Bit je Pixel und
// braucht fuer Zahlen und kurze Bezeichner keine Textengine. So bleiben die
// Clipping- und TE-Logik in Canvas unveraendert gueltig.
//
// 0x7F ist kein DEL, sondern ein Gradzeichen — fuer die Temperaturanzeige.
// ---------------------------------------------------------------------------

#include <stdint.h>

static const int kFontWidth   = 5;
static const int kFontHeight  = 7;
static const int kFontAdvance = 6;

extern const uint8_t kFont5x7[96 * 5];

// Deutsche Sonderzeichen ausserhalb von ASCII. Die grossen Umlaute fehlen
// absichtlich: ein Grossbuchstabe belegt alle sieben Zeilen, fuer die Punkte
// bleibt keine frei. Canvas::text() schreibt sie stattdessen um (Ae, Oe, Ue).
enum {
    kGlyphAe = 0,
    kGlyphOe,
    kGlyphUe,
    kGlyphSz,
    kFontExtraCount
};

extern const uint8_t kFont5x7Extra[kFontExtraCount * 5];
