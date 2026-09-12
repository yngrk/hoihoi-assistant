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

// Ab hier gilt ein Kandidat als das Weckwort. Die Zahl ist gemessen und nicht
// geraten: zehn Mal "HoiHoi" lagen zwischen 5,94 und 8,02, die fremden Woerter
// derselben Runde bei 10,92, 12,92 und 13,65. Dazwischen liegt eine Luecke von
// knapp drei, und die Schwelle liegt in ihrer Mitte.
//
// Die Luecke stammt aus einer Stimme in einem Raum. Wer das Geraet woanders
// aufstellt, lernt neu ein — dann stimmt sie wieder.
const float kSchwelle = 9.5f;

esp_err_t bereit();

// Die naechsten kVorlagen Woerter sind das Weckwort. Ohne diesen Anstoss wird
// nichts eingelernt — sonst wuerde das erste Stuhlruecken nach dem Einschalten
// zur Vorlage.
void einlernen();

// Nur zur Diagnose: rechnet zwei kuenstliche Folgen durch und meldet, was
// dabei herauskommt. Laeuft beim Start, wo Loggen nichts kostet.
void selbsttest();

// Ein abgegrenzter Kandidat, bereits mittelwertbefreit. Laeuft im
// Aufnahmetask: nicht loggen, nichts belegen. true heisst: das war das
// Weckwort.
bool kandidat(const float *folge, int32_t rahmen, int32_t ms);

// Wie viele Vorlagen schon stehen, und ob gerade eingelernt wird. Fuer die
// Anzeige.
int  eingelernt();
bool lernt();

// Das letzte bewertete Wort, fuer die Anzeige. Ohne diese Zahlen bliebe auf
// dem Bild nur "erkannt" oder "nicht erkannt" — und daran ist nicht zu sehen,
// ob es knapp war. Geschrieben im Aufnahmetask, gelesen im Anzeigetask:
// ausgerichtete 32-Bit-Worte, dieselbe Ueberlegung wie bei den Umweltwerten.
int32_t letzter_abstand();   // x100; -1 heisst: die Laenge passte zu keiner
int32_t letzte_dauer();      // ms
bool    letzter_treffer();
uint32_t bewertet();         // wie viele Woerter ueberhaupt bewertet wurden
uint32_t treffer();          // und wie viele davon das Weckwort waren

}  // namespace vergleich
