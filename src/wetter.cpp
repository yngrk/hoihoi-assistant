#include "wetter.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "net.h"

namespace wetter {
namespace {

const char *TAG = "wetter";

const int64_t kAbstandUs   = 15LL * 60 * 1000000;   // Open-Meteo rechnet in 15-Minuten-Schritten
const int64_t kNochmalUs   = 60LL * 1000000;        // nach einem Fehlschlag
const size_t  kMinIntern   = 20000;                 // darunter wird nicht abgerufen
const size_t  kAntwortMax  = 2048;

bool (*darf_)() = nullptr;

// Vom Task geschrieben, vom Anzeigetask gelesen. Einzelne 32-Bit-Worte wie
// bei den Umweltwerten in main.cpp; ein Bild mit halb neuem Stand faellt nicht
// auf, und gueltig wird zuletzt gesetzt.
volatile int32_t v_gueltig = 0;
volatile int32_t v_art     = 0;
volatile int32_t v_nacht   = 0;
volatile int32_t v_temp    = 0;
volatile int32_t v_feuchte = 0;
volatile int32_t v_wolken  = 0;
volatile int32_t v_regen   = 0;
volatile int32_t v_wind    = 0;
volatile int32_t v_nebel   = 0;
volatile int32_t v_ort     = 0;
volatile int32_t v_breite  = 0;
volatile int32_t v_laenge  = 0;

volatile int32_t v_g_gueltig    = 0;
volatile int32_t v_g_hoehe      = 0;
volatile int32_t v_g_relief     = 0;
volatile int32_t v_g_bergigkeit = 0;
volatile int32_t v_g_keim       = 0;

// Liest eine Antwort ganz in puf. Rueckgabe: Laenge, oder -1.
int holen(const char *url, char *puf, size_t n)
{
    esp_http_client_config_t cfg = {};
    cfg.url         = url;
    cfg.timeout_ms  = 8000;
    cfg.buffer_size = 1024;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == nullptr) return -1;

    int laenge = -1;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        const int status = esp_http_client_get_status_code(h);
        int       gelesen = 0;
        while (gelesen < (int)n - 1) {
            const int r = esp_http_client_read(h, puf + gelesen, (int)n - 1 - gelesen);
            if (r <= 0) break;
            gelesen += r;
        }
        puf[gelesen] = '\0';
        if (status == 200 && gelesen > 0) {
            laenge = gelesen;
        } else {
            ESP_LOGW(TAG, "HTTP %d von %.*s", status, 40, url);
        }
    } else {
        ESP_LOGW(TAG, "Keine Verbindung zu %.*s", 40, url);
    }
    esp_http_client_cleanup(h);
    return laenge;
}

// WMO-Wetterschluessel, wie Open-Meteo sie liefert.
Art art_aus_code(int code)
{
    if (code == 0) return Art::Sonnig;
    if (code <= 2) return Art::Heiter;
    if (code <= 48) return Art::Bewoelkt;        // 3 bedeckt, 45/48 Nebel
    if (code <= 57) return Art::Niesel;          // 51..57, auch gefrierend
    if (code <= 67) return Art::Regen;           // 61..67
    if (code <= 77) return Art::Schnee;          // 71..77
    if (code <= 82) return Art::Regen;           // Schauer
    if (code <= 86) return Art::Schnee;          // Schneeschauer
    return Art::Gewitter;                        // 95..99
}

const char *art_text(Art a)
{
    switch (a) {
    case Art::Sonnig:   return "sonnig";
    case Art::Heiter:   return "heiter";
    case Art::Bewoelkt: return "bewoelkt";
    case Art::Niesel:   return "Niesel";
    case Art::Regen:    return "Regen";
    case Art::Gewitter: return "Gewitter";
    case Art::Schnee:   return "Schnee";
    default:            return "unbekannt";
    }
}

bool ort_ermitteln(char *puf, double *lat, double *lon)
{
    if (holen("http://ip-api.com/json/?fields=status,lat,lon,city", puf, kAntwortMax) < 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(puf);
    if (root == nullptr) return false;

    bool ok = false;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(root, "lat");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(root, "lon");
    const cJSON *ci = cJSON_GetObjectItemCaseSensitive(root, "city");
    if (cJSON_IsString(st) && strcmp(st->valuestring, "success") == 0
        && cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
        // Auf zwei Stellen, rund einen Kilometer: genauer ist die IP ohnehin
        // nicht, und genauer muss das Wetter nicht sein.
        *lat = round(la->valuedouble * 100.0) / 100.0;
        *lon = round(lo->valuedouble * 100.0) / 100.0;
        v_breite = (int32_t)lround(*lat * 100.0);
        v_laenge = (int32_t)lround(*lon * 100.0);
        v_ort    = 1;
        ok   = true;
        ESP_LOGI(TAG, "Ort aus der IP: %s.", cJSON_IsString(ci) ? ci->valuestring : "(ohne Namen)");
    }
    cJSON_Delete(root);
    return ok;
}

bool wetter_holen(char *puf, double lat, double lon)
{
    char url[260];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,is_day,cloud_cover,precipitation,wind_speed_10m",
             lat, lon);
    if (holen(url, puf, kAntwortMax) < 0) return false;

    cJSON *root = cJSON_Parse(puf);
    if (root == nullptr) return false;

    bool ok = false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *t   = cur ? cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m") : nullptr;
    const cJSON *rh  = cur ? cJSON_GetObjectItemCaseSensitive(cur, "relative_humidity_2m") : nullptr;
    const cJSON *wc  = cur ? cJSON_GetObjectItemCaseSensitive(cur, "weather_code") : nullptr;
    const cJSON *tag = cur ? cJSON_GetObjectItemCaseSensitive(cur, "is_day") : nullptr;
    const cJSON *cc  = cur ? cJSON_GetObjectItemCaseSensitive(cur, "cloud_cover") : nullptr;
    const cJSON *pr  = cur ? cJSON_GetObjectItemCaseSensitive(cur, "precipitation") : nullptr;
    const cJSON *ws  = cur ? cJSON_GetObjectItemCaseSensitive(cur, "wind_speed_10m") : nullptr;
    if (cJSON_IsNumber(t) && cJSON_IsNumber(rh) && cJSON_IsNumber(wc)) {
        const Art a = art_aus_code(wc->valueint);
        v_temp    = (int32_t)lround(t->valuedouble * 100.0);
        v_feuchte = (int32_t)lround(rh->valuedouble * 100.0);
        v_art     = (int32_t)a;
        v_nacht   = (cJSON_IsNumber(tag) && tag->valueint == 0) ? 1 : 0;
        v_wolken  = cJSON_IsNumber(cc) ? cc->valueint : 0;
        v_regen   = cJSON_IsNumber(pr) ? (int32_t)lround(pr->valuedouble * 10.0) : 0;
        v_wind    = cJSON_IsNumber(ws) ? (int32_t)lround(ws->valuedouble * 10.0) : 0;
        v_nebel   = (wc->valueint == 45 || wc->valueint == 48) ? 1 : 0;
        v_gueltig = 1;
        ok        = true;
        ESP_LOGI(TAG, "%s%s, %.1f Grad, %d %% Feuchte, %d %% Wolken, %.1f mm, Wind %.0f km/h (Code %d).", art_text(a),
                 v_nacht ? " (Nacht)" : "", t->valuedouble, (int)rh->valuedouble, (int)v_wolken,
                 v_regen / 10.0, v_wind / 10.0, wc->valueint);
    }
    cJSON_Delete(root);
    return ok;
}

// Hoehen am Ort und auf zwei Ringen (4 und 12 km, je 8 Richtungen). Wie
// bergig es ist, sagt der Unterschied zwischen hoechstem und tiefstem Punkt;
// logarithmisch, weil 50 m Huegel schon deutlich mehr sind als 0, 1250 m
// gegenueber 1000 aber kaum. Unter 25 m ist es flach.
bool gelaende_holen(char *puf, double lat, double lon)
{
    const int kPunkte = 17;
    char      url[640];
    char      las[260], los[260];
    int       nla = 0, nlo = 0;
    const double km_lat = 1.0 / 111.0, km_lon = 1.0 / (111.0 * cos(lat * M_PI / 180.0));
    for (int i = 0; i < kPunkte; i++) {
        const double r = i == 0 ? 0.0 : (i <= 8 ? 4.0 : 12.0);
        const double w = (i - 1) * M_PI / 4.0 + (i > 8 ? M_PI / 8.0 : 0.0);
        nla += snprintf(las + nla, sizeof(las) - nla, "%s%.3f", i ? "," : "", lat + r * sin(w) * km_lat);
        nlo += snprintf(los + nlo, sizeof(los) - nlo, "%s%.3f", i ? "," : "", lon + r * cos(w) * km_lon);
    }
    snprintf(url, sizeof(url), "http://api.open-meteo.com/v1/elevation?latitude=%s&longitude=%s", las, los);
    if (holen(url, puf, kAntwortMax) < 0) return false;

    cJSON *root = cJSON_Parse(puf);
    if (root == nullptr) return false;
    const cJSON *el = cJSON_GetObjectItemCaseSensitive(root, "elevation");
    bool ok = cJSON_IsArray(el) && cJSON_GetArraySize(el) == kPunkte;
    if (ok) {
        double mitte = 0, lo = 1e9, hi = -1e9;
        for (int i = 0; i < kPunkte; i++) {
            const cJSON *e = cJSON_GetArrayItem(el, i);
            if (!cJSON_IsNumber(e)) { ok = false; break; }
            const double h = e->valuedouble;
            if (i == 0) mitte = h;
            lo = h < lo ? h : lo;
            hi = h > hi ? h : hi;
        }
        if (ok) {
            const double relief = hi - lo;
            double       b      = log(1.0 + (relief > 25 ? relief - 25 : 0) / 30.0) / log(1.0 + 1200.0 / 30.0);
            b = b < 0 ? 0 : (b > 1 ? 1 : b);
            v_g_hoehe      = (int32_t)lround(mitte);
            v_g_relief     = (int32_t)lround(relief);
            v_g_bergigkeit = (int32_t)lround(b * 1000.0);
            v_g_keim       = (int32_t)((uint32_t)lround(lat * 100.0) * 73856093u ^ (uint32_t)lround(lon * 100.0) * 19349663u);
            v_g_gueltig    = 1;
            ESP_LOGI(TAG, "Gelaende: %d m hoch, %d m Relief, Bergigkeit %d.", (int)v_g_hoehe, (int)v_g_relief,
                     (int)v_g_bergigkeit);
        }
    }
    cJSON_Delete(root);
    return ok;
}

void task(void *)
{
    static char puf[kAntwortMax];
    bool    ort_da    = false;
    bool    gelaende_da = false;
    double  lat = 0, lon = 0;
    int64_t naechster = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        const int64_t jetzt = esp_timer_get_time();
        if (jetzt < naechster || !net::connected()) continue;
        if (darf_ != nullptr && !darf_()) continue;
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kMinIntern) {
            naechster = jetzt + kNochmalUs;
            continue;
        }

        if (!ort_da) ort_da = ort_ermitteln(puf, &lat, &lon);
        if (ort_da && !gelaende_da) gelaende_da = gelaende_holen(puf, lat, lon);
        const bool ok = ort_da && wetter_holen(puf, lat, lon);
        naechster = esp_timer_get_time() + (ok ? kAbstandUs : kNochmalUs);
    }
}

}  // namespace

void begin(bool (*darf)())
{
    darf_ = darf;
    // Stack im PSRAM wie bei Stt und Chat; der Task wartet fast nur auf das Netz.
    xTaskCreatePinnedToCoreWithCaps(task, "wetter", 6144, nullptr, 2, nullptr, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

Stand stand()
{
    Stand s;
    s.gueltig     = v_gueltig != 0;
    s.art         = (Art)v_art;
    s.nacht       = v_nacht != 0;
    s.temp_c100   = v_temp;
    s.feuchte_100 = v_feuchte;
    s.wolken_pct  = v_wolken;
    s.regen_mm10  = v_regen;
    s.wind_kmh10  = v_wind;
    s.nebel       = v_nebel != 0;
    s.ort         = v_ort != 0;
    s.breite_100  = v_breite;
    s.laenge_100  = v_laenge;
    return s;
}

Gelaende gelaende()
{
    Gelaende g;
    g.gueltig    = v_g_gueltig != 0;
    g.hoehe_m    = v_g_hoehe;
    g.relief_m   = v_g_relief;
    g.bergigkeit = v_g_bergigkeit;
    g.keim       = (uint32_t)v_g_keim;
    return g;
}

}  // namespace wetter
