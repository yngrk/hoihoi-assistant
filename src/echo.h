#pragma once

// ---------------------------------------------------------------------------
// Echounterdrueckung: waehrend einer Antwort hineinsprechen koennen.
//
// Bisher war das Mikrofon zu, solange der Lautsprecher lief. Das hatte zwei
// Gruende, und beide sind jetzt weg: der I2S-Port liess sich nicht fuer beide
// zugleich oeffnen (siehe audio.h), und was das Mikrofon gehoert haette, waere
// vor allem die eigene Stimme gewesen.
//
// Gegen das Zweite gibt es die Referenz: die Platine fuehrt das
// Lautsprechersignal auf einen Eingang des ES7210 zurueck. esp_aec aus
// Espressifs esp-sr schaetzt daraus, was davon ueber Gehaeuse und Raum im
// Mikrofon ankommt, und zieht es ab. Uebrig bleibt, was im Raum sonst
// gesprochen wird.
//
// Das Verfahren folgt xiaozhi-esp32 auf derselben Platine: esp_aec im Modus
// FD_LOW_COST (fuer Vollduplex, also Gegensprechen, gedacht) mit der
// staerksten Nachunterdrueckung. esp_aec rechnet nur mit 16 kHz; Mikrofon und
// Referenz werden deshalb von 24 kHz heruntergetaktet und das Ergebnis wieder
// herauf, denn Aufnahme und Erkennung bleiben bei 24 kHz.
//
// Aus dem gereinigten Ton entscheidet ein Pegelvergleich, ob jemand sprechen
// koennte — dieselbe Regel wie beim Satzende (vierfache Ruhe, fester Boden),
// aber mit einem Zaehler, der ueber Sprechpausen hinweg traegt. Das ist nur
// ein Verdacht. Der erste Versuch brach darauf hin sofort ab, und das war
// viel zu empfindlich: ein Stuhlruecken, ein Klopfen auf den Tisch oder der
// Rest des eigenen Echos in der ersten Sekunde, bevor sich das Filter
// eingestellt hat, reichten.
//
// Jetzt folgt auf den Verdacht eine Pruefaufnahme, mitsamt dem Vorspann (dem
// gereinigten Ton der letzten kVorspannMs, in dem der Satzanfang steht). Die
// Antwort laeuft leiser weiter, die Aufnahme geht zur Erkennung, und
// abgebrochen wird erst, wenn im Text wirklich Worte stehen — und nicht die
// der Antwort selbst, die als Echo durchgerutscht sind. Das entscheidet
// main.cpp (hineingesprochen()).
//
// Das Weckwort hoert waehrend einer Antwort nicht mit. Seine Vorlagen stammen
// vom ungereinigten Mikrofon, und die Nachunterdrueckung veraendert gerade das
// Spektrum, auf dem sie beruhen.
//
// Alles ausser bereit() laeuft im Aufnahmetask und schreibt nur ueber
// nachtrag ins Log.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

namespace echo {

// So viel gereinigter Ton steht vor einer Unterbrechung zur Verfuegung. Bis der
// Zaehler anschlaegt, vergeht bei zusammenhaengender Sprache gut eine halbe
// Sekunde; der Rest ist Sicherheit fuer den Anlaut.
const int32_t kVorspannMs = 800;

// So viel Sprache muss der Zaehler gesammelt haben. Er steigt mit jedem lauten
// Abschnitt um dessen Dauer und faellt in leisen um die Haelfte davon.
const int32_t kUnterbrechMs = 200;

// Nach dem ersten Ton der Antwort muss sich das Filter erst einstellen:
// gemessen daempfte es in der ersten Sekunde oft nur 6 bis 12 dB, danach 20
// bis 48. In dieser Zeit muss ein Block kAnlaufFaktor mal ueber der Schwelle
// liegen. 1500 ms waren zu kurz: bei einer kurzen Antwort nach langer Stille
// stand die Daempfung nach 2,5 s erst bei 15 dB, 68 ms nach dem Anlauf kam
// der Verdacht, und die Erkennung machte aus dem Echorest "GitHub ist ein."
const int32_t kAnlaufMs     = 3000;
const int32_t kAnlaufFaktor = 4;

// Tabellen, Filter und Puffer. Im Haupttask, vor dem Aufnahmetask.
esp_err_t bereit();
bool      aktiv();

// Eine Antwort beginnt. ruhe ist der Ruhepegel des Weckworts, ausgleich der
// Faktor, um den das Mikrofon waehrend der Antwort leiser gestellt ist — der
// gereinigte Ton wird damit wieder auf den gewohnten Pegel gebracht, damit
// Schwellen und Aufnahme dieselben Zahlen sehen wie sonst.
void beginnen(int32_t ruhe, float ausgleich);

// Ein Block Mikrofon und Referenz, beide 24 kHz. Liefert, was an gereinigtem
// Ton fertig geworden ist (24 kHz, hoechstens aus_max Frames). Die Menge
// schwankt, weil esp_aec in eigenen Bloecken rechnet.
size_t verarbeiten(const int16_t *mic, const int16_t *ref, size_t n,
                   int16_t *aus, size_t aus_max);

// Seit beginnen() oder weiter() koennte in die Antwort hineingesprochen
// worden sein. Bleibt dann stehen.
bool unterbrochen();

// Die Pruefaufnahme ist vorbei, und die Antwort laeuft noch: neuer Verdacht
// moeglich.
void weiter();

// Die letzten kVorspannMs gereinigten Tons, aeltester zuerst.
size_t vorspann(int16_t *aus, size_t max);

// Die Antwort ist vorbei: Bilanz ins Log.
void beenden();

}  // namespace echo
