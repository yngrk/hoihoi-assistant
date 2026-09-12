#pragma once

// ---------------------------------------------------------------------------
// Das Log mitschneiden, damit es auf dem Display landen kann.
//
// Bis hierher stand alles Interessante auf der seriellen Schnittstelle, und
// die haengt am Rechner. Ein Geraet, das man in die Hand nimmt, soll aber
// selbst sagen koennen, was es gerade tut — sonst ist jeder Fehler nur am
// Schreibtisch zu sehen.
//
// ESP-IDF laesst die Logausgabe ueber esp_log_set_vprintf() umlenken. Hier
// wird sie nicht umgelenkt, sondern abgezweigt: die Originalfunktion bekommt
// weiterhin jede Zeile, zusaetzlich landet sie in einem Ringspeicher. Der
// Mitschnitt kostet damit nichts an Diagnose auf dem Rechner.
//
// Der Ring haelt die letzten kLines abgeschlossenen Zeilen. Aeltere fallen
// heraus — auf 300 Pixel Hoehe passen ohnehin nur knapp dreissig.
// ---------------------------------------------------------------------------

#include <esp_err.h>
#include <stddef.h>

namespace logview {

// Laenge einer gespeicherten Zeile ohne Nullbyte. Grosszuegiger als eine
// Bildschirmzeile breit ist: lange Meldungen werden beim Zeichnen umbrochen
// und sollen dafuer nicht schon im Speicher abgeschnitten sein.
static const int kCols  = 100;
static const int kLines = 40;

// Haengt sich in die Logausgabe. Je frueher in app_main, desto mehr vom
// Hochlauf ist spaeter auf dem Display zu sehen.
esp_err_t begin();

// Kopiert die letzten Zeilen heraus, aelteste zuerst. dst ist ein Feld von
// max_lines mal (kCols + 1) Bytes; Rueckgabe ist die Anzahl gefuellter
// Zeilen. Eine noch nicht mit Zeilenumbruch abgeschlossene Ausgabe kommt als
// letzte mit — sonst fehlte auf dem Display genau die Zeile, die gerade
// entsteht.
int snapshot(char *dst, int max_lines);

// Zeilen, die diesen Text enthalten, kommen nicht in den Ring. Gedacht fuer
// die eigenen Taktmeldungen: eine Zeile im Sekundentakt fuellt ein Display
// mit dreissig Zeilen in einer halben Minute restlos und verdraengt genau
// das, was man sehen will. Auf der seriellen Schnittstelle bleiben sie
// vollstaendig stehen — dort stoeren sie niemanden.
esp_err_t mute(const char *fragment);

}  // namespace logview
