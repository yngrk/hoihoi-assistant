#pragma once

// ---------------------------------------------------------------------------
// WLAN-Zugangsdaten ueber Bluetooth LE hereinreichen.
//
// Das Geraet hat keine Tastatur. Ein WLAN-Passwort muss trotzdem irgendwie
// hinein, und zwar auch dann, wenn gar kein Netz erreichbar ist — sonst waere
// ein Umzug oder ein geaendertes Passwort nur noch per USB zu beheben.
//
// Espressifs wifi_provisioning macht das fertig: BLE als Transportweg, ein
// x25519-Handschlag mit Kennwortnachweis darueber, und passende Apps von
// Espressif fuer iOS und Android. Der Alternativweg — eigener Zugangspunkt,
// eigener HTTP-Server, eigene Seite — waere mehr Code, unverschluesselt und
// zwaenge das Telefon dazu, das Netz zu wechseln.
//
// BLE statt des ebenfalls moeglichen Zugangspunkts, weil das Telefon dabei
// in seinem eigenen Netz bleibt. Der Bluetooth-Stack wird nach getaner
// Arbeit wieder freigegeben; im Normalbetrieb laeuft hier nichts.
//
// Wichtig: dieser Weg reicht genau ein Netz herein. Das Geraet soll aber
// mehrere kennen (Wohnung, Hotspot), deshalb landen die Zugangsdaten nicht
// nur im WLAN-Treiber, sondern zusaetzlich in cfg — siehe cfg.h.
// ---------------------------------------------------------------------------

#include <esp_err.h>

namespace prov {

// Startet den Dienst. Setzt ein initialisiertes WLAN voraus (esp_wifi_init),
// aber keine laufende Verbindung. Kehrt sofort zurueck.
esp_err_t begin();

bool running();

// Geraetename, unter dem das Telefon es findet ("HOIHOI-A1B2"), und der
// Kennwortnachweis, den die App abfragt. Beide gehoeren auf das Display —
// ein Geraet, das seinen eigenen Namen nicht anzeigt, muss man raten.
const char *device_name();
const char *pop();

}  // namespace prov
