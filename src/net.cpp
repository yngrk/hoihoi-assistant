#include "net.h"

#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>

static const char *TAG = "net";

namespace {

volatile bool      s_connected = false;
volatile bool      s_started   = false;
int                s_retries   = 0;
const char        *s_status    = "WLAN aus";
esp_timer_handle_t s_retry     = nullptr;

// Der neue Versuch laeuft ueber einen Timer, nicht ueber vTaskDelay: der
// Handler haengt im Event-Task, und ein Warten darin hielte auch alle
// anderen Ereignisse auf — einschliesslich der IP-Zuteilung, auf die wir
// gerade warten.
void retry_connect(void *)
{
    esp_wifi_connect();
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_status = "WLAN sucht";
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;

        const auto *d = (const wifi_event_sta_disconnected_t *)data;
        // Mit wachsendem Abstand neu versuchen, hoechstens alle zehn Sekunden.
        // Sekuendliche Versuche blockieren das Geraet und stoeren das Netz.
        const int delay_ms = (s_retries < 5) ? 1000 : 10000;
        if (s_retries < 5) s_retries++;

        s_status = "WLAN getrennt";
        ESP_LOGW(TAG, "Verbindung verloren (Grund %d), neuer Versuch in %d ms.",
                 d ? d->reason : -1, delay_ms);

        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, (uint64_t)delay_ms * 1000);
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *ip = (const ip_event_got_ip_t *)data;
        s_retries   = 0;
        s_connected = true;
        s_status    = "WLAN verbunden";
        ESP_LOGI(TAG, "Verbunden, IP " IPSTR ".", IP2STR(&ip->ip_info.ip));
    }
}

}  // namespace

esp_err_t net::begin(const char *ssid, const char *password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        s_status = "WLAN nicht gesetzt";
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) return ESP_OK;

    esp_timer_create_args_t targs = {};
    targs.callback = &retry_connect;
    targs.name     = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, password ? password : "",
            sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // Kein Stromsparen: der Sendebetrieb waehrend einer Aufnahme soll nicht
    // auf den naechsten Beacon warten muessen.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    return ESP_OK;
}

bool net::connected()
{
    return s_connected;
}

const char *net::status()
{
    return s_status;
}
