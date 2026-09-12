#pragma once

// ---------------------------------------------------------------------------
// Das Weckwort, Schritt eins: Woerter aus dem laufenden Ton herausschneiden.
//
// Ein Vergleich mit eingelernten Vorlagen kann nur so gut sein wie die
// Abgrenzung davor. Wer nicht weiss, wo das Wort anfaengt und aufhoert,
// vergleicht Vorlagen mit Stille am Anfang und einer halben Silbe am Ende —
// und bekommt Abstaende, aus denen sich keine Schwelle ableiten laesst.
//
// Abgegrenzt wird ueber die Energie, nicht ueber ein Modell. Das reicht fuer
// ein einzelnes Wort vor einem ruhigen Hintergrund und kostet nichts: ein
// quadratischer Mittelwert je 20-ms-Block, den der Aufnahmetask ohnehin
// berechnet.
//
// Der Ruhepegel wird dabei nicht geraten, sondern nachgefuehrt. Ein festes
// Mass waere entweder im stillen Zimmer taub oder neben einem Luefter
// dauerhaft ausgeloest. Nachgefuehrt wird nur in der Stille — waehrend
// gesprochen wird, bleibt der Wert stehen, sonst zoege sich die Schwelle an
// der eigenen Stimme hoch.
//
// Was hier herauskommt, ist noch kein Weckwort, sondern ein Kandidat: ein
// Abschnitt zwischen 200 und 1600 ms. Was davon "HoiHoi" ist, entscheidet
// Schritt vier.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

namespace wachwort {

// Einmal vor dem ersten feed(). Legt die Tabellen fuer MFCC an und den Ring,
// in dem die Merkmale der letzten Sekunden stehen.
esp_err_t bereit();

// PCM aus dem Aufnahmetask, blockweise. Darf nicht loggen — siehe nachtrag.h.
void feed(const int16_t *pcm, size_t frames, uint32_t rate);

// Das Mikrofon war zu (der Lautsprecher hatte den Port). Danach stimmt weder
// der Ruhepegel noch ein angefangenes Wort.
void ruhe();

// Wie viele Kandidaten bisher erkannt wurden. Fuer die Anzeige.
uint32_t woerter();

}  // namespace wachwort
