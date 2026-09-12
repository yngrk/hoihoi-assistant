#pragma once

// ---------------------------------------------------------------------------
// Das Weckwort, Schritt vier: passt der Kandidat zu einer Vorlage?
//
// Zwei Aufnahmen desselben Wortes sind nie gleich lang, und sie sind auch
// nicht gleichmaessig gedehnt — die Vokale ziehen sich, die Konsonanten nicht.
// Rahmen gegen Rahmen zu vergleichen geht deshalb schief, sobald jemand das
// Wort einmal etwas langsamer sagt.
//
// Dynamic Time Warping sucht stattdessen den guenstigsten Weg durch die Tafel
// aller Rahmenpaare: jeder Rahmen der Vorlage darf auf einen oder mehrere
// Rahmen des Kandidaten fallen, solange die Reihenfolge stimmt. Was
// herauskommt, ist ein Abstand je Rahmen — vergleichbar auch zwischen
// verschieden langen Woertern.
//
// Eingelernt wird mit der eigenen Stimme. Das erkennt dafuer auch nur die
// eigene Stimme, und das ist hier keine Einschraenkung, sondern der Zweck.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <esp_err.h>

namespace vergleich {

const int kVorlagen  = 4;    // so oft wird das Wort eingesprochen
const int kMaxRahmen = 96;   // gut eine Sekunde

esp_err_t bereit();

// Die naechsten kVorlagen Woerter sind das Weckwort. Ohne diesen Anstoss wird
// nichts eingelernt — sonst wuerde das erste Stuhlruecken nach dem Einschalten
// zur Vorlage.
void einlernen();

// Nur zur Diagnose: rechnet zwei kuenstliche Folgen durch und meldet, was
// dabei herauskommt. Laeuft beim Start, wo Loggen nichts kostet.
void selbsttest();

// Ein abgegrenzter Kandidat, bereits mittelwertbefreit. Laeuft im
// Aufnahmetask: nicht loggen, nichts belegen.
void kandidat(const float *folge, int32_t rahmen, int32_t ms);

// Wie viele Vorlagen schon stehen, und ob gerade eingelernt wird. Fuer die
// Anzeige.
int  eingelernt();
bool lernt();

}  // namespace vergleich
