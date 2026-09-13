#pragma once

// ---------------------------------------------------------------------------
// Die Oberflaeche im Stil von MapleStory: ein Fenster mit Datum in der
// Titelleiste, links HoiHoi im Helm-Portraet, rechts die grosse Uhr auf
// gerastertem Himmel, darunter das Wellenfeld und unten die schwarze
// Statusleiste mit Temperatur, Feuchte, Akku und WLAN.
//
// Antworten stehen nie als Text auf dem Display: ein langer Satz liefe aus
// jedem Feld. Was HoiHoi gerade tut, zeigen ein kurzes, festes Wort in der
// Kopfzeile des Wellenfelds und die Welle selbst — beim Zuhoeren folgt sie
// dem Mikrofon, beim Antworten der Stimme.
//
// Gezeichnet wird ganz nur, wenn sich etwas Sichtbares geaendert hat. Sonst
// laeuft nur die Welle weiter; der Rest steht noch im Puffer.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "gfx.h"

namespace oberflaeche {

enum class Zustand {
    Ruhe,        // wartet auf das Weckwort
    HoertZu,
    DenktNach,   // zwischen Frage und Antwort
    Antwortet,
};

// Kein Wert ist hier Pflicht: was nicht gueltig ist, erscheint als "--".
struct Stand {
    Zustand zustand = Zustand::Ruhe;

    // Mikrofon per Taste stummgeschaltet: das Weckwort hoert nicht zu.
    // Steht in Kopfzeile und Wellenfeld, die Welle liegt dann flach.
    bool stumm = false;

    bool zeit_gueltig = false;
    int  stunde = 0, minute = 0;
    int  wochentag = 0;             // 0 Sonntag, wie tm_wday
    int  tag = 1, monat = 1, jahr = 2026;

    bool    klima_gueltig = false;
    int32_t temp_c100     = 0;
    int32_t feuchte_100   = 0;

    int akku_prozent = -1;          // -1: kein Akku

    enum class Wlan { Aus, Einrichten, Verbunden };
    Wlan wlan = Wlan::Aus;
    int  rssi = 0;                  // nur bei Verbunden

    // Neuester Ausschlag der Welle, 0 bis 1. Wird bei jedem Aufruf von
    // zeichnen() eingeschoben, auch wenn sonst nichts neu ist.
    float pegel = 0.0f;
};

// Einmal je Bild. Zeichnet ganz, wenn sich gegenueber dem letzten Aufruf
// etwas Sichtbares geaendert hat, sonst nur das Wellenfeld. flush() bleibt
// beim Aufrufer.
void zeichnen(Canvas &c, const Stand &s);

}  // namespace oberflaeche
