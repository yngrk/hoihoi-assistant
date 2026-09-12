#include "prov.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "cfg.h"

static const char *TAG = "prov";

namespace {

bool s_running  = false;
bool s_starting = false;
char s_name[16] = "";
char s_pop[9]   = "";

// Name und Kennwortnachweis aus der MAC. Feste Werte waeren bequemer, aber
// zwei Geraete im selben Raum haetten denselben Namen — und ein fest
// einkompilierter Nachweis ist keiner.
void identity_from_mac()
{
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    snprintf(s_name, sizeof(s_name), "HOIHOI-%02X%02X", mac[4], mac[5]);
    snprintf(s_pop, sizeof(s_pop), "%02x%02x%02x%02x", mac[2], mac[3], mac[4],
             mac[5]);
}

void on_prov_event(void *, esp_event_base_t, int32_t id, void *data)
{
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "Bereitstellung laeuft: %s, Nachweis %s", s_name, s_pop);
        break;

    case WIFI_PROV_CRED_RECV: {
        const auto *c = (const wifi_sta_config_t *)data;

        // ssid und password sind Felder fester Groesse OHNE garantiertes
        // Nullbyte: eine genau 32 Zeichen lange SSID fuellt ihres restlos
        // aus. Wer sie dann als Zeichenkette behandelt, liest weiter — und
        // direkt dahinter liegt im selben Struct das Passwort. Genau so ist
        // es hier passiert, mitsamt Ausgabe ins Log. Deshalb erst umkopieren
        // und terminieren, dann anfassen.
        char ssid[sizeof(c->ssid) + 1] = {};
        char pass[sizeof(c->password) + 1] = {};
        memcpy(ssid, c->ssid, sizeof(c->ssid));
        memcpy(pass, c->password, sizeof(c->password));

        // Nur die SSID ins Log, nie das Passwort. Ein Mitschnitt der
        // seriellen Schnittstelle ist eine Datei wie jede andere.
        ESP_LOGI(TAG, "Zugangsdaten empfangen: %s", ssid);

        // Zusaetzlich in cfg ablegen. Der Bereitstellungsmanager kennt nur
        // ein Netz und ueberschreibt es beim naechsten Mal; die eigene Liste
        // behaelt Wohnung und Hotspot nebeneinander.
        cfg::net_add(ssid, pass);
        break;
    }

    case WIFI_PROV_CRED_FAIL: {
        const auto *r = (const wifi_prov_sta_fail_reason_t *)data;
        ESP_LOGW(TAG, "Zugangsdaten abgelehnt (%s).",
                 (*r == WIFI_PROV_STA_AUTH_ERROR) ? "Passwort falsch"
                                                  : "Netz nicht gefunden");
        // Nicht selbst aufraeumen: die App darf es gleich noch einmal
        // versuchen, ohne dass das Geraet dafuer neu starten muss.
        break;
    }

    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Verbindung bestaetigt.");
        break;

    case WIFI_PROV_END:
        ESP_LOGI(TAG, "Bereitstellung beendet, Bluetooth wird freigegeben.");
        wifi_prov_mgr_deinit();
        s_running  = false;
        s_starting = false;
        break;

    default:
        break;
    }
}

esp_err_t do_begin()
{
    identity_from_mac();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               &on_prov_event, nullptr));

    // free_ble und nicht free_btdm: der S3 kann nur BLE, klassisches
    // Bluetooth gibt es auf diesem Chip nicht — es waere nichts freizugeben.
    wifi_prov_mgr_config_t cfgp = {};
    cfgp.scheme               = wifi_prov_scheme_ble;
    cfgp.scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE;

    esp_err_t err = wifi_prov_mgr_init(cfgp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init: %s", esp_err_to_name(err));
        return err;
    }

    // Eigene Dienst-UUID, damit die App das Geraet zwischen fremden
    // BLE-Geraeten sicher erkennt. Little Endian, wie von der API erwartet.
    uint8_t uuid[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
    wifi_prov_scheme_ble_set_service_uuid(uuid);

    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, s_pop, s_name,
                                           nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_provisioning: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return err;
    }

    s_running = true;
    return ESP_OK;
}

// Der Aufruf kommt aus dem WLAN-Event-Task, und der hat 2304 Byte Stapel.
// Von dort aus den BLE-Stack hochzufahren geht schief — dieselbe Falle, in
// die schon der Suchlauf mit seinen Ergebnissen auf dem Stapel gelaufen ist.
// Also ein eigener, kurzlebiger Task mit eigenem Stapel; er raeumt sich
// selbst weg, sobald der Dienst steht.
void start_task(void *)
{
    if (do_begin() != ESP_OK) s_starting = false;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t prov::begin()
{
    if (s_running || s_starting) return ESP_OK;
    s_starting = true;

    if (xTaskCreate(&start_task, "prov", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_starting = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool prov::running()
{
    return s_running;
}

const char *prov::device_name()
{
    return s_name;
}

const char *prov::pop()
{
    return s_pop;
}
