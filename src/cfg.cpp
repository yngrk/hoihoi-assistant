#include "cfg.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>

#include "secrets.h"

static const char *TAG = "cfg";

namespace {

// Eigener Namensraum im NVS, damit die Werte nicht zwischen denen des
// WLAN-Stacks liegen — der legt dort seine eigenen ab.
const char *kNamespace = "hoihoi";
const char *kKeySeeded = "seeded";

cfg::Netz s_nets[cfg::kMaxNets];
int       s_count = 0;

esp_err_t open_rw(nvs_handle_t *h)
{
    return nvs_open(kNamespace, NVS_READWRITE, h);
}

// Netze liegen als ssid0/pass0 bis ssid3/pass3 im NVS. Ein Feld waere ein
// Blob und muesste bei jeder Aenderung als Ganzes geschrieben werden; so
// bleibt jede Angabe fuer sich lesbar, auch mit nvs_get_str von aussen.
void key_name(char *out, size_t n, const char *praefix, int i)
{
    snprintf(out, n, "%s%d", praefix, i);
}

esp_err_t save_nets(nvs_handle_t h)
{
    char name[16];
    for (int i = 0; i < cfg::kMaxNets; i++) {
        key_name(name, sizeof(name), "ssid", i);
        if (i < s_count) nvs_set_str(h, name, s_nets[i].ssid);
        else             nvs_erase_key(h, name);

        key_name(name, sizeof(name), "pass", i);
        if (i < s_count) nvs_set_str(h, name, s_nets[i].pass);
        else             nvs_erase_key(h, name);
    }
    return nvs_commit(h);
}

void load_nets(nvs_handle_t h)
{
    char name[16];
    s_count = 0;
    for (int i = 0; i < cfg::kMaxNets; i++) {
        key_name(name, sizeof(name), "ssid", i);
        size_t n = sizeof(s_nets[s_count].ssid);
        if (nvs_get_str(h, name, s_nets[s_count].ssid, &n) != ESP_OK) continue;
        if (s_nets[s_count].ssid[0] == '\0') continue;

        key_name(name, sizeof(name), "pass", i);
        n = sizeof(s_nets[s_count].pass);
        if (nvs_get_str(h, name, s_nets[s_count].pass, &n) != ESP_OK) {
            s_nets[s_count].pass[0] = '\0';
        }
        s_count++;
    }
}

// Einmalig beim allerersten Start: ein in secrets.h eingetragenes Netz ins
// NVS heben. Danach nie wieder, sonst kaeme ein geloeschtes Netz zurueck.
void seed(nvs_handle_t h)
{
    uint8_t schon = 0;
    if (nvs_get_u8(h, kKeySeeded, &schon) == ESP_OK && schon != 0) return;

    if (WIFI_SSID[0] != '\0') {
        snprintf(s_nets[0].ssid, sizeof(s_nets[0].ssid), "%s", WIFI_SSID);
        snprintf(s_nets[0].pass, sizeof(s_nets[0].pass), "%s", WIFI_PASSWORD);
        s_count = 1;
        save_nets(h);
        ESP_LOGI(TAG, "Netz aus secrets.h uebernommen: %s", s_nets[0].ssid);
    }

    nvs_set_u8(h, kKeySeeded, 1);
    nvs_commit(h);
}

}  // namespace

esp_err_t cfg::begin()
{
    nvs_handle_t h;
    esp_err_t    err = open_rw(&h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS nicht zu oeffnen: %s", esp_err_to_name(err));
        return err;
    }

    load_nets(h);
    seed(h);
    nvs_close(h);

    ESP_LOGI(TAG, "%d bekannte(s) Netz(e).", s_count);
    return ESP_OK;
}

int cfg::net_count()
{
    return s_count;
}

const cfg::Netz *cfg::net_at(int i)
{
    if (i < 0 || i >= s_count) return nullptr;
    return &s_nets[i];
}

esp_err_t cfg::net_add(const char *ssid, const char *pass)
{
    if (ssid == nullptr || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    int platz = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { platz = i; break; }
    }
    if (platz < 0) {
        if (s_count < kMaxNets) {
            platz = s_count++;
        } else {
            // Voll: das aelteste faellt heraus, der Rest rutscht auf.
            memmove(&s_nets[0], &s_nets[1], sizeof(Netz) * (kMaxNets - 1));
            platz = kMaxNets - 1;
        }
    }

    snprintf(s_nets[platz].ssid, sizeof(s_nets[platz].ssid), "%s", ssid);
    snprintf(s_nets[platz].pass, sizeof(s_nets[platz].pass), "%s",
             pass ? pass : "");

    nvs_handle_t h;
    esp_err_t    err = open_rw(&h);
    if (err != ESP_OK) return err;
    err = save_nets(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Netz gespeichert: %s (%d insgesamt)", ssid, s_count);
    return err;
}

esp_err_t cfg::net_remove(const char *ssid)
{
    if (ssid == nullptr) return ESP_ERR_INVALID_ARG;

    int platz = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { platz = i; break; }
    }
    if (platz < 0) return ESP_ERR_NOT_FOUND;

    for (int i = platz; i + 1 < s_count; i++) s_nets[i] = s_nets[i + 1];
    s_count--;

    nvs_handle_t h;
    esp_err_t    err = open_rw(&h);
    if (err != ESP_OK) return err;
    err = save_nets(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Netz entfernt: %s (%d verbleiben)", ssid, s_count);
    return err;
}
