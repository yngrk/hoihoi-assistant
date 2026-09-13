#pragma once

// ---------------------------------------------------------------------------
// Das Wetter draussen, fuer den Himmel hinter der Uhr und die Werte unten in
// der Leiste.
//
// Der Ort kommt einmal nach dem Verbinden aus der oeffentlichen IP
// (ip-api.com), das Wetter danach alle 15 Minuten von Open-Meteo. Beide
// Dienste brauchen keinen Schluessel, und beide gehen ueber einfaches HTTP:
// ein TLS-Handschlag kostete internen Speicher, und der ist auf diesem Geraet
// das Knappste. Inhaltlich ist nichts davon geheim.
//
// Abgerufen wird nur, wenn darf() ja sagt — waehrend eines Gespraechs soll
// keine weitere Verbindung um Speicher und Funkzeit konkurrieren.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace wetter {

enum class Art : int32_t {
    Unbekannt,
    Sonnig,
    Heiter,      // sonnig mit Wolken
    Bewoelkt,    // auch Nebel
    Niesel,
    Regen,       // auch Schauer
    Gewitter,
    Schnee,
};

struct Stand {
    bool    gueltig     = false;
    Art     art         = Art::Unbekannt;
    bool    nacht       = false;
    int32_t temp_c100   = 0;
    int32_t feuchte_100 = 0;
    int32_t wolken_pct  = 0;      // Bedeckung
    int32_t regen_mm10  = 0;      // Niederschlag der letzten Viertelstunde, Zehntel mm
    int32_t wind_kmh10  = 0;      // Wind in 10 m Hoehe, Zehntel km/h
    bool    nebel       = false;  // Code 45/48, in art als Bewoelkt
    bool    ort         = false;  // Ort bekannt, auch wenn das Wetter noch fehlt
    int32_t breite_100  = 0;      // Hundertstel Grad
    int32_t laenge_100  = 0;
};

// Das Gelaende um den Ort, fuer die Landschaft: einmal nach dem Ort aus den
// Hoehen eines Rings von Punkten bis 12 km (Open-Meteo Elevation).
struct Gelaende {
    bool     gueltig    = false;
    int32_t  hoehe_m    = 0;      // am Ort selbst
    int32_t  relief_m   = 0;      // hoechster minus tiefster Punkt im Ring
    int32_t  bergigkeit = 0;      // 0 flach .. 1000 Hochgebirge
    uint32_t keim       = 0;      // aus der Position, damit jeder Ort seine eigene Landschaft hat
};

// Startet den Abruf-Task. darf wird aus diesem Task heraus gefragt.
void begin(bool (*darf)());

Stand    stand();
Gelaende gelaende();

}  // namespace wetter
