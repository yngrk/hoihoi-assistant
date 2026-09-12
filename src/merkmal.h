#pragma once

// ---------------------------------------------------------------------------
// Das Weckwort, Schritt zwei: Merkmale statt Rohton.
//
// Zwei Aufnahmen desselben Wortes sind als Abtastwerte voellig verschieden —
// eine Verschiebung um eine halbe Schwingung dreht jedes Vorzeichen um, ohne
// dass sich fuer das Ohr etwas aendert. Verglichen wird deshalb nicht der Ton,
// sondern wie sich sein Klang ueber die Zeit veraendert.
//
// MFCC ist dafuer das uebliche Mass und seit Jahrzehnten das erste, was jede
// Spracherkennung rechnet: Spektrum, dann Zusammenfassen in Baender nach der
// Mel-Skala (unten fein, oben grob — so wie das Ohr es tut), dann Logarithmus
// (doppelte Lautstaerke ist ein fester Zuschlag, kein Faktor), dann eine
// Kosinustransformation, die die Baender wieder entkoppelt.
//
// Uebrig bleiben zwoelf Zahlen je Rahmen. Der erste Koeffizient faellt weg: er
// ist nur die Gesamtlautstaerke und saehe bei leisem und lautem "HoiHoi"
// verschieden aus, obwohl es dasselbe Wort ist.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <esp_err.h>

namespace merkmal {

const int kRate  = 24000;
const int kFft   = 512;   // 21,3 ms Fenster
const int kHop   = 256;   // 10,7 ms Vorschub, also halbe Ueberlappung
const int kBand  = 26;    // Mel-Baender zwischen 100 und 8000 Hz
const int kKoeff = 12;    // c1..c12

// Einmal beim Start. Legt Fenster, Filterbank, Kosinustabelle und die
// Drehfaktoren der FFT an.
esp_err_t bereit();

// kFft Abtastwerte hinein, kKoeff Zahlen heraus. Laeuft im Aufnahmetask und
// darf deshalb nicht loggen und nichts belegen.
void rahmen(const int16_t *x, float *aus);

}  // namespace merkmal
