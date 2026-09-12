#pragma once

// ---------------------------------------------------------------------------
// Melden, ohne zu warten.
//
// Der Aufnahmetask darf nicht ins Log schreiben. Eine einzige Zeile kostet
// ueber die serielle Schnittstelle Zehntelsekunden — gemessen wurden 713 ms
// fuer eine Ausgabe, waehrend der Rechner am anderen Ende nicht las. Die
// Konsole haengt an UART0 mit 115200 Baud und zusaetzlich am USB-Anschluss;
// geschrieben wird blockierend, also wartet der schreibende Task, bis beide
// Seiten abgenommen haben.
//
// In dieser Zeit laeuft der DMA-Ring des I2S ueber. Er fasst 60 ms. Aus einem
// Tastendruck von 2069 ms wurden so 1160 ms Ton, und der Satzanfang fehlte —
// der Fehler, der monatelang als "manchmal schneidet es ab" auffiel.
//
// Deshalb legt der Aufnahmetask die fertige Zeile hier nur ab. Ausgegeben
// wird sie vom Sensortask auf Kern 0, wo Warten nichts kostet. Ein Ring mit
// einem Schreiber und einem Leser braucht dafuer keine Sperre: der Schreiber
// fasst nur den Schreibzeiger an, der Leser nur den Lesezeiger.
//
// Ist der Ring voll, faellt die Zeile weg. Das ist Absicht — eine Meldung zu
// verlieren ist harmlos, Ton zu verlieren nicht.
// ---------------------------------------------------------------------------

namespace nachtrag {

// stufe: 'I' oder 'W'. tag muss ein Literal sein, es wird nur der Zeiger
// gemerkt.
void schreiben(char stufe, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Gibt alles Abgelegte aus. Nur aus einem Task aufrufen, der warten darf.
void ausgeben();

}  // namespace nachtrag
