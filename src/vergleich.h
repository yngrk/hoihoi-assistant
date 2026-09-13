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
//
// ---------------------------------------------------------------------------
// Zwei Arten zu vergleichen, und warum gleichzeitig
//
// "ha ha", "he he" und "ho ho" weckten das Geraet ebenfalls. Der Verdacht
// faellt auf die Mittelwertbefreiung je Wort: sie zieht von jedem Koeffizienten
// sein Mittel ueber das Wort ab, und bei "ha ha" ist dieses Mittel fast ganz
// das A. Uebrig bleibt ein Muster aus zwei H-Stoessen und zwei Vokalen nahe
// null — und genau dasselbe Muster bleibt von "HoiHoi". Was die Woerter
// unterscheidet, die Vokalfarbe, ist das, was abgezogen wurde.
//
// Zwei Auswege lagen nahe:
//
//   Fest    Das Mittel nicht aus dem Kandidaten nehmen, sondern einmal aus
//           allen Vorlagen. Mikrofon und Raum fallen trotzdem heraus — sie
//           waren beim Einlernen dieselben —, die Vokalfarbe aber bleibt.
//   Delta   Zu jedem Koeffizienten seine Aenderung von Rahmen zu Rahmen. Das
//           O-I in "HoiHoi" ist eine Gleitbewegung, das A in "ha ha" steht.
//
// Gemessen wurden alle vier Kombinationen. "Fest" trennt, Delta brachte
// keiner der beiden Arten etwas — die Luecke blieb auf den Prozentpunkt
// gleich, bei doppelter Rechenzeit. Delta ist deshalb wieder heraus.
//
// Gerechnet werden weiter "Wort" und "Fest", ausgeloest wird nach kAusloeser.
// "Wort" bleibt als Vergleichsspalte im Log, damit
// sich ein Rueckfall sofort zeigt.
//
// Der Vergleich laeuft in einem eigenen Task auf dem anderen Kern. Im
// Aufnahmetask stehen 60 ms fuer alles zur Verfuegung, und ein langes Wort
// gegen vier Vorlagen kommt dem schon allein nahe.
//
// ---------------------------------------------------------------------------
// Dauerhaft abgelegt
//
// Die Vorlagen liegen im NVS, und zwar roh, ohne jede Mittelwertbefreiung.
// Das ist Absicht: welche Art am Ende ausloest, aendert dann nichts
// an dem, was gespeichert ist. Verworfen wird ein Stand nur, wenn sich die
// Merkmale selbst geaendert haben — Rahmenlaenge, Vorschub, Koeffizienten.
//
// Abgelegt als 16-Bit-Zahlen mit einem Faktor je Vorlage. Acht Vorlagen als
// float haetten gut 15 KB belegt, und die NVS-Partition hat 24 KB, von denen
// auch WLAN und Zugangsdaten leben. Der Rundungsfehler liegt bei einem
// Dreitausendstel des groessten Koeffizienten — die Abstaende, um die es geht,
// unterscheiden sich um ganze Einheiten.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <esp_err.h>

namespace vergleich {

// So oft wird das Wort eingesprochen, und zwar in zwei Lagen: kJeLage Mal nah
// am Geraet, kJeLage Mal aus etwa zwei Metern.
//
// Vier Vorlagen aus einer Lage reichten nicht. Dasselbe HoiHoi lag nach "Fest"
// bei 8,8 bis 10,9, solange man sprach wie beim Einlernen — aus einer anderen
// Position bei 13 bis 14, und in einer dritten Runde bei 22 bis 31, bis neu
// eingelernt wurde. Die Schwelle kann das nicht auffangen: "ha ha" beginnt
// bei 14,5. Also muss die andere Lage selbst unter den Vorlagen sein; es zaehlt
// ohnehin nur die naechstgelegene.
//
// Das feste Mittel der Art "Fest" gilt dabei je Lage und nicht ueber alle
// Vorlagen. Der erste Versuch mit einem gemeinsamen Mittel lag bei 15,9 bis
// 19,0 fuer jedes HoiHoi, schlechter als vorher mit vier Vorlagen: das Mittel
// aus nah und fern passt zu keiner der beiden Lagen. Der Kandidat wird deshalb
// fuer jede Lage mit deren Mittel vorbereitet und nur gegen ihre Vorlagen
// verglichen.
const int kJeLage    = 4;
const int kLagen     = 2;
const int kVorlagen  = kLagen * kJeLage;

// Kuerzer darf ein Wort beim Einlernen nicht sein. Ein HoiHoi lag bisher bei
// 300 bis 540 ms; eine Vorlage von 200 ms war ein halbes Wort und haette
// jedes kurze Geraeusch zwischen 100 und 400 ms zum Vergleich zugelassen.
const int32_t kMinLernMs = 260;
const int kMaxRahmen = 96;   // gut eine Sekunde

enum Art { kWort = 0, kFest = 1, kArten = 2 };

// Nach dieser Art wird ausgeloest, und ab dieser Schwelle gilt ein Kandidat
// als das Weckwort.
//
// Bis hierher war es "Wort" mit 9,5. Die erste Runde mit Stoerern hat das
// widerlegt: zehn HoiHoi lagen bei hoechstens 8,20, aber "he he" schon bei
// 9,13 und "ha ha" bei 9,39 — beide weckten. Dieselben Woerter nach "Fest":
//
//   HoiHoi   6,39 bis 10,99
//   ha ha   14,54 bis 26,67
//   he he   15,63 bis 23,43
//   ho ho   22,51 bis 29,16
//
// Eine Luecke von 32 % statt 11 %. Die Schwelle liegt in ihrer Mitte.
//
// Die Bestaetigungsrunde mit 12,75 und Marken per KEY: HoiHoi 10 von 10
// geweckt (8,83 bis 10,91), ha ha 0 von 5 (ab 15,61), he he 0 von 5 (ab
// 16,43), ho ho 0 von 6 (ab 17,62).
const int   kAusloeser = kFest;
const float kSchwelle  = 12.75f;

// Belegt die Tafeln, laedt die Vorlagen aus dem NVS und startet den Task.
esp_err_t bereit();

// Die naechsten kVorlagen Woerter sind das Weckwort. Ohne diesen Anstoss wird
// nichts eingelernt — sonst wuerde das erste Stuhlruecken nach dem Einschalten
// zur Vorlage. Der alte Stand im NVS bleibt, bis der neue vollstaendig ist.
void einlernen();

// Nur zur Diagnose: rechnet zwei kuenstliche Folgen durch und meldet, was
// dabei herauskommt. Laeuft beim Start, wo Loggen nichts kostet.
void selbsttest();

// Ein abgegrenzter Kandidat, roh — die Mittelwertbefreiung macht jetzt der
// Vergleich selbst, weil sie je nach Art verschieden ist. Aus dem
// Aufnahmetask: kopiert und kehrt sofort zurueck. false heisst, der vorige
// Kandidat wird noch gerechnet, und dieser faellt weg.
bool einreichen(const float *roh, int32_t rahmen, int32_t ms);

// Genau einmal true, nachdem ein Kandidat als Weckwort erkannt wurde.
bool geweckt();

// Wie viele Vorlagen schon stehen, und ob gerade eingelernt wird. Fuer die
// Anzeige.
int  eingelernt();
bool lernt();

// Das letzte bewertete Wort, fuer die Anzeige, nach kAusloeser. Ohne diese
// Zahlen bliebe auf dem Bild nur "erkannt" oder "nicht erkannt" — und daran
// ist nicht zu sehen, ob es knapp war. Geschrieben im Vergleichstask, gelesen
// im Anzeigetask: ausgerichtete 32-Bit-Worte, dieselbe Ueberlegung wie bei den
// Umweltwerten.
int32_t letzter_abstand();   // x100; -1 heisst: die Laenge passte zu keiner
int32_t letzte_dauer();      // ms
bool    letzter_treffer();
uint32_t bewertet();         // wie viele Woerter ueberhaupt bewertet wurden
uint32_t treffer();          // und wie viele davon das Weckwort waren

// ---------------------------------------------------------------------------
// Testbetrieb: Woerter unter einer Marke sammeln
//
// Wer im Log nach Zeitstempeln sortieren muss, welches Wort "HoiHoi" und
// welches "ha ha" war, rechnet hinterher mit geratenen Grenzen. Stattdessen
// sagt man vorher, was jetzt kommt — ein Druck auf KEY schaltet die Marke
// weiter —, und jedes bewertete Wort wird unter ihr gezaehlt.
//
// Daraus ergibt sich je Art, was die Frage entscheidet: der groesste Abstand
// eines HoiHoi und der kleinste eines Stoerers. Liegt der zweite ueber dem
// ersten, trennt diese Art, und die Schwelle gehoert in die Luecke.
//
// Alle Zahlen x100. Geschrieben im Vergleichstask, gelesen in der Anzeige;
// die Marke schreibt der Aufnahmetask.
// ---------------------------------------------------------------------------

enum Marke { kHoi = 0, kHa = 1, kHe = 2, kHo = 3, kMarken = 4 };

const char *marke_name(int marke);
int  marke();
void marke_weiter();
void statistik_leeren();

int32_t  letzt_art(int art);            // -1: keiner, oder Laenge passte nicht
int      letzt_marke();
int32_t  hoi_max(int art);              // -1: noch kein HoiHoi
int32_t  fremd_min(int art);            // -1: noch kein Stoerer
uint32_t gesagt(int marke);             // Woerter unter dieser Marke
uint32_t aufgewacht(int marke);         // davon ausgeloest (nach kAusloeser)

}  // namespace vergleich
