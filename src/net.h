#pragma once

// ---------------------------------------------------------------------------
// WLAN im Stationsbetrieb, mit mehreren bekannten Netzen.
//
// Nur so viel, wie die Transkription braucht: verbinden, verbunden bleiben,
// und im Zweifel sagen, woran es liegt. begin() kehrt sofort zurueck — der
// Verbindungsaufbau dauert je nach Netz ein bis mehrere Sekunden, und darauf
// soll weder der Bring-up noch die Anzeige warten.
//
// Welches der bekannten Netze gerade erreichbar ist, entscheidet die
// Umgebung: mal die Wohnung, mal der Hotspot des Telefons. Das Geraet sucht
// deshalb erst und verbindet sich dann mit dem staerksten Netz, das es
// kennt — blindes Durchprobieren waere bei vier Eintraegen langsam und
// wuerde bei gleichzeitig erreichbaren Netzen das falsche nehmen.
//
// Ist keines davon da, geht die Bereitstellung ueber Bluetooth auf (prov.h).
// Das ist kein Betriebsmodus, sondern der einzige Weg zurueck: ohne ihn waere
// ein Geraet ohne Tastatur und mit falschem WLAN-Passwort nur noch per USB zu
// erreichen.
// ---------------------------------------------------------------------------

#include <esp_err.h>

namespace net {

// Nimmt die Netze aus cfg. Ohne ein einziges bekanntes Netz geht es direkt
// in die Bereitstellung — es gaebe nichts zu versuchen.
esp_err_t begin();

bool connected();

// True, solange die Bereitstellung laeuft und auf Zugangsdaten wartet.
bool provisioning();

// IP als Text, oder "-" wenn es keine gibt. Fuer die Anzeige.
const char *ip();

// Kurzer Zustandstext, geeignet fuer die Kopfzeile.
const char *status();

}  // namespace net
