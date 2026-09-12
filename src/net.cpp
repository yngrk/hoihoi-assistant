#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "cfg.h"
#include "prov.h"

static const char *TAG = "net";

namespace {

// So viele vergebliche Durchgaenge, bevor die Bereitstellung aufgeht. Einer
// waere zu wenig — ein Netz kann beim Hochfahren des Geraets kurz fehlen,
// etwa wenn beide nach einem Stromausfall gleichzeitig starten.
const int kMaxRunden = 3;

// Obergrenze fuer den Suchlauf. Mehr Netze als das sieht ein Wohnzimmer
// selten. Die Ergebnisse gehoeren auf den Heap und nicht auf den Stapel:
// wifi_ap_record_t ist gut 80 Byte gross, zwanzig davon sind 1,6 KB, und der
// Event-Task, in dem dieser Code laeuft, hat insgesamt nur 2304.
const int kMaxAps = 20;

volatile bool s_connected = false;
volatile bool s_prov      = false;
bool          s_started   = false;
int           s_runden    = 0;
const char   *s_status    = "WLAN aus";
char          s_ip[16]    = "-";

esp_timer_handle_t s_retry = nullptr;

void scan_start()
{
    if (prov::running()) return;

    s_status = "WLAN sucht";
    wifi_scan_config_t sc = {};
    sc.show_hidden        = false;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        // Ein Suchlauf laeuft bereits oder das Funkmodul ist beschaeftigt —
        // in beiden Faellen kommt das Ergebnis ohnehin gleich.
        ESP_LOGD(TAG, "Suchlauf nicht gestartet, laeuft vermutlich schon.");
    }
}

// Der neue Versuch laeuft ueber einen Timer, nicht ueber vTaskDelay: der
// Handler haengt im Event-Task, und ein Warten darin hielte auch alle
// anderen Ereignisse auf — einschliesslich der IP-Zuteilung, auf die wir
// gerade warten.
void retry(void *)
{
    scan_start();
}

void plan_retry(int ms)
{
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, (uint64_t)ms * 1000);
}

// Aus dem Suchergebnis das staerkste Netz heraussuchen, das wir kennen.
// Nicht das erste, das passt: stehen Wohnung und Hotspot beide bereit, ist
// das staerkere fast immer das gemeinte, und es traegt besser.
bool pick_and_connect()
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    if (n > kMaxAps) n = kMaxAps;

    auto *aps = (wifi_ap_record_t *)malloc(n * sizeof(wifi_ap_record_t));
    if (aps == nullptr) return false;

    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        free(aps);
        return false;
    }

    const cfg::Netz *treffer = nullptr;
    int8_t           beste   = -128;

    for (uint16_t i = 0; i < n; i++) {
        for (int k = 0; k < cfg::net_count(); k++) {
            const cfg::Netz *netz = cfg::net_at(k);
            if (strcmp((const char *)aps[i].ssid, netz->ssid) != 0) continue;
            if (aps[i].rssi > beste) {
                beste   = aps[i].rssi;
                treffer = netz;
            }
        }
    }
    free(aps);

    if (treffer == nullptr) return false;

    // strncpy und nicht snprintf: die Felder im Treiber sind 32 und 64 Byte
    // und brauchen kein Nullbyte, wenn sie ganz gefuellt sind. snprintf wuerde
    // eines erzwingen und damit das letzte Zeichen eines maximal langen
    // Passworts verschlucken.
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, treffer->ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, treffer->pass, sizeof(wc.sta.password));

    ESP_LOGI(TAG, "Verbinde mit %s (%d dBm).", treffer->ssid, (int)beste);
    s_status = "WLAN verbindet";

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    return true;
}

// Kein bekanntes Netz in Reichweite. Nach ein paar Runden geht die
// Bereitstellung auf — das ist der einzige Weg zurueck, wenn sich das
// Passwort geaendert hat oder das Geraet umgezogen ist.
void keins_gefunden()
{
    s_runden++;
    if (s_runden < kMaxRunden) {
        ESP_LOGI(TAG, "Kein bekanntes Netz (Runde %d von %d).", s_runden,
                 kMaxRunden);
        s_status = "WLAN sucht";
        plan_retry(5000);
        return;
    }

    if (!prov::running()) {
        ESP_LOGW(TAG, "Kein bekanntes Netz erreichbar, Bereitstellung geht auf.");
        s_prov   = true;
        s_status = "Bereitstellung";
        prov::begin();
    }
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        scan_start();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (prov::running()) return;
        if (!pick_and_connect()) keins_gefunden();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        snprintf(s_ip, sizeof(s_ip), "-");

        // Waehrend der Bereitstellung probiert die App die Zugangsdaten aus;
        // ein Fehlschlag gehoert dort dazu und ist nicht unsere Sache.
        if (prov::running()) return;

        const auto *d = (const wifi_event_sta_disconnected_t *)data;
        s_status      = "WLAN getrennt";
        ESP_LOGW(TAG, "Verbindung verloren (Grund %d), neuer Suchlauf.",
                 d ? d->reason : -1);

        // Mit wachsendem Abstand neu suchen, hoechstens alle zehn Sekunden.
        // Sekuendliche Versuche blockieren das Geraet und stoeren das Netz.
        plan_retry((s_runden < 3) ? 2000 : 10000);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *ip = (const ip_event_got_ip_t *)data;
        s_runden       = 0;
        s_connected    = true;
        s_prov         = false;
        s_status       = "WLAN verbunden";
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip->ip_info.ip));
        ESP_LOGI(TAG, "Verbunden, IP %s.", s_ip);
    }
}

}  // namespace

esp_err_t net::begin()
{
    if (s_started) return ESP_OK;

    esp_timer_create_args_t targs = {};
    targs.callback = &retry;
    targs.name     = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfgw = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfgw));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Kein Stromsparen: der Sendebetrieb waehrend einer Aufnahme soll nicht
    // auf den naechsten Beacon warten muessen.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;

    // Ohne ein einziges bekanntes Netz gibt es nichts zu suchen. Der
    // Suchlauf laeuft trotzdem an (WIFI_EVENT_STA_START), findet nichts
    // Bekanntes und landet nach kMaxRunden in der Bereitstellung — das ist
    // derselbe Weg, nur ohne Sonderfall im Code.
    if (cfg::net_count() == 0) {
        ESP_LOGI(TAG, "Kein Netz gespeichert, Bereitstellung wird gebraucht.");
        s_runden = kMaxRunden - 1;
    }
    return ESP_OK;
}

bool net::connected()
{
    return s_connected;
}

bool net::provisioning()
{
    return s_prov;
}

const char *net::ip()
{
    return s_ip;
}

const char *net::status()
{
    return s_status;
}
