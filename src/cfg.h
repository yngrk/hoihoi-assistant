#pragma once

// ---------------------------------------------------------------------------
// Bekannte WLAN-Netze im NVS.
//
// Der Internetzugang ist das einzige, was sich am Geraet aendert: es steht
// mal in der Wohnung, mal am Telefon-Hotspot. Alles andere — API-Schluessel,
// Modell, Sprache — bleibt in secrets.h, weil es sich nicht aendert und im
// Abbild besser aufgehoben ist als in einem beschreibbaren Speicher.
//
// Mehrere Netze, weil nicht der Benutzer entscheidet, welches davon gerade
// da ist, sondern die Umgebung. Das Geraet probiert sie der Reihe nach durch.
//
// secrets.h dient als Startbelegung: beim ersten Start wandert ein dort
// eingetragenes Netz einmalig ins NVS. Danach gilt das NVS — sonst kaeme ein
// ueber die Webseite geloeschtes Netz beim naechsten Neustart wieder zurueck.
// ---------------------------------------------------------------------------

#include <esp_err.h>

namespace cfg {

// Vier Netze reichen fuer Wohnung, Telefon und zwei Ausweichfaelle. Die
// Grenze kostet nichts ausser Platz im NVS, aber irgendwo muss sie liegen.
static const int kMaxNets = 4;

// Groessen aus dem Standard: 32 Zeichen SSID, 63 Zeichen WPA2-Passwort,
// jeweils plus Nullbyte.
struct Netz {
    char ssid[33];
    char pass[65];
};

// Liest die Netze ein und saet beim ersten Start aus secrets.h.
// Braucht ein initialisiertes NVS.
esp_err_t begin();

int         net_count();
const Netz *net_at(int i);

// Legt ein Netz an oder ueberschreibt das gleichnamige. Ist kein Platz mehr,
// faellt das aelteste heraus — bei vier Plaetzen ist eine Verwaltung dafuer
// mehr Bedienaufwand, als sie erspart.
esp_err_t net_add(const char *ssid, const char *pass);
esp_err_t net_remove(const char *ssid);

}  // namespace cfg
