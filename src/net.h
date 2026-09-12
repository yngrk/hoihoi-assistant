#pragma once

// ---------------------------------------------------------------------------
// WLAN im Stationsbetrieb.
//
// Nur so viel, wie die Transkription braucht: verbinden, verbunden bleiben,
// und im Zweifel sagen, woran es liegt. begin() kehrt sofort zurueck — der
// Verbindungsaufbau dauert je nach Netz ein bis mehrere Sekunden, und darauf
// soll weder der Bring-up noch die Anzeige warten.
//
// Bei Verbindungsverlust wird neu versucht, mit wachsendem Abstand. Ein
// Geraet, das sekuendlich einen Verbindungsversuch startet, blockiert sich
// selbst und stoert das Funknetz.
// ---------------------------------------------------------------------------

#include <esp_err.h>

namespace net {

// Leerer ssid schaltet WLAN ab und liefert ESP_ERR_INVALID_ARG — gedacht fuer
// eine nicht ausgefuellte secrets.h.
esp_err_t begin(const char *ssid, const char *password);

bool connected();

// Kurzer Zustandstext, geeignet fuer die Kopfzeile.
const char *status();

}  // namespace net
