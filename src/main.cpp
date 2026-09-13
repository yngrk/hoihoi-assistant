// ---------------------------------------------------------------------------
// Hardware-Bring-up fuer das Waveshare ESP32-S3-RLCD-4.2
//
// Prueft der Reihe nach: Chip und PSRAM, I2C-Bus mit allen vier Bausteinen,
// eine echte SHTC3-Messung, das ST7305-Display und zuletzt Tasten und
// Batteriespannung. Jeder Abschnitt meldet sich einzeln im Log, damit ein
// Fehlschlag sofort einem Teilsystem zuzuordnen ist.
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_private/esp_clk.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/temperature_sensor.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <esp_netif_sntp.h>

#include "display_bsp.h"
#include "display_sync.h"
#include "gfx.h"
#include "font5x7.h"
#include "audio.h"
#include "echo.h"
#include "film.h"
#include "listen.h"
#include "nachtrag.h"
#include "oberflaeche.h"
#include "vergleich.h"
#include "wachwort.h"
#include "schrift.h"
#include "wetter.h"
#include "cfg.h"
#include "net.h"
#include "prov.h"
#include "secrets.h"
#include "stt.h"
#include "chat.h"
#include "tts.h"
#include "user_config.h"

// Aeltere secrets.h kennen die Zeile noch nicht. Ein fehlendes Modell ist
// kein Grund, den Bau scheitern zu lassen — der Standardwert ist derselbe,
// der in der Vorlage steht.
#ifndef CHAT_MODEL
#define CHAT_MODEL "gpt-5.6-luna"
#endif
#ifndef TTS_MODEL
#define TTS_MODEL "gpt-4o-mini-tts"
#endif
#ifndef TTS_VOICE
#define TTS_VOICE "cedar"
#endif

static const char *TAG = "bringup";

static i2c_master_bus_handle_t i2c_bus = nullptr;

// Zeichenflaeche, angelegt in test_display(), danach fuer alle weiteren
// Ausgaben gueltig.
static Canvas *display = nullptr;

// Zuhoeren auf Tastendruck. Geschrieben wird nur im Aufnahmetask, gelesen
// zusaetzlich im Anzeigetask — siehe listen.h zur Synchronisierung.
static Listener listener;

// Spracherkennung. Haengt am Listener und arbeitet in einem eigenen Task,
// der Anzeigetask liest nur Phase und Text.
static Stt  stt;
static Chat chat;
static Tts  tts;

// Erwartete Teilnehmer am I2C-Bus, fuer eine lesbare Scan-Ausgabe.
struct KnownDevice {
    uint8_t     addr;
    const char *name;
};

static const KnownDevice kKnownDevices[] = {
    {0x18, "ES8311 Audio-Codec"},
    {0x40, "ES7210 Mic-ADC"},
    {0x51, "PCF85063 RTC"},
    {0x70, "SHTC3 Temp/Feuchte"},
};

static const char *lookup_device(uint8_t addr)
{
    for (const auto &d : kKnownDevices) {
        if (d.addr == addr) return d.name;
    }
    return "unbekannt";
}

// --- 1. Chip, Flash, PSRAM ------------------------------------------------

static void report_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t  flash_size = 0;
    esp_err_t flash_err  = esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "--- Chip ---");
    ESP_LOGI(TAG, "Kerne      : %d", info.cores);
    ESP_LOGI(TAG, "Revision   : %d", info.revision);

    // Ungeprueft wuerde ein Fehlschlag hier als "0 MB" durchgehen und wie ein
    // Hardwaredefekt aussehen, statt als das was er ist.
    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "Flash      : %" PRIu32 " MB", flash_size / (1024 * 1024));
    } else {
        ESP_LOGE(TAG, "Flash      : nicht lesbar (%s)", esp_err_to_name(flash_err));
    }

    ESP_LOGI(TAG, "CPU-Takt   : %d MHz", (int)(esp_clk_cpu_freq() / 1000000));

    if (esp_psram_is_initialized()) {
        size_t psram = esp_psram_get_size();
        ESP_LOGI(TAG, "PSRAM      : %u MB (%u Bytes frei)",
                 (unsigned)(psram / (1024 * 1024)),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (psram < 8 * 1024 * 1024) {
            ESP_LOGW(TAG, "PSRAM kleiner als die erwarteten 8 MB — Octal-Modus pruefen!");
        }
    } else {
        ESP_LOGE(TAG, "PSRAM NICHT initialisiert — CONFIG_SPIRAM_MODE_OCT gesetzt?");
    }

    ESP_LOGI(TAG, "Heap intern: %u Bytes frei",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// --- 2. I2C-Bus scannen ---------------------------------------------------

static void init_i2c(void)
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port                     = I2C_NUM_0;
    cfg.sda_io_num                   = I2C_SDA_PIN;
    cfg.scl_io_num                   = I2C_SCL_PIN;
    cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt            = 7;
    cfg.flags.enable_internal_pullup = true;

    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));
}

static void scan_i2c(void)
{
    ESP_LOGI(TAG, "--- I2C-Scan (SDA=%d, SCL=%d) ---", I2C_SDA_PIN, I2C_SCL_PIN);

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X  %s", addr, lookup_device(addr));
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "  Kein Geraet gefunden — Verkabelung oder Pins pruefen.");
    } else {
        ESP_LOGI(TAG, "  %d Geraet(e) gefunden.", found);
    }
}

// --- 3. SHTC3 wirklich auslesen -------------------------------------------
// Beweist, dass der Bus nicht nur ACKt, sondern plausible Daten liefert.

// Der Baustein bleibt nach dem Bring-up angemeldet, weil der Stats-Task ihn
// zyklisch weiterliest. Zwischen den Messungen schlaeft er ohnehin.
static i2c_master_dev_handle_t shtc3_dev = nullptr;

static esp_err_t shtc3_open(void)
{
    if (shtc3_dev != nullptr) return ESP_OK;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = 0x70;
    dev_cfg.scl_speed_hz    = 100000;

    return i2c_master_bus_add_device(i2c_bus, &dev_cfg, &shtc3_dev);
}

// Werte in Hundertsteln, damit die Anzeige ohne Gleitkomma formatieren kann.
static esp_err_t shtc3_measure(int32_t *t_c100, int32_t *rh_100)
{
    if (shtc3_dev == nullptr) return ESP_ERR_INVALID_STATE;

    const uint8_t cmd_wakeup[]  = {0x35, 0x17};
    const uint8_t cmd_measure[] = {0x78, 0x66};  // Normalmodus, Temperatur zuerst
    const uint8_t cmd_sleep[]   = {0xB0, 0x98};

    esp_err_t err = i2c_master_transmit(shtc3_dev, cmd_wakeup, sizeof(cmd_wakeup), 100);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(1));

    err = i2c_master_transmit(shtc3_dev, cmd_measure, sizeof(cmd_measure), 100);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t raw[6] = {0};
    err = i2c_master_receive(shtc3_dev, raw, sizeof(raw), 100);

    // Schlafbefehl in jedem Fall, auch nach einem Lesefehler — sonst bliebe
    // der Sensor wach und zieht dauerhaft Strom.
    i2c_master_transmit(shtc3_dev, cmd_sleep, sizeof(cmd_sleep), 100);
    if (err != ESP_OK) return err;

    const uint16_t t_raw = (raw[0] << 8) | raw[1];
    const uint16_t h_raw = (raw[3] << 8) | raw[4];
    *t_c100 = (int32_t)(-4500 + (17500 * (int32_t)t_raw) / 65535);
    *rh_100 = (int32_t)((10000 * (int32_t)h_raw) / 65535);
    return ESP_OK;
}

static void read_shtc3(void)
{
    ESP_LOGI(TAG, "--- SHTC3 ---");

    if (shtc3_open() != ESP_OK) {
        ESP_LOGE(TAG, "  Geraet konnte nicht angelegt werden.");
        return;
    }

    int32_t t100 = 0, h100 = 0;
    const esp_err_t err = shtc3_measure(&t100, &h100);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "  Temperatur : %.2f C", t100 / 100.0f);
        ESP_LOGI(TAG, "  Feuchte    : %.2f %%rH", h100 / 100.0f);
    } else {
        ESP_LOGE(TAG, "  Messung fehlgeschlagen (%s)", esp_err_to_name(err));
    }
}

// --- 4. Display -----------------------------------------------------------

static void test_display(void)
{
    ESP_LOGI(TAG, "--- Display (ST7305, %dx%d) ---", LCD_WIDTH, LCD_HEIGHT);

    // Bewusst erst hier instanziiert, nicht als globales Objekt: der Treiber
    // allokiert Puffer, und zum Zeitpunkt globaler Konstruktoren laesst sich
    // ueber die Heap-Situation weniger sicher urteilen als hier in app_main.
    static SyncDisplay rlcd(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                            RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

    rlcd.RLCD_Init();

    // Den Bildpuffer aus dem PSRAM holen, bevor das erste Bild laeuft: sonst
    // legt spi_master bei jedem Bild 15 KB internen Zwischenspeicher an und
    // gibt sie wieder her, und der Treiber bricht ab, wenn das einmal nicht
    // klappt. Siehe display_sync.h.
    const esp_err_t dmabuf = rlcd.pin_buffer_to_dma();
    if (dmabuf != ESP_OK) {
        ESP_LOGW(TAG, "  Bildpuffer bleibt im PSRAM (%s).",
                 esp_err_to_name(dmabuf));
    }

    // Vor dem ersten Bild: ohne diese Rueckmeldung wuerde der Zeichencode in
    // den noch laufenden DMA-Transfer hineinschreiben.
    const esp_err_t dma = rlcd.enable_transfer_wait();
    if (dma != ESP_OK) {
        ESP_LOGW(TAG, "  Kein DMA-Abschlusssignal (%s), Bild kann mischen.",
                 esp_err_to_name(dma));
    }

    // Ab hier laeuft jedes Zeichnen ueber Canvas, nie direkt ueber
    // RLCD_SetPixel() — siehe gfx.h zur Begruendung.
    display = new Canvas(rlcd, LCD_WIDTH, LCD_HEIGHT);

    // RLCD_Init() hat die TE-Leitung des Controllers bereits eingeschaltet
    // (Befehl 0x35 mit Parameter 0x00), ausgewertet hat sie bisher niemand.
    const esp_err_t te = display->enable_tearing_sync(RLCD_TE_PIN);
    if (te == ESP_OK) {
        ESP_LOGI(TAG, "  TE-Synchronisation aktiv (GPIO%d).", RLCD_TE_PIN);
    } else {
        ESP_LOGW(TAG, "  TE-Synchronisation nicht moeglich (%s), Bild kann reissen.",
                 esp_err_to_name(te));
    }

    // Beim Start bleibt das Display weiss, bis die Oberflaeche uebernimmt.
    // Frueher stand hier das Avatarbild; die Oberflaeche liess davon am Rand
    // und in den Ecken ihres Fensters Reste stehen.
    display->clear(ColorWhite);
    display->flush();

    // Selbsttest der Bereichspruefung. Ohne die Huelle wuerde jeder dieser
    // Aufrufe hinter die LUT greifen; dass der Bring-up hier nicht abstuerzt
    // und das Bild unveraendert bleibt, ist der eigentliche Nachweis.
    // Saemtliche Koordinaten liegen vollstaendig ausserhalb, keine ragt in die
    // Flaeche hinein. Geclippt wuerde sonst der sichtbare Teil tatsaechlich
    // gezeichnet und das weisse Bild ueberschrieben — hier darf sich am Puffer
    // nachweislich nichts aendern.
    display->pixel(-1, -1, ColorBlack);
    display->pixel(LCD_WIDTH, LCD_HEIGHT, ColorBlack);
    display->pixel(LCD_WIDTH - 1, LCD_HEIGHT, ColorBlack);   // y genau eins zu weit
    display->pixel(LCD_WIDTH, LCD_HEIGHT - 1, ColorBlack);   // x genau eins zu weit
    display->pixel(30000, 30000, ColorBlack);
    display->hline(-500, -100, 150, ColorWhite);
    display->hline(LCD_WIDTH + 10, LCD_WIDTH + 99, 150, ColorWhite);
    display->vline(200, -500, -100, ColorWhite);
    display->vline(-7, 0, LCD_HEIGHT - 1, ColorWhite);
    display->line(-200, -200, -10, -10, ColorWhite);
    display->fill_rect(-50, -50, -10, -10, ColorWhite);
    ESP_LOGI(TAG, "  Bereichspruefung: 11 Aufrufe ausserhalb ueberstanden, "
                  "Bild unveraendert.");
}

// --- 5. Tasten und Batterie ----------------------------------------------

static adc_oneshot_unit_handle_t adc_handle = nullptr;
static adc_cali_handle_t         adc_cali   = nullptr;

static void init_inputs(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BOOT_BUTTON_PIN) | (1ULL << KEY_BUTTON_PIN);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten    = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &chan_cfg));

    // Ohne Kalibrierung weicht der Rohwert je Chip um einige Prozent ab — beim
    // Akku ist das der Unterschied zwischen halb und fast leer.
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id  = ADC_UNIT_1;
    cali_cfg.chan     = ADC_CHANNEL_3;
    cali_cfg.atten    = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali) != ESP_OK) {
        adc_cali = nullptr;
        ESP_LOGW(TAG, "  ADC ohne Kalibrierung, Akkuspannung nur geschaetzt.");
    }
}

// --- 6. Mikrofon und Lautsprecher -----------------------------------------
//
// Beim Start ist das Display weiss, siehe test_display(). Danach
// uebernimmt die Oberflaeche, siehe ui_task() — oder mit Animationen die
// Szene mit Kacheln und HoiHoi, siehe display_task().

static MicInput      mic;
static SpeakerOutput speaker;

// 24 kHz, nicht 16: die Realtime-API bekommt den Ton so, wie er aufgenommen
// wurde, und 24 kHz ist dort die Rate, auf die alles ausgelegt ist.
// Umrechnen auf dem Geraet waere zusaetzlicher Code an einer Stelle, an der
// ein Fehler nur als schlechtere Erkennung auffiele.
static const uint32_t kSampleRate = 24000;

// true: keine Anfragen an OpenAI, siehe visualize_mic(). Nur fuer Tests des
// Weckworts — im Betrieb false.
static const bool kOhneKi = false;

static const int kReadFrames = 480;   // 20 ms

// Verstaerkung der Mikrofone sonst und waehrend einer Antwort. Der
// Lautsprecher sitzt im selben Gehaeuse; mit 37,5 dB laeuft sein Echo an den
// Anschlag, und ein abgeschnittenes Echo ist nicht mehr das, was die
// Referenz zeigt — es laesst sich nicht abziehen. Ob 7,5 dB weniger reichen,
// zeigen die Anschlagzaehler am Ende jeder Antwort.
static const float kMicDb        = 37.5f;
static const float kMicDbAntwort = 30.0f;

// Messwerte aus dem stats_task fuer Logzeile und Kacheln. Ohne Mutex:
// ausgerichtete 32-Bit-Worte, unteilbar gesetzt und gelesen.
static volatile int32_t battery_raw   = 0;
static volatile int32_t battery_mv    = 0;   // am Akku, also hinter dem Teiler
static volatile int32_t env_temp_c100 = 0;
static volatile int32_t env_hum_100   = 0;
static volatile int32_t env_valid     = 0;

// Der SHTC3 sitzt auf der Platine neben Prozessor, WLAN und Laderegler und
// misst deren Abwaerme mit. Verglichen mit einem Thermometer im selben Raum:
// 32,6 statt 24,4 Grad. Die Feuchte ist relativ zur Temperatur am Sensor und
// deshalb ebenso falsch — zu niedrig, weil warme Luft mehr Wasser fasst. Sie
// wird auf die Raumtemperatur umgerechnet: gleicher Wassergehalt, geteilt
// durch den Saettigungsdruck bei Raumtemperatur (Magnus-Formel).
//
// Die Abwaerme haengt davon ab, was das Geraet gerade tut und ob der Akku
// laedt; ein fester Wert ist eine Naeherung, gemessen im Normalbetrieb.
static const int32_t kEigenwaerme_c100 = 820;

static float saettigung(float t_c)
{
    return 6.112f * exp(17.62f * t_c / (243.12f + t_c));
}

static void klima_korrigieren(int32_t *t_c100, int32_t *rh_100)
{
    const float t_sensor = *t_c100 / 100.0f;
    const float t_raum   = t_sensor - kEigenwaerme_c100 / 100.0f;
    float rh = (*rh_100 / 100.0f) * saettigung(t_sensor) / saettigung(t_raum);
    if (rh > 100.0f) rh = 100.0f;

    *t_c100 -= kEigenwaerme_c100;
    *rh_100  = (int32_t)(rh * 100.0f + 0.5f);
}

// Am ADC liegt ein Drittel der Akkuspannung (Teiler laut Waveshare-Doku).
static const int32_t kAkkuTeiler = 3;

// Kurze Druecke auf BOOT, gezaehlt; die Kartenprobe blaettert damit.
static volatile int32_t boot_kurz = 0;

// BOOT 3 s gehalten schaltet die Landschaft zwischen LIVE und dem Testlauf
// durch alle Wetterlagen um; im Testlauf springt ein kurzer Druck weiter.
// Die Taste haengt an keinem Interrupt, sondern wird alle 100 ms abgefragt —
// der Abstand ist zugleich die Entprellung.
static volatile bool dunst_test = false;

static void poll_boot_button(void)
{
    static int     vorher = 1;
    static int64_t seit   = 0;
    static bool    lang   = false;

    const int jetzt = gpio_get_level(BOOT_BUTTON_PIN);

    if (vorher != 0 && jetzt == 0) {
        seit = esp_timer_get_time();
        lang = false;
    }

    if (jetzt == 0 && !lang && esp_timer_get_time() - seit > 3000000) {
        lang = true;
        dunst_test = !dunst_test;
    }

    if (vorher == 0 && jetzt != 0 && !lang) boot_kurz = boot_kurz + 1;

    vorher = jetzt;
}

// Hintergrundtask auf Kern 0: gibt aus, was der Aufnahmetask ins Log legen
// will, misst Akku, Temperatur und Feuchte und fragt die BOOT-Taste ab.
static void stats_task(void *)
{
    int64_t letzte_messung = 0;

    while (true) {
        // Der Aufnahmetask darf nicht selbst schreiben, siehe nachtrag.h —
        // hier auf Kern 0 kostet das Warten auf die serielle Schnittstelle
        // niemanden Ton.
        nachtrag::ausgeben();
        poll_boot_button();

        const int64_t jetzt = esp_timer_get_time();
        if (jetzt - letzte_messung >= 2000000) {
            letzte_messung = jetzt;
            int raw = 0;
            if (adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw) == ESP_OK) {
                battery_raw = raw;
                int mv = 0;
                if (adc_cali != nullptr && adc_cali_raw_to_voltage(adc_cali, raw, &mv) == ESP_OK) {
                    battery_mv = mv * kAkkuTeiler;
                } else {
                    battery_mv = raw * 3100 / 4095 * kAkkuTeiler;
                }
            }

            // Eine Messung wartet 16 ms; hier auf Kern 0 kostet das niemanden.
            int32_t t100 = 0, h100 = 0;
            if (shtc3_measure(&t100, &h100) == ESP_OK) {
                klima_korrigieren(&t100, &h100);
                env_temp_c100 = t100;
                env_hum_100   = h100;
                env_valid     = 1;
            } else {
                env_valid = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


// --- Szene: Kacheln und HoiHoi -----------------------------------------------
//
// In Ruhe steht HoiHoi klein im Lichtkegel, davor vier Kacheln mit dem Zustand
// des Geraets. Faellt das Weckwort, gleiten die Kacheln seitlich hinaus, und
// HoiHoi kommt nach vorn — das ist die Bildfolge aus assets/wach.film. Vorn
// bleibt er stehen, solange das Gespraech laeuft. Danach zieht er den Vorhang
// zu (assets/zu.film), das Bild blendet auf Schwarz ab, und aus dem Schwarz
// blendet die Ruhestellung wieder auf, waehrend die Kacheln hereingleiten.
//
// Faellt das Weckwort, waehrend der Vorhang zugeht, geht er rueckwaerts wieder
// auf, und HoiHoi steht vorn. Wird HoiHoi zurueckgeschickt, bevor er vorn
// angekommen ist, passt der Vorhang nicht an: dann wird nur abgeblendet.
//
// Alles haengt an drei Groessen: welcher Film, die Position darin und die
// Helligkeit. Die Kacheln leiten ihren Versatz daraus ab, statt eine eigene
// Uhr zu fuehren. So passt alles zusammen, auch wenn ein neues Weckwort einen
// Uebergang mittendrin umkehrt.

extern const uint8_t wach_start[] asm("_binary_wach_film_start");
extern const uint8_t wach_ende[]  asm("_binary_wach_film_end");
extern const uint8_t zu_start[]   asm("_binary_zu_film_start");
extern const uint8_t zu_ende[]    asm("_binary_zu_film_end");

static Film wach;
static Film zu;

// Nach dem letzten Lebenszeichen des Gespraechs bleibt HoiHoi so lange vorn:
// zwischen Frage und Antwort oder vor einer Nachfrage liegen oft Sekunden, in
// denen nichts laeuft, und ein Hin und Her dazwischen waere Unruhe.
static const int64_t kNachlaufUs = 4000000;

// Nach so vielen Bildern sind die Kacheln ganz draussen, eine halbe Sekunde.
static const int kKachelBilder = 12;

static const int kKachelB   = 124;
static const int kKachelH   = 60;
static const int kKachelY1  = 8;
static const int kKachelY2  = kKachelY1 + kKachelH + 8;
static const int kKachelL   = 8;
static const int kKachelR   = LCD_WIDTH - 8 - kKachelB;
static const int kKachelWeg = kKachelB + 16;   // Versatz, bei dem sie ganz draussen sind

// Aus dem Aufnahmetask fuer die Welle, ohne Mutex wie die Messwerte oben:
// ein Pegel, der schnell steigt und langsam faellt.
static volatile int32_t mic_pegel = 0;

// Per KEY im Ruhezustand umgeschaltet. Das Mikrofon laeuft weiter — die
// Echounterdrueckung braucht den Port —, aber das Weckwort bekommt nichts.
static volatile int32_t mikro_stumm = 1;   // beim Start stumm, KEY schaltet
static const bool       kWeckwort   = false;   // false: KEY startet die Frage, HoiHoi weckt nicht

// Eine Antwort wurde per KEY abgebrochen, und gleich wird zugehoert — sobald
// Lautsprecher und Wandler ausgeklungen sind. Die Anzeige springt schon jetzt
// auf "HOERT ZU", sonst stuende dazwischen kurz "SAG HOIHOI".
static volatile int32_t hoeren_gleich = 0;

static bool gespraech_aktiv(void)
{
    const Stt::Phase  sp = stt.phase();
    const Chat::Phase cp = chat.phase();
    const Tts::Phase  tp = tts.phase();
    return listener.listening() || tts.spricht()
           || sp == Stt::Phase::Verbindet || sp == Stt::Phase::Hoert || sp == Stt::Phase::Wartet
           || cp == Chat::Phase::Fragt || cp == Chat::Phase::Antwortet
           || tp == Tts::Phase::Holt || tp == Tts::Phase::Spricht;
}

// Weisse Kachel mit Rand: auf dem fast schwarzen Bild traegt nur eine helle
// Flaeche Text, der vom anderen Ende des Tisches lesbar ist.
static void kachel_rahmen(int x, int y, const char *titel)
{
    display->fill_rect(x, y, x + kKachelB - 1, y + kKachelH - 1, ColorWhite);
    display->rect(x + 2, y + 2, x + kKachelB - 3, y + kKachelH - 3, ColorBlack);
    display->text(x + 8, y + 8, titel, ColorBlack, 1);
}

static void kachel_wlan(int x, int y)
{
    kachel_rahmen(x, y, "WLAN");

    if (net::provisioning()) {
        display->text(x + 8, y + 28, "EINRICHTEN", ColorBlack, 2);
        return;
    }

    wifi_ap_record_t ap = {};
    if (!net::connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        display->text(x + 8, y + 26, "AUS", ColorBlack, 3);
        return;
    }

    // Vier Balken nach Empfangsstaerke, wie man sie vom Telefon kennt.
    const int rssi   = ap.rssi;
    const int balken = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        const int bx = x + 8 + i * 9;
        const int by = y + kKachelH - 10;
        const int bh = 6 + i * 6;
        if (i < balken) {
            display->fill_rect(bx, by - bh, bx + 5, by, ColorBlack);
        } else {
            display->rect(bx, by - bh, bx + 5, by, ColorBlack);
        }
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", rssi);
    display->text(x + 50, y + 26, buf, ColorBlack, 2);
    display->text(x + 50, y + 44, "DBM", ColorBlack, 1);
}

static void kachel_klima(int x, int y)
{
    kachel_rahmen(x, y, "RAUM");
    if (!env_valid) {
        display->text(x + 8, y + 26, "--", ColorBlack, 3);
        return;
    }
    char buf[24];
    const int32_t t = env_temp_c100;
    snprintf(buf, sizeof(buf), "%s%d.%d\x7F", t < 0 ? "-" : "",
             (int)((t < 0 ? -t : t) / 100), (int)((t < 0 ? -t : t) / 10 % 10));
    display->text(x + 8, y + 24, buf, ColorBlack, 3);
    snprintf(buf, sizeof(buf), "%d%% FEUCHTE", (int)(env_hum_100 / 100));
    display->text(x + 8, y + 48, buf, ColorBlack, 1);
}

// Ladestand in Prozent, -1 ohne Akku. Unter 2 V haengt keiner dran — dann ist
// die Zahl Rauschen am Teiler. Sonst linear zwischen 3,0 V (leer) und 4,2 V
// (voll); die Entladekurve einer 18650 ist nicht linear, fuer die Anzeige
// reicht es.
static int akku_prozent(void)
{
    const int32_t mv = battery_mv;
    if (mv < 2000) return -1;
    int prozent = (int)((mv - 3000) * 100 / 1200);
    if (prozent < 0) prozent = 0;
    if (prozent > 100) prozent = 100;
    return prozent;
}

static void kachel_akku(int x, int y)
{
    kachel_rahmen(x, y, "AKKU");

    const int32_t mv      = battery_mv;
    const int     prozent = akku_prozent();
    if (prozent < 0) {
        display->text(x + 8, y + 26, "--", ColorBlack, 3);
        return;
    }

    // Akkusymbol: Koerper, Pol, Fuellung nach Ladestand.
    const int ax = x + 8, ay = y + 26, aw = 30, ah = 16;
    display->rect(ax, ay, ax + aw, ay + ah, ColorBlack);
    display->fill_rect(ax + aw + 1, ay + 5, ax + aw + 3, ay + ah - 5, ColorBlack);
    const int fuell = (aw - 4) * prozent / 100;
    if (fuell > 0) display->fill_rect(ax + 2, ay + 2, ax + 2 + fuell, ay + ah - 2, ColorBlack);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", prozent);
    display->text(x + 46, y + 24, buf, ColorBlack, 2);
    snprintf(buf, sizeof(buf), "%d.%02d V", (int)(mv / 1000), (int)(mv / 10 % 100));
    display->text(x + 46, y + 44, buf, ColorBlack, 1);
}

// Die HoiHoi-Kachel zeigt nur eine Welle: Balken mit runden Enden, die aus
// der Mitte nach aussen laufen. Das Neueste steht in der Mitte, zu den Raendern
// hin wird es aelter und flacher. Auch in Stille bewegt sich ein kleines
// Kraeuseln, damit man sieht, dass zugehoert wird. Der Fortschritt beim
// Einlernen steht im Log.
static const int kWelleBalken = 21;   // ungerade, damit es eine Mitte gibt
static const int kWelleHalb   = kWelleBalken / 2;
static float     welle[kWelleHalb + 1];

// Einmal je gezeichnetem Bild: den Pegel als neuesten Wert einschieben. Er
// zaehlt gegen den Ruhepegel des Raums, nicht gegen Vollausschlag — so schlaegt
// die Welle in einem lauten Raum nicht dauernd aus und in einem stillen nicht nie.
static float mikro_anteil(void)
{
    int32_t ruhe = wachwort::ruhepegel();
    if (ruhe < 30) ruhe = 30;
    float a = log2f((float)mic_pegel / (float)ruhe) / 4.0f;   // 16-fach = voll
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

static void welle_schieben(void)
{
    for (int i = kWelleHalb; i > 0; i--) welle[i] = welle[i - 1];
    welle[0] = mikro_anteil();
}

static void kachel_hoihoi(int x, int y)
{
    display->fill_rect(x, y, x + kKachelB - 1, y + kKachelH - 1, ColorWhite);
    display->rect(x + 2, y + 2, x + kKachelB - 3, y + kKachelH - 3, ColorBlack);

    const int   mitte  = y + kKachelH / 2;
    const int   abst   = 5;                                  // 3 Pixel Balken, 2 Luecke
    const int   x0     = x + (kKachelB - (kWelleBalken * abst - 2)) / 2;
    const float phase  = (float)(esp_timer_get_time() / 1000) * 0.006f;
    const int   hmax   = kKachelH / 2 - 8;

    for (int i = 0; i < kWelleBalken; i++) {
        const int   d      = i > kWelleHalb ? i - kWelleHalb : kWelleHalb - i;
        const float rand   = (float)d / (kWelleHalb + 1);
        const float huelle = 1.0f - 0.6f * rand * rand;
        const float ruhig  = 1.5f + 1.5f * sinf(phase - d * 0.7f);
        int h = (int)((ruhig + welle[d] * (hmax - 3)) * huelle);
        if (h < 1) h = 1;
        if (h > hmax) h = hmax;

        const int bx = x0 + i * abst;
        // Runde Enden: die aeussersten Zeilen nur in der mittleren Spalte.
        display->fill_rect(bx, mitte - h + 1, bx + 2, mitte + h - 1, ColorBlack);
        display->pixel(bx + 1, mitte - h, ColorBlack);
        display->pixel(bx + 1, mitte + h, ColorBlack);
    }
}

// Solange Kacheln zu sehen sind, laeuft die Welle mit etwa 15 Bildern je Sekunde.
static const int64_t kWelleUs = 66000;

// Ab- und Aufblenden dauern je so lange.
static const float kBlendeS = 0.5f;

// Die Animationen sind zurueckgestellt: jeder Clip muss erst erzeugt werden,
// und das ist im Moment zu teuer. Solange das aus ist, steht die Ruhestellung
// still mit den Kacheln davor; Weckwort und KEY bewegen nichts. Filme und
// Ablauf bleiben im Code, damit das Einschalten spaeter nur diese Zeile ist.
static const bool kAnimationen = false;

static void display_task(void *)
{
    bool    vorhang     = false;  // false: wach.film, true: zu.film
    float   pos         = 0.0f;   // Position im jeweiligen Film, in Bildern
    float   hell        = 1.0f;   // 0 schwarz, 1 wie gezeichnet
    int64_t zuletzt     = esp_timer_get_time();
    int64_t aktiv_bis   = 0;
    int64_t gezeichnet  = 0;
    int     bild_vorher    = -1;
    int     stufe_vorher   = -1;
    bool    vorhang_vorher = false;

    // Zum Ausprobieren ohne Weckwort: KEY schaltet zwischen vorn und hinten.
    bool    test_vorn   = false;
    int     key_vorher  = 1;
    int64_t key_zuletzt = 0;

    // Laufzeit des Zeichnens, einmal je Minute ins Log.
    int32_t ms_max = 0;
    int64_t ms_log = zuletzt;

    const float wach_letztes = (float)(wach.bilder() - 1);
    const float zu_letztes   = (float)(zu.bilder() - 1);

    while (true) {
        const int64_t jetzt = esp_timer_get_time();
        const float   dt    = (float)(jetzt - zuletzt) / 1e6f;
        zuletzt = jetzt;

        // Abgefragt im Takt dieser Schleife, 20 bis 40 ms; die Sperre danach
        // ist die Entprellung.
        const int key = gpio_get_level(KEY_BUTTON_PIN);
        if (kAnimationen && key_vorher != 0 && key == 0 && jetzt - key_zuletzt > 200000) {
            key_zuletzt = jetzt;
            test_vorn   = !test_vorn;
            if (!test_vorn) aktiv_bis = 0;   // sofort zurueck, ohne Nachlauf
            ESP_LOGI(TAG, "Szene: KEY, HoiHoi geht nach %s.", test_vorn ? "vorn" : "hinten");
        }
        key_vorher = key;

        if (gespraech_aktiv()) aktiv_bis = jetzt + kNachlaufUs;

        const float blende = dt / kBlendeS;
        if (kAnimationen && (test_vorn || jetzt < aktiv_bis)) {
            // Nach vorn. Zuerst aufhellen, falls gerade abgeblendet wurde.
            hell += blende;
            if (vorhang) {
                // Der Vorhang geht rueckwaerts wieder auf, doppelt so schnell;
                // ist er offen, steht HoiHoi am Ende von wach.film.
                if (hell >= 1.0f) pos -= dt * zu.fps() * 2;
                if (pos <= 0.0f) {
                    vorhang = false;
                    pos     = wach_letztes;
                }
            } else {
                pos += dt * wach.fps();
                if (pos > wach_letztes) pos = wach_letztes;
            }
        } else if (vorhang) {
            // Zurueck: den Vorhang zuziehen, am Ende abblenden, im Schwarz an
            // den Anfang von wach.film.
            pos += dt * zu.fps();
            if (pos >= zu_letztes) {
                pos   = zu_letztes;
                hell -= blende;
                if (hell <= 0.0f) {
                    hell    = 0.0f;
                    vorhang = false;
                    pos     = 0.0f;
                }
            }
        } else if (pos >= wach_letztes && hell >= 1.0f) {
            // Vorn angekommen: von hier aus passt der Vorhang.
            vorhang = true;
            pos     = 0.0f;
        } else if (pos > 0.0f) {
            // Unterwegs zurueckgeschickt: nur abblenden.
            hell -= blende;
            if (hell <= 0.0f) {
                hell = 0.0f;
                pos  = 0.0f;
            }
        } else {
            // Die Ruhestellung aus dem Schwarz aufblenden.
            hell += blende;
        }
        if (hell > 1.0f) hell = 1.0f;

        const int bild  = (int)pos;
        const int stufe = (int)(hell * 64.0f + 0.5f);

        // Die Kacheln sind draussen, sobald HoiHoi ein Stueck gegangen ist oder
        // das Bild dunkel wird; kubisch abbremsend hinaus und herein.
        float a = vorhang ? 1.0f : (float)bild / kKachelBilder;
        if (1.0f - hell > a) a = 1.0f - hell;
        if (a > 1.0f) a = 1.0f;
        const float b       = 1.0f - a;
        const int   weg     = (int)((1.0f - b * b * b) * kKachelWeg);
        const bool  kacheln = weg < kKachelWeg;

        // Neu gezeichnet wird, wenn sich Bild oder Helligkeit aendern oder die
        // Welle weiterlaufen soll. Vorn steht HoiHoi still und die Kacheln
        // sind draussen, dann gibt es nichts zu tun.
        const bool bewegt = (bild != bild_vorher) || (stufe != stufe_vorher)
                            || (vorhang != vorhang_vorher);
        if (!bewegt && (!kacheln || jetzt - gezeichnet < kWelleUs)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        Film &f = vorhang ? zu : wach;
        f.gehe_zu(bild);
        display->bitmap(0, 0, f.breite(), f.hoehe(), f.puffer());
        display->abdunkeln(stufe);

        if (kacheln) {
            welle_schieben();
            kachel_wlan(kKachelL - weg, kKachelY1);
            kachel_klima(kKachelL - weg, kKachelY2);
            kachel_akku(kKachelR + weg, kKachelY1);
            kachel_hoihoi(kKachelR + weg, kKachelY2);
        }
        const int32_t ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
        if (ms > ms_max) ms_max = ms;

        display->flush();
        gezeichnet   = jetzt;
        bild_vorher    = bild;
        stufe_vorher   = stufe;
        vorhang_vorher = vorhang;

        if (jetzt - ms_log > 60000000) {
            ESP_LOGI(TAG, "Szene: Zeichnen hoechstens %d ms je Bild.", (int)ms_max);
            ms_log = jetzt;
            ms_max = 0;
        }
    }
}

// --- Oberflaeche ---------------------------------------------------------------
//
// Solange die Animationen aus sind, steht statt der Szene die Oberflaeche aus
// oberflaeche.h auf dem Display. Dieser Task sammelt nur ein, was sie zeigt.

static oberflaeche::Zustand ui_zustand(void)
{
    const Stt::Phase  sp = stt.phase();
    const Chat::Phase cp = chat.phase();
    const Tts::Phase  tp = tts.phase();

    if (hoeren_gleich) return oberflaeche::Zustand::HoertZu;

    // Wer in eine Antwort hineinspricht, wird auch gehoert — sichtbar bleibt
    // trotzdem die Antwort, sie laeuft ja noch.
    if (tp == Tts::Phase::Spricht) return oberflaeche::Zustand::Antwortet;
    if (listener.listening() || sp == Stt::Phase::Verbindet || sp == Stt::Phase::Hoert) {
        return oberflaeche::Zustand::HoertZu;
    }
    if (sp == Stt::Phase::Wartet || cp == Chat::Phase::Fragt || cp == Chat::Phase::Antwortet
        || tp == Tts::Phase::Holt) {
        return oberflaeche::Zustand::DenktNach;
    }
    return oberflaeche::Zustand::Ruhe;
}

// Die Stimme gegen einen festen Boden statt gegen den Ruhepegel des Raums:
// sie kommt aus dem eigenen Wandler, ihre Lautstaerke haengt nicht vom Raum
// ab. 100 ist Stille, das 32-Fache voller Ausschlag.
static float stimm_anteil(void)
{
    const int32_t p = speaker.pegel();
    if (p <= 100) return 0.0f;
    float a = log2f((float)p / 100.0f) / 5.0f;
    return a > 1.0f ? 1.0f : a;
}

// Probe fuer den Umbau zur Karte: der orange Pilz aus MapleStory laeuft auf
// einer leeren Karte herum. Die Rahmen liegen in assets/karte/pilz.bin (nicht
// im Repo): "PILZ", Anzahl, dann je Rahmen Breite, Hoehe und ein Byte je Pixel,
// zeilenweise: 0 frei, sonst die Helligkeit 1 bis 255. Rahmen 0 bis 2 sind
// Gehen, 3 bis 6 Springen.
//
// Welche Helligkeit welches Muster bekommt, legt eine Kontrastvariante fest;
// gewaehlt ist K7. Wie dick der Umriss wird, legt eine Linienvariante fest,
// gewaehlt ist L2.
//
// Das Muster fuer die Zwischentoene haengt am Display, nicht am Pilz. Hing es
// am Pilz, kippte bei jedem Pixel Bewegung das Schachbrett auf der ganzen
// Kappe um, und das flimmerte. So aendern sich beim Laufen nur die Raender.
//
// Ein Pixel der Grafik ist kPilzMass mal kPilzMass Pixel der Anzeige, und auch
// Position und Boden rechnen in diesem Raster, damit die Pixel der Vorlage
// ganzzahlig deckungsgleich mit denen des Panels bleiben. Solange das an ist,
// zeigt die Anzeige nur den Pilz; Zuhoeren und Antworten laufen weiter.
static const bool    kPilzProbe  = true;
static const int     kPilzMass   = 1;
static const int64_t kPilzTaktUs = 40000;

extern const uint8_t pilz_daten[]  asm("_binary_pilz_bin_start");
extern const uint8_t boden_daten[] asm("_binary_boden_bin_start");

struct PilzBild {
    int            b, h;
    const uint8_t *ton;
};

// Unter grenze[0] schwarz, dann 75, 50, 25 und 12,5 Prozent, ab grenze[4]
// weiss. Zwei gleiche Grenzen lassen die Stufe dazwischen aus.
struct PilzKontrast {
    const char *code;
    uint8_t     grenze[5];
};

static const PilzKontrast kPilzKontraste[] = {
    { "K1", {  70,  70, 150, 185, 185 } },   // bisher
    { "K2", { 100, 100, 165, 205, 205 } },   // haerter
    { "K3", {  80, 130, 180, 215, 215 } },   // dunkler, fuenf Stufen
    { "K4", {  60,  60, 130, 165, 215 } },   // weicher
    { "K5", { 120, 120, 210, 210, 210 } },   // nur Schwarz, Schachbrett, Weiss
    { "K6", {  90,  90,  90, 175, 225 } },   // hell
    { "K7", {  90, 150, 190, 190, 225 } },   // kraeftige Kappe
    { "K8", { 110, 110, 110, 110, 200 } },   // Umriss mit leichtem Schatten
};
static const int kPilzKontrast = 6;   // K7

// Linienvarianten. Der Umriss wird vorab in eine Kopie jedes Rahmens gerechnet,
// mit kPilzRand Pixeln Luft ringsum, damit er nach aussen wachsen kann.
//   aussen: so viele Ringe schwarz um die Silhouette herum
//   rund:   Nachbarn auch ueber Eck, sonst nur waagrecht und senkrecht
//   innen:  der aeusserste Ring innerhalb der Silhouette wird schwarz
//   linien: auch die dunklen Linien im Inneren (Augen, Kappenrand) um ein
//           Pixel verbreitern
struct PilzLinie {
    const char *code;
    uint8_t     aussen;
    bool        rund, innen, linien;
};

static const PilzLinie kPilzLinien[] = {
    { "L1", 0, false, false, false },   // wie K7
    { "L2", 1, false, false, false },   // ein Pixel aussen
    { "L3", 1, true,  false, false },   // ein Pixel aussen, runde Ecken
    { "L4", 0, false, true,  false },   // ein Pixel nach innen
    { "L5", 1, false, true,  false },   // aussen und innen
    { "L6", 2, true,  false, false },   // zwei Pixel aussen
    { "L7", 1, true,  false, true  },   // aussen, dazu innere Linien dicker
    { "L8", 0, false, true,  true  },   // innen, dazu innere Linien dicker
};
static const int kPilzRand  = 2;
static const int kPilzLinie = 1;   // L2

// Rechnet Rahmen q mit Linienvariante l nach ziel, (b + 2R) x (h + 2R).
// Schwarz wird als Helligkeit 1 eingetragen, das liegt unter jeder Grenze.
static void pilz_umriss(const PilzBild &q, const PilzLinie &l, uint8_t *ziel)
{
    const int R = kPilzRand, b = q.b + 2 * R, h = q.h + 2 * R;
    memset(ziel, 0, b * h);
    for (int j = 0; j < q.h; j++) memcpy(ziel + (j + R) * b + R, q.ton + j * q.b, q.b);

    const auto an = [&](int x, int y) -> uint8_t {
        return (x < 0 || y < 0 || x >= b || y >= h) ? 0 : ziel[y * b + x];
    };
    static const int kNb[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
                                   { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };

    // Markierungen erst sammeln, dann eintragen, sonst wuechse eine Aenderung
    // innerhalb desselben Durchgangs weiter. 2 = wird schwarz.
    static uint8_t *marke = nullptr;
    static int      marke_n = 0;
    if (marke_n < b * h) {
        heap_caps_free(marke);
        marke   = (uint8_t *)heap_caps_malloc(b * h, MALLOC_CAP_SPIRAM);
        marke_n = marke ? b * h : 0;
        if (!marke) return;
    }
    const auto durchgang = [&](int art) {
        memset(marke, 0, b * h);
        const int nb = (art == 0 && l.rund) ? 8 : 4;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < b; x++) {
                const uint8_t v = ziel[y * b + x];
                for (int n = 0; n < nb; n++) {
                    const uint8_t w = an(x + kNb[n][0], y + kNb[n][1]);
                    const bool treffer = art == 0 ? (v == 0 && w != 0)             // aussen
                                       : art == 1 ? (v != 0 && w == 0)             // innen
                                                  : (v != 0 && w != 0 && w < 90);  // linien
                    if (treffer) { marke[y * b + x] = 2; break; }
                }
            }
        }
        for (int i = 0; i < b * h; i++) {
            if (marke[i]) ziel[i] = 1;
        }
    };
    // Die inneren Linien zuerst, solange der Umriss noch der alte ist; sonst
    // zaehlte der neue Rand selbst als dunkle Linie und wuechse nach innen.
    if (l.linien) durchgang(2);
    if (l.innen) durchgang(1);
    for (int r = 0; r < l.aussen; r++) durchgang(0);
}

// Ob das Displaypixel (x, y) bei Helligkeit l schwarz wird. Das Muster haengt
// an x und y des Displays, nicht an der Grafik.
static inline bool pilz_schwarz(uint8_t l, int x, int y, const PilzKontrast &k)
{
    int s = 0;
    while (s < 5 && l >= k.grenze[s]) s++;
    switch (s) {
    case 0:  return true;
    case 1:  return !((x & 1) && (y & 1));
    case 2:  return ((x + y) & 1) == 0;
    case 3:  return !(x & 1) && !(y & 1);
    case 4:  return !(y & 1) && ((x + ((y & 2) ? 2 : 0)) & 3) == 0;
    default: return false;
    }
}

static void pilz_zeichnen(const PilzBild &p, int x, int y, bool spiegeln,
                          const PilzKontrast &k)
{
    for (int j = 0; j < p.h; j++) {
        for (int i = 0; i < p.b; i++) {
            const uint8_t l = p.ton[j * p.b + (spiegeln ? p.b - 1 - i : i)];
            if (l == 0) continue;
            const int x0 = (x + i) * kPilzMass, y0 = (y + j) * kPilzMass;
            for (int yy = y0; yy < y0 + kPilzMass; yy++) {
                for (int xx = x0; xx < x0 + kPilzMass; xx++) {
                    display->pixel(xx, yy, pilz_schwarz(l, xx, yy, k) ? ColorBlack : ColorWhite);
                }
            }
        }
    }
}

// Hintergrundobjekte aus dem Dragon-Road-Sheet in assets/karte/objekte.bin:
// "OBJK", Anzahl, je Objekt Breite und Hoehe (u16, little endian), dann ein
// Byte je Pixel wie beim Pilz. Objekt 0 ist ein Baum, 1 und 2 sind Buesche,
// 3 eine Tulpe. 4 und 5 sind Baum und Busch von GPT Image 2, 6 und 7 dieselben
// von Nano Banana 2, 8 und 9 von Recraft.
//
// Zum Vergleich der Quellen schaltet BOOT kurz zwischen den Szenen um, der
// Code steht oben links.
extern const uint8_t objekt_daten[] asm("_binary_objekte_bin_start");

// Wo die Objekte stehen: Mitte in x, von hinten nach vorn gezeichnet.
struct ObjektPlatz {
    int objekt, mitte;
};
struct ObjektSzene {
    const char       *code;
    int               n;
    const ObjektPlatz plaetze[4];
};
static const ObjektSzene kObjektSzenen[] = {
    { "H1", 3, { { 10, 75 }, { 11, 200 }, { 12, 325 } } },             // Hollow-Knight-Stil, 160 px
    { "H2", 3, { { 13, 75 }, { 14, 200 }, { 15, 325 } } },             // derselbe, 116 px
    { "M", 4, { { 0, 300 }, { 1, 110 }, { 2, 350 }, { 3, 215 } } },   // MapleStory
    { "R", 2, { { 8, 300 }, { 9, 110 } } },                           // Recraft
};

// Setzt oder loescht ein Pixel im 1-Bit-Bild bild (Zeilen, hoechstes Bit
// links, 1 = schwarz), mit Bereichspruefung.
static inline void bild_punkt(uint8_t *bild, int x, int y, bool schwarz)
{
    if (x < 0 || y < 0 || x >= 400 || y >= 300) return;
    const int i = y * 400 + x;
    if (schwarz) bild[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    else         bild[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
}

[[noreturn]] static void pilz_laufen(void)
{
    static PilzBild bilder[8];
    int             anzahl = 0;
    if (memcmp(pilz_daten, "PILZ", 4) == 0) {
        const uint8_t *p = pilz_daten + 5;
        for (int k = 0; k < pilz_daten[4] && k < 8; k++) {
            const int b = p[0], h = p[1];
            bilder[k] = { b, h, p + 2 };
            p += 2 + b * h;
            anzahl++;
        }
    }
    ESP_LOGI(TAG, "Pilz: %d Rahmen.", anzahl);

    // Kopien mit Umriss, je Rahmen einmal fuer die gewaehlte Linienvariante.
    static PilzBild strich[8];
    for (int k = 0; k < anzahl; k++) {
        const int b = bilder[k].b + 2 * kPilzRand, h = bilder[k].h + 2 * kPilzRand;
        uint8_t *puf = (uint8_t *)heap_caps_malloc(b * h, MALLOC_CAP_SPIRAM);
        if (puf == nullptr) { anzahl = 0; break; }
        strich[k] = { b, h, puf };
        pilz_umriss(bilder[k], kPilzLinien[kPilzLinie], puf);
    }
    if (anzahl < 7) {
        while (true) vTaskDelay(portMAX_DELAY);
    }

    // Der Boden ist die Linie, auf der die Fuesse stehen.
    const int breite = display->width() / kPilzMass;
    // Boden aus den Dragon-Road-Tiles, fertig auf ein Bit gebracht in mehreren
    // Varianten: "BODN", Anzahl, Breite (u16), Hoehe, dann die Bitbilder.
    // Gewaehlt ist B1. Der Streifen ragt kBodenTiefer Pixel unten aus dem Bild;
    // die Fuesse stehen ein Stueck im Gras.
    static const int kBodenVariante = 0;
    static const int kBodenTiefer   = 20;
    const bool boden_ok = memcmp(boden_daten, "BODN", 4) == 0 && boden_daten[4] > kBodenVariante;
    const int  boden_b  = boden_ok ? boden_daten[5] | (boden_daten[6] << 8) : 0;
    const int  boden_h  = boden_ok ? boden_daten[7] : 0;
    const int  boden_y  = display->height() - boden_h + kBodenTiefer;
    const int  boden    = (boden_ok ? boden_y + 10 : display->height() - 28) / kPilzMass;
    const int rand_px = 34 / kPilzMass;

    static PilzBild objekte[16];
    int             objekt_n = 0;
    if (memcmp(objekt_daten, "OBJK", 4) == 0) {
        const uint8_t *p = objekt_daten + 5;
        for (int k = 0; k < objekt_daten[4] && k < 16; k++) {
            const int b = p[0] | (p[1] << 8), h = p[2] | (p[3] << 8);
            objekte[k] = { b, h, p + 4 };
            p += 4 + b * h;
            objekt_n++;
        }
    }

    // Boden und Objekte stehen still. Sie werden deshalb nur beim Umschalten
    // einmal in ein eigenes Bild gerechnet, das jeder Takt als Ganzes
    // uebernimmt; darauf kommt dann nur noch der Pilz.
    uint8_t *hinter = (uint8_t *)heap_caps_calloc(1, 15000, MALLOC_CAP_SPIRAM);
    if (hinter == nullptr) {
        while (true) vTaskDelay(portMAX_DELAY);
    }
    const auto hintergrund_bauen = [&](const ObjektSzene &szene) {
        memset(hinter, 0, 15000);
        if (boden_ok && boden_b == 400) {
            const uint8_t *bits = boden_daten + 8 + kBodenVariante * 50 * boden_h;
            for (int j = 0; j < boden_h; j++) {
                const int y = boden_y + j;
                if (y >= 0 && y < 300) memcpy(hinter + y * 50, bits + j * 50, 50);
            }
        }
        // Jedes Objekt bekommt denselben Umriss wie der Pilz und steht mit dem
        // Fuss dort, wo auch der Pilz steht.
        for (int k = 0; k < szene.n; k++) {
            const ObjektPlatz &platz = szene.plaetze[k];
            if (platz.objekt >= objekt_n) continue;
            const PilzBild &o = objekte[platz.objekt];
            const int b = o.b + 2 * kPilzRand, h = o.h + 2 * kPilzRand;
            uint8_t  *puf = (uint8_t *)heap_caps_malloc(b * h, MALLOC_CAP_SPIRAM);
            if (puf == nullptr) continue;
            pilz_umriss(o, kPilzLinien[kPilzLinie], puf);
            const int x0 = platz.mitte - b / 2;
            const int y0 = boden * kPilzMass - o.h - kPilzRand;
            for (int j = 0; j < h; j++) {
                for (int i = 0; i < b; i++) {
                    const uint8_t l = puf[j * b + i];
                    if (l == 0) continue;
                    bild_punkt(hinter, x0 + i, y0 + j,
                               pilz_schwarz(l, x0 + i, y0 + j, kPilzKontraste[kPilzKontrast]));
                }
            }
            heap_caps_free(puf);
        }
    };
    const int     szenen_n    = (int)(sizeof kObjektSzenen / sizeof kObjektSzenen[0]);
    const int32_t boot_anfang = boot_kurz;
    int           szene_jetzt = -1;
    ESP_LOGI(TAG, "Pilz: %d Objekte.", objekt_n);

    int64_t   ms_log = esp_timer_get_time();
    int32_t   takte  = 0;

    enum class Tun { Stehen, Gehen, Anlauf, Luft, Landen };
    Tun     tun      = Tun::Stehen;
    int     dauer    = 20;       // Takte bis zur naechsten Entscheidung
    int     schritt  = 0;        // Takte im aktuellen Zustand
    int     mitte    = breite / 2;
    int     richtung = -1;       // -1 links, +1 rechts; die Vorlage schaut nach links
    bool    weiter   = false;    // laeuft beim Sprung seitlich mit
    int     hoehe4   = 0;        // Hoehe ueber dem Boden in Viertelpixeln
    int     tempo4   = 0;
    int64_t takt     = 0;

    const auto zufall = [](int n) { return (int)(esp_random() % (uint32_t)n); };

    while (true) {
        const int64_t jetzt = esp_timer_get_time();
        if (jetzt - takt < kPilzTaktUs) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        takt = jetzt;
        schritt++;
        if (++takte == 250) {
            ESP_LOGI(TAG, "Pilz: %" PRId64 " ms je Takt.", (jetzt - ms_log) / 1000 / takte);
            ms_log = jetzt;
            takte  = 0;
        }

        // --- Verhalten, wie ein Monster in MapleStory: stehen, ein Stueck
        // gehen, ab und zu springen, am Rand umdrehen.
        int rahmen = 0;
        switch (tun) {
        case Tun::Stehen:
            rahmen = 0;
            if (schritt >= dauer) {
                const int w = zufall(100);
                schritt     = 0;
                if (w < 55) {
                    tun      = Tun::Gehen;
                    richtung = zufall(2) ? 1 : -1;
                    dauer    = 20 + zufall(60);
                } else if (w < 75) {
                    tun    = Tun::Anlauf;
                    weiter = false;
                } else {
                    dauer = 10 + zufall(30);
                }
            }
            break;
        case Tun::Gehen: {
            static const int kFolge[] = { 0, 1, 2, 1 };
            rahmen = kFolge[(schritt / 3) % 4];
            mitte += richtung;
            if (mitte < rand_px || mitte > breite - rand_px) {
                richtung = -richtung;
                mitte += 2 * richtung;
            }
            if (zufall(60) == 0) {
                tun     = Tun::Anlauf;
                weiter  = true;
                schritt = 0;
            } else if (schritt >= dauer) {
                tun     = Tun::Stehen;
                dauer   = 10 + zufall(40);
                schritt = 0;
            }
            break;
        }
        case Tun::Anlauf:
            rahmen = 4;
            if (schritt >= 2) {
                tun     = Tun::Luft;
                tempo4  = 26;
                hoehe4  = 0;
                schritt = 0;
            }
            break;
        case Tun::Luft:
            hoehe4 += tempo4;
            tempo4 -= 3;
            rahmen = tempo4 > 0 ? 5 : 6;
            if (weiter) {
                mitte += richtung;
                if (mitte < rand_px || mitte > breite - rand_px) richtung = -richtung;
            }
            if (hoehe4 <= 0) {
                hoehe4  = 0;
                tun     = Tun::Landen;
                schritt = 0;
            }
            break;
        case Tun::Landen:
            rahmen = 4;
            if (schritt >= 2) {
                tun     = weiter ? Tun::Gehen : Tun::Stehen;
                dauer   = weiter ? 10 + zufall(40) : 10 + zufall(30);
                schritt = 0;
            }
            break;
        }

        // --- Zeichnen: leere Karte, Boden, Pilz mit den Fuessen auf dem Boden.
        const int v = (int)((uint32_t)(boot_kurz - boot_anfang) % (uint32_t)szenen_n);
        if (v != szene_jetzt) {
            hintergrund_bauen(kObjektSzenen[v]);
            ESP_LOGI(TAG, "Pilz: Szene %s.", kObjektSzenen[v].code);
            szene_jetzt = v;
        }
        display->bitmap(0, 0, display->width(), display->height(), hinter);
        display->text(6, 6, kObjektSzenen[v].code, ColorBlack, 2);

        // Die Fuesse des Originals bleiben auf dem Boden; ein Umriss darunter
        // verschwindet im Gras.
        const PilzBild &p = strich[rahmen];
        pilz_zeichnen(p, mitte - p.b / 2, boden - bilder[rahmen].h - kPilzRand - hoehe4 / 4,
                      richtung > 0, kPilzKontraste[kPilzKontrast]);
        display->flush();
    }
}

// Das Projekt baut mit -Og; die Landschaft rechnet jeden Takt 120000 Pixel
#pragma GCC push_options
#pragma GCC optimize("O2")

// --- Probe: Landschaft wie Alto's Adventure --------------------------------
// Drei Silhouetten-Ebenen, Tiefe nur durch Dunst: jede Ebene ist eine flache
// Form mit eigenem Ton, hinten hell, vorn voll schwarz und nah an der Kamera.
// Der Himmel bleibt weiss, dort stehen spaeter Uhrzeit und Wetter; Wolken und
// der Schein um die Sonne tragen helle Toene. Toene sind 0 (weiss) bis 16
// (schwarz) ueber eine 4x4-Bayer-Matrix, fest am Display, damit nichts
// flimmert, wenn die Ebenen ziehen.
//
// Sonne und Mond ziehen auf einer flachen Ellipse von links nach rechts, tief
// am Horizont groesser; die Toene folgen ihrer Hoehe. Im Testlauf
// (BOOT 3 s gehalten) laeuft je Wetterlage ein Tag und eine Nacht von je 3 s, dann
// geht es weich zur naechsten Lage, BOOT springt sofort weiter. Sonst
// nimmt LIVE Uhrzeit und echtes Wetter: Bedeckung gibt die Wolkenmenge, Wind ihr Tempo,
// Niederschlag den Regen, Gewitter dunkle Wolken und Blitze.
//
// Wie bergig die Landschaft ist, kommt aus dem Gelaende um den Ort der IP;
// die Position ist zugleich der Keim fuer Baeume und Wolken, jeder Ort hat
// also seine eigene Landschaft.
//
// Gezeichnet wird zeilenweise mit ganzen Bytes: das Bayer-Muster wiederholt
// sich alle 4 Pixel, also ist eine Tonzeile ein einziger Bytewert, und eine
// Ebene legt sich per Maske darueber.
static const bool    kDunstProbe  = true;
static const int64_t kDunstTaktUs = 40000;
static const int     kBergTest    = -1;     // 0..1000 erzwingt eine Bergigkeit, -1: aus dem Gelaende
static const int     kWeltB       = 1200;   // Breite einer Ebene in Pixeln, danach wiederholt sie sich
static const int     kWeltBytes   = kWeltB / 8;
static const int     kEbenen      = 3;
static const int     kVorn        = kEbenen - 1;
static const int     kWolken      = 16;
static const int     kTropfen     = 220;
static const int     kFlocken     = 160;
static const int64_t kHaltenUs    = 3000000;   // Dauer eines Tages, ebenso einer Nacht, im Testlauf
static const int     kBlende      = 40;        // Takte fuer den Uebergang

static const uint8_t kBayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

struct DunstWetter {
    const char *code;
    uint8_t     wolken;        // Bedeckung 0..100
    int8_t      wolken_ton;    // dunkler als die Tageszeit
    uint8_t     regen, schnee; // 0..100
    uint8_t     gewitter;      // 1: Blitze
    uint8_t     wind;          // km/h
    uint8_t     dunst;         // 0..100, hellt die hinteren Ebenen auf
    uint8_t     himmel;        // so viel dunkler wird der Himmel
};
static const DunstWetter kWetterlagen[] = {
    { "SONNIG",   0,   0,  0,   0,  0, 8,  0   , 0 },
    { "HEITER",   35,  0,  0,   0,  0, 15, 0   , 0 },
    { "BEWOELKT", 95,  3,  0,   0,  0, 25, 10  , 1 },
    { "NEBEL",    15,  -1, 0,   0,  0, 3,  100 , 1 },
    { "NIESEL",   85,  3,  25,  0,  0, 15, 25  , 1 },
    { "REGEN",    95,  4,  70,  0,  0, 30, 40  , 2 },
    { "GEWITTER", 100, 9,  100, 0,  1, 50, 20  , 3 },
    { "SCHNEE",   90,  2,  0,   70, 0, 12, 35  , 1 },
};
static const int kAnzWetter = (int)(sizeof kWetterlagen / sizeof kWetterlagen[0]);

struct DunstEbene {
    uint8_t *bits;          // kWeltB x Hoehe, ein Bit je Pixel, hoechstes Bit links
    uint8_t *schnee[2];     // Oberkanten derselben Form, duenn und dick: dort liegt Schnee
    int      oben, unten;   // Zeilen, zwischen denen ueberhaupt etwas steht
    float    tempo;         // Pixel je Takt
};

// Zweiter Zeichner: rechnet einen Teil der Bildzeilen auf dem anderen Kern.
// Der Auftrag ist ein Funktionszeiger mit Kontext, gestartet und abgeholt
// ueber zwei Semaphoren; die Zeilen beider Teile beruehren sich nicht.
static SemaphoreHandle_t helfer_los = nullptr, helfer_fertig = nullptr;
static void (*helfer_fn)(void *) = nullptr;
static void *helfer_ctx = nullptr;

static void helfer_task(void *)
{
    while (true) {
        xSemaphoreTake(helfer_los, portMAX_DELAY);
        helfer_fn(helfer_ctx);
        xSemaphoreGive(helfer_fertig);
    }
}

[[noreturn]] static void dunst_laufen(void)
{
    const int B = display->width(), H = display->height(), BB = B / 8;
    static DunstEbene ebenen[kEbenen];
    uint8_t *bild = (uint8_t *)heap_caps_calloc(1, BB * H, MALLOC_CAP_SPIRAM);
    for (auto &e : ebenen)
        for (uint8_t *&m : e.schnee) m = (uint8_t *)heap_caps_calloc(1, kWeltBytes * H, MALLOC_CAP_SPIRAM),
        e.bits = (uint8_t *)heap_caps_calloc(1, kWeltBytes * H, MALLOC_CAP_SPIRAM);

    static const float kTempo[kEbenen] = { 0.02f, 0.08f, 0.25f };

    // Ganzzahlige Wurzel als Tabelle fuer den Schein um Sonne und Mond
    static const int kWurzelMax = 160;
    uint8_t *wurzel = (uint8_t *)heap_caps_malloc(kWurzelMax * kWurzelMax, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < kWurzelMax * kWurzelMax; i++) wurzel[i] = (uint8_t)sqrt((float)i);
    uint8_t halo_ton[kWurzelMax];

    // Kleiner Planet: das flache Bild wird um einen Mittelpunkt weit unter dem
    // Display gelegt. Jede Zeile ist ein Kreis um ihn; zum Rand hin sinkt sie
    // um bogen[x] ab, und weiter oben liegt sie auf einem groesseren Kreis,
    // wird also breiter gezogen (zeile_s < 1). So neigen sich Baeume zum Rand
    // hin, statt verzerrt zu werden
    static const int   kBogen  = 14;   // so weit sinkt der untere Rand an den Seiten
    static const float kRadius = (200.0f * 200.0f + kBogen * kBogen) / (2.0f * kBogen);   // Radius der untersten Zeile
    static int8_t      bogen[400];
    static int32_t     zeile_s[300];   // Schritt im flachen Bild je Bildpixel, 16.16
    for (int x = 0; x < B; x++) {
        const float dx = x + 0.5f - B / 2.0f;
        bogen[x] = (int8_t)(kRadius - sqrt(kRadius * kRadius - dx * dx) + 0.5f);
    }
    for (int y = 0; y < H; y++) zeile_s[y] = (int32_t)(65536.0f * kRadius / (kRadius + (H - 1 - y)));
    // Die letzten flachen Tonzeilen. Im PSRAM: gemessen kaum langsamer, und der
    // interne Speicher reicht sonst nicht fuer Aufnahme, KI und Wetter zugleich
    uint8_t (*const ring)[400] = (uint8_t (*)[400])heap_caps_calloc(kBogen + 1, 400, MALLOC_CAP_SPIRAM);

    // Kronen der vorderen Baeume, dort fallen die Blaetter heraus
    struct Krone { int16_t x, y, r; };
    Krone kronen[8];
    int   kronen_n = 0;
    // Funktuerme: eigene Maske der mittleren Ebene, kraeftiger getoent, damit
    // das feine Gitter im Dunst nicht zerfaellt; die Spitzen senden bei WLAN
    static const int kTuerme = 2;
    uint8_t *turm = (uint8_t *)heap_caps_calloc(1, kWeltBytes * H, MALLOC_CAP_SPIRAM);
    int16_t  turm_x[kTuerme] = {}, turm_y[kTuerme] = {};
    // Woelfe: nachts laufen ein paar ueber den Boden der vorderen Ebene, bleiben
    // stehen, drehen um und heulen, wenn der Mond hoch steht
    struct Wolf { float x; int8_t richtung; float tempo; int16_t pause; bool heult, aktiv; };
    static Wolf woelfe[3];
    // Nicht auf den Stapel und nicht intern: der interne Speicher gehoert Aufnahme und TLS
    int16_t (*const grund)[kWeltB] = (int16_t (*)[kWeltB])heap_caps_calloc(kEbenen * kWeltB, sizeof(int16_t), MALLOC_CAP_SPIRAM);

    // Wolken als Buckel mit flacher Unterkante; schwelle sagt, ab welcher
    // Bedeckung sie erscheint, so verteilt, dass wenige Wolken weit auseinander stehen
    struct Wolke { float x; int16_t y, w; float schwelle; uint8_t n; int8_t dx[9]; uint8_t r[9]; };
    static Wolke wolken[kWolken];
    struct Buckel { int16_t cx, cy, r, boden, wolke; };
    static Buckel buckel[kWolken * 9];   // intern: jede Zeile liest sie

    struct Stern { int16_t x, y; };
    Stern sterne_pos[40];
    struct Blatt { float x, y, vy, phase; bool aktiv; };   // x in Koordinaten der vorderen Ebene
    static Blatt blaetter[40];
    struct Tropfen { float x, y; uint8_t laenge; };
    Tropfen *const tropfen = (Tropfen *)heap_caps_calloc(kTropfen, sizeof(Tropfen), MALLOC_CAP_SPIRAM);
    struct Flocke { float x, y, phase; };
    Flocke *const flocken = (Flocke *)heap_caps_calloc(kFlocken, sizeof(Flocke), MALLOC_CAP_SPIRAM);

    uint32_t keim = 12345;
    const auto zufall = [&keim](int n) { keim = keim * 1664525u + 1013904223u; return (int)((keim >> 8) % (uint32_t)n); };
    const float tau = 2.0f * (float)M_PI;

    for (int i = 0; i < kTropfen; i++) { Tropfen &t = tropfen[i]; t.x = (float)zufall(B + 100); t.y = (float)zufall(H); t.laenge = (uint8_t)(5 + zufall(5)); }
    for (int i = 0; i < kFlocken; i++) { Flocke &f = flocken[i]; f.x = (float)zufall(B); f.y = (float)zufall(H); f.phase = zufall(628) / 100.0f; }

    // Baut alle Masken neu; berg 0 flach .. 1 Hochgebirge
    const auto bauen = [&](float berg, uint32_t ort) {
        keim = ort ^ 12345u;
        kronen_n = 0;
        for (auto &b : blaetter) b.aktiv = false;
        for (int k = 0; k < kEbenen; k++) {
            DunstEbene &e = ebenen[k];
            e.tempo = kTempo[k];
            memset(e.bits, 0, kWeltBytes * H);
            const auto setzen = [&](int x, int y) {
                x = ((x % kWeltB) + kWeltB) % kWeltB;
                if (y < 0 || y >= H) return;
                e.bits[y * kWeltBytes + (x >> 3)] |= 0x80 >> (x & 7);
            };
            const auto kreis = [&](int cx, int cy, int r) {
                for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
                    if (dx * dx + dy * dy <= r * r) setzen(cx + dx, cy + dy);
            };
            // Tanne in Etagen: jede Etage ein Rock, der oben schmal ansetzt und
            // unten ausschwingt; die naechste ueberlappt ihn zur Haelfte. Grosse
            // Tannen bekommen gezackte Zweigspitzen und hochgebogene Enden
            const auto tanne = [&](int x, int g, int h) {
                const int   n     = h < 45 ? 3 : 6;
                const int   krone = h - (h < 45 ? 3 : h / 12);   // darunter Stamm
                const float breit = h * 0.21f;                    // halbe Breite unten
                const bool  gross = h >= 45;
                for (int i = 0; i < n; i++) {
                    const int y0 = g - h + krone * i / (n + 1), y1 = g - h + krone * (i + 2) / (n + 1);
                    const float wb = breit * (i + 2) / (n + 1);
                    const float ansatz = i == 0 ? 0.0f : 0.3f;
                    for (int y = y0; y <= y1; y++) {
                        const float f = (float)(y - y0) / (y1 - y0 > 0 ? y1 - y0 : 1);
                        int halb = (int)(wb * (ansatz + (1 - ansatz) * f) + 0.5f);
                        if (gross) {
                            halb -= ((y * 37 + i * 11) >> 3) % 3 == 0 ? 2 : 0;   // Zacken
                            if (y1 - y < 3) halb -= (3 - (y1 - y)) * 2;          // Enden heben sich
                        }
                        for (int j = -halb; j <= halb; j++) setzen(x + j, y);
                        if (gross && y1 - y < 3)   // Zweigspitzen, die ueber den Saum haengen
                            for (int s = -1; s <= 1; s += 2) setzen(x + s * (halb + 1), y - 1);
                    }
                }
                const int stamm = h < 45 ? 1 : (h / 60 > 2 ? h / 60 : 2);
                for (int y = g - h + krone; y <= g; y++) for (int j = -stamm; j <= stamm; j++) setzen(x + j, y);
            };
            // Huegellinie: weiche Sinuswellen, mit der Bergigkeit zu spitzen
            // Graten gemischt; ganze Perioden, damit sie nahtlos umlaeuft
            static const int kBasis[kEbenen] = { 222, 250, 276 };
            const float amp   = k == 0 ? 4 + 26 * berg : (k == 1 ? 3 + 12 * berg : 3 + 8 * berg);
            const float basis = kBasis[k] - (k == 0 ? 10 * berg : (k == 1 ? 3 * berg : 0));
            const float grat  = k == 0 ? berg : (k == 1 ? berg * 0.5f : 0.0f);
            const float p1 = zufall(628) / 100.0f, p2 = zufall(628) / 100.0f, p3 = zufall(628) / 100.0f;
            for (int x = 0; x < kWeltB; x++) {
                const float u     = (float)x / kWeltB;
                const float weich = 0.55f * sin(tau * (2 + k % 2) * u + p1) + 0.3f * sin(tau * (5 + k) * u + p2)
                                  + 0.15f * sin(tau * (11 + 2 * k) * u + p3);
                const float g1 = 1 - fabs(sin(M_PI * (3 + k) * u + p1)), g2 = 1 - fabs(sin(M_PI * (8 + k) * u + p2));
                const float spitz = 2 * (0.7f * g1 * g1 + 0.3f * g2 * g2) - 1;
                grund[k][x] = (int16_t)(basis - amp * (weich * (1 - grat) + spitz * grat));
                for (int y = grund[k][x]; y < H; y++) setzen(x, y);
            }
            if (k == 1) {
                // Mitte: kleine Baeume, im Gebirge fast nur Tannen
                for (int n = 0; n < 14; n++) {
                    const int x = zufall(kWeltB), g = grund[k][x] + 2;
                    if (zufall(1000) > 300 + 650 * berg) {
                        const int r = 6 + zufall(4);
                        for (int y = g - r - 2; y <= g; y++) setzen(x, y);
                        kreis(x, g - r - 4 - r / 2, r);
                    } else {
                        tanne(x, g, 22 + zufall(10));
                    }
                }
            } else if (k == kVorn) {
                // Vorn: wenige grosse Baeume, gleichmaessig verteilt, damit
                // immer einer im Bild steht; Kronen reichen weit nach oben
                for (int n = 0; n < 5; n++) {
                    const int x = n * kWeltB / 5 + zufall(80), g = grund[k][x] + 4;
                    if (zufall(1000) > 200 + 700 * berg || n == 0) {
                        const int r = 30 + zufall(10), stamm = 7 + zufall(3), sh = 60 + zufall(20);
                        for (int y = g - sh; y <= g; y++) {
                            const int w = stamm + (g - y < 10 ? (10 - (g - y)) / 2 : 0);   // Stamm unten breiter
                            for (int i = -w / 2; i <= w / 2; i++) setzen(x + i, y);
                        }
                        for (int i = 0; i < 18; i++) for (int j = 0; j < 3; j++) setzen(x + i, g - sh + 22 - i / 2 + j);
                        const int cy = g - sh - r / 3;
                        kreis(x, cy, r);
                        kreis(x - r * 8 / 10, cy + r / 3, r * 7 / 10);
                        kreis(x + r * 8 / 10, cy + r / 4, r * 7 / 10);
                        kreis(x + r / 3, cy - r * 6 / 10, r * 6 / 10);
                        if (kronen_n < 8) kronen[kronen_n++] = { (int16_t)x, (int16_t)cy, (int16_t)(r + r / 2) };
                    } else {
                        tanne(x, g, 130 + zufall(30));
                        for (int y = g - 14; y <= g; y++) for (int i = -3; i <= 3; i++) setzen(x + i, y);
                    }
                }
                // Hohes Gras
                for (int n = 0; n < 420; n++) {
                    const int x = zufall(kWeltB), g = grund[k][x] + 1, h = 5 + zufall(zufall(4) ? 10 : 22), neig = zufall(7) - 3;
                    for (int y = 0; y < h; y++) setzen(x + neig * y / h, g - y);
                }
            }
            e.oben = H; e.unten = -1;
            for (int y = 0; y < H; y++)
                for (int i = 0; i < kWeltBytes; i++)
                    if (e.bits[y * kWeltBytes + i]) { if (e.oben == H) e.oben = y; e.unten = y; break; }
            // Schnee: wo die Form von oben beginnt, die obersten Pixel, vorn dicker;
            // die Dicke schwankt leicht, damit die Kante nicht wie gezogen aussieht
            static const int kDuenn[kEbenen] = { 1, 2, 2 }, kDick[kEbenen] = { 2, 3, 5 };
            memset(e.schnee[0], 0, kWeltBytes * H);
            memset(e.schnee[1], 0, kWeltBytes * H);
            for (int x = 0; x < kWeltB; x++) {
                const int wackel = (int)(1.2f + sin(x * 0.21f) + sin(x * 0.047f));
                int  d0 = 0, d1 = 0;
                bool vorher = false;
                for (int y = 0; y < H; y++) {
                    const int     i   = y * kWeltBytes + (x >> 3);
                    const uint8_t bit = 0x80 >> (x & 7);
                    const bool    da  = e.bits[i] & bit;
                    if (da && !vorher) { d0 = kDuenn[k]; d1 = kDick[k] + (k == kVorn ? wackel : wackel / 2); }
                    if (da && d0 > 0) { e.schnee[0][i] |= bit; d0--; }
                    if (da && d1 > 0) { e.schnee[1][i] |= bit; d1--; }
                    vorher = da;
                }
            }
        }
        {
            DunstEbene &e = ebenen[1];
            memset(turm, 0, kWeltBytes * H);
            const auto punkt = [&](int x, int y) {
                x = ((x % kWeltB) + kWeltB) % kWeltB;
                if (y < 0 || y >= H) return;
                e.bits[y * kWeltBytes + (x >> 3)] |= 0x80 >> (x & 7);
                turm[y * kWeltBytes + (x >> 3)] |= 0x80 >> (x & 7);
                if (y < e.oben) e.oben = y;
            };
            const auto linie = [&](float x0, float y0, float x1, float y1) {
                const int n = (int)(fabs(x1 - x0) > fabs(y1 - y0) ? fabs(x1 - x0) : fabs(y1 - y0)) + 1;
                for (int i = 0; i <= n; i++) punkt((int)lround(x0 + (x1 - x0) * i / n), (int)lround(y0 + (y1 - y0) * i / n));
            };
            for (int i = 0; i < kTuerme; i++) {
                const int cx = (int)(kWeltB * (0.22f + 0.5f * i)) + zufall(120);
                int       fuss = 0;
                for (int dx = -8; dx <= 8; dx++) {
                    const int g = grund[1][((cx + dx) % kWeltB + kWeltB) % kWeltB];
                    if (g > fuss) fuss = g;
                }
                const int   hoehe = 50 + zufall(12), oben = fuss - hoehe;
                const float unten_b = 8, oben_b = 1.5f;
                const auto  halb = [&](float y) { return unten_b + (oben_b - unten_b) * (fuss + 2 - y) / (hoehe + 2); };
                // Beine, zwei Pixel breit
                for (int si = 0; si < 2; si++)
                    for (int dd = 0; dd < 2; dd++) {
                        const float s = si ? 1.0f : -1.0f;
                        linie(cx + s * (unten_b + dd * 0.8f), (float)fuss + 2, cx + s * (oben_b + dd * 0.8f), (float)oben);
                    }
                // Kreuzstreben, nach oben enger
                for (float y = (float)fuss; y > oben + 4;) {
                    const float h2 = 7 * halb(y) / unten_b + 3, y2 = y - h2;
                    linie(cx - halb(y), y, cx + halb(y2), y2);
                    linie(cx + halb(y), y, cx - halb(y2), y2);
                    linie(cx - halb(y2), y2, cx + halb(y2), y2);
                    y = y2;
                }
                // Plattform, Schuesseln, Mast
                linie((float)cx - 4, (float)oben, (float)cx + 4, (float)oben);
                for (int dy = 0; dy < 4; dy++) { punkt(cx - 5, oben + 6 + dy); punkt(cx - 6, oben + 6 + dy); punkt(cx + 4, oben + 14 + dy); punkt(cx + 5, oben + 14 + dy); }
                linie((float)cx, (float)oben, (float)cx, (float)oben - 14);
                linie((float)cx + 1, (float)oben, (float)cx + 1, (float)oben - 10);
                punkt(cx - 1, oben - 14); punkt(cx + 1, oben - 14);
                turm_x[i] = (int16_t)cx;
                turm_y[i] = (int16_t)(oben - 15);
            }
        }
        for (int n = 0; n < kWolken; n++) {
            Wolke &c = wolken[n];
            int umgekehrt = 0;   // Bits von n gespiegelt: 0, 8, 4, 12, ... liegen weit auseinander
            for (int b = 0; b < 4; b++) if (n & (1 << b)) umgekehrt |= 8 >> b;
            c.x        = (float)(n * kWeltB / kWolken + zufall(40));
            c.y        = (int16_t)(34 + zufall(86));
            c.w        = (int16_t)(40 + zufall(70));
            c.schwelle = (umgekehrt + zufall(100) / 100.0f) / kWolken;
            c.n        = (uint8_t)(3 + c.w / 22);
            for (int i = 0; i < c.n; i++) {
                c.dx[i] = (int8_t)(-c.w / 2 + i * c.w / (c.n - 1));
                c.r[i]  = (uint8_t)((i == 0 || i == c.n - 1) ? 7 + zufall(4) : 10 + zufall(10));
            }
        }
        for (auto &st : sterne_pos) {
            const int r = zufall(1000);
            st.x = (int16_t)(5 + zufall(B - 10));
            st.y = (int16_t)(6 + 118 * r * r / 1000000);
        }
        ESP_LOGI(TAG, "Dunst: Landschaft gebaut, Bergigkeit %d, %d Kronen.", (int)(berg * 1000), kronen_n);
    };
    bauen((kBergTest >= 0 ? kBergTest : 150) / 1000.0f, 0);

    // Wetter als Kommazahlen, damit Wechsel weich laufen
    struct Lage { float wt, wo, rg, sn, gw, wi, du, hi; };
    const auto lage = [](int wl) {
        const DunstWetter &l = kWetterlagen[wl];
        return Lage{ (float)l.wolken_ton, (float)l.wolken, (float)l.regen, (float)l.schnee,
                     (float)l.gewitter, (float)l.wind, l.dunst / 100.0f, (float)l.himmel };
    };
    // LIVE: Lage aus dem Wetterschluessel, Menge, Regen und Wind aus den Messwerten
    const auto live_lage = [&lage]() {
        const wetter::Stand ws = wetter::stand();
        if (!ws.gueltig) return lage(1);
        int wl = 2;
        switch (ws.art) {
        case wetter::Art::Sonnig:   wl = 0; break;
        case wetter::Art::Heiter:   wl = 1; break;
        case wetter::Art::Niesel:   wl = 4; break;
        case wetter::Art::Regen:    wl = 5; break;
        case wetter::Art::Gewitter: wl = 6; break;
        case wetter::Art::Schnee:   wl = 7; break;
        default:                    wl = ws.nebel ? 3 : 2; break;
        }
        static int zuletzt = -1;
        if (wl != zuletzt) { ESP_LOGI(TAG, "Dunst: %s.", kWetterlagen[wl].code); zuletzt = wl; }
        Lage l = lage(wl);
        l.wo = (float)(ws.wolken_pct < 0 ? 0 : (ws.wolken_pct > 100 ? 100 : ws.wolken_pct));
        l.wi = ws.wind_kmh10 / 10.0f > 80 ? 80 : ws.wind_kmh10 / 10.0f;
        if (l.rg > 0 && ws.regen_mm10 * 10 > l.rg) l.rg = ws.regen_mm10 * 10 > 100 ? 100 : ws.regen_mm10 * 10;
        return l;
    };
    // LIVE: Tageszeit aus Uhr und Ort. Auf- und Untergang aus der Sonnenbahn
    // (Deklination, Zeitgleichung, 0,83 Grad fuer Brechung und Scheibe);
    // u 0..1 vom Auf- zum Untergang, 1..2 die Nacht bis zum naechsten Aufgang
    const auto live_u = []() {
        const time_t t = time(nullptr);
        struct tm    utc;
        gmtime_r(&t, &utc);
        if (utc.tm_year < 120) return 0.5f;   // Uhr noch nicht gestellt
        const wetter::Stand ws = wetter::stand();
        static const float kGrad = (float)M_PI / 180;
        const float breite = ws.ort ? ws.breite_100 / 100.0f : 52.9f, laenge = ws.ort ? ws.laenge_100 / 100.0f : 8.0f;
        const int   n      = utc.tm_yday + 1;
        const float bw     = 2 * (float)M_PI * (n - 81) / 364;
        const float zeitgl = 9.87f * sin(2 * bw) - 7.53f * cos(bw) - 1.5f * sin(bw);   // Minuten
        const float dekl   = -23.44f * kGrad * cos(2 * (float)M_PI * (n + 10) / 365);
        const float phi    = breite * kGrad;
        float       cw     = (sin(-0.83f * kGrad) - sin(phi) * sin(dekl)) / (cos(phi) * cos(dekl));
        cw = cw < -1 ? -1 : (cw > 1 ? 1 : cw);
        const float tag    = 2 * acos(cw) / kGrad / 15;   // Stunden zwischen Auf- und Untergang
        if (tag < 0.05f) return 1.5f;                     // Polarnacht
        if (tag > 23.95f) return 0.5f;                    // Mitternachtssonne
        const float mittag = 12 - laenge / 15 - zeitgl / 60;   // UTC
        const float s      = utc.tm_hour + utc.tm_min / 60.0f + utc.tm_sec / 3600.0f;
        const float seit   = fmod(s - (mittag - tag / 2) + 48, 24.0f);
        return seit < tag ? seit / tag : 1 + (seit - tag) / (24 - tag);
    };
    // --- Uhr, Datum und Wetterwerte ------------------------------------------
    // Fest am Display, nicht mitgekruemmt: eine schwarze Maske fuer die Schrift
    // und eine weisse fuer einen schmalen Rand darum. Sie liegen vor allen
    // Ebenen; nur die Wolken mit Vordergrund-Merker (Ton | 0x80) ziehen davor.
    // Nur die Zeilen, in denen Uhr und Pille stehen, im PSRAM
    static const int kAufY0 = 40, kAufZeilen = 150, kAufEnde = kAufY0 + kAufZeilen;
    uint8_t *const   auf_z_speicher = (uint8_t *)heap_caps_calloc(kAufZeilen, BB, MALLOC_CAP_SPIRAM);
    uint8_t *const   auf_h_speicher = (uint8_t *)heap_caps_calloc(kAufZeilen, BB, MALLOC_CAP_SPIRAM);
    uint8_t *const   auf_z = auf_z_speicher - kAufY0 * BB;   // Zeile y liegt bei auf_z[y * BB]
    uint8_t *const   auf_h = auf_h_speicher - kAufY0 * BB;
    // Glaspille: Geometrie und je Pixel ein Versatz, von wo das Glas die Szene
    // dahinter holt. Am Rand biegt es stark nach aussen wie eine Linsenkante,
    // in der Mitte vergroessert es leicht. Die Szene dazu wird beim Rastern
    // mit Rand eingefangen und die Pille danach neu gerastert.
    static const int kGlasB = 400, kGlasH = 72, kGlasRand = 16;
    int8_t  *glas_dx  = (int8_t *)heap_caps_calloc(1, kGlasB * kGlasH, MALLOC_CAP_SPIRAM);
    int8_t  *glas_dy  = (int8_t *)heap_caps_calloc(1, kGlasB * kGlasH, MALLOC_CAP_SPIRAM);
    uint8_t *glas_art = (uint8_t *)heap_caps_calloc(1, kGlasB * kGlasH, MALLOC_CAP_SPIRAM);   // 1 Glas, 2 Glanz oben, 3 Rand unten
    uint8_t *glas_ton = (uint8_t *)heap_caps_calloc(1, 400 * (kGlasH + 2 * kGlasRand), MALLOC_CAP_SPIRAM);
    bool     glas_da  = false;
    int      glas_x0 = 0, glas_y0 = 0, glas_x1 = -1, glas_y1 = -1;       // Pille
    int      fang_x0 = 0, fang_y0 = 0, fang_x1 = -1, fang_y1 = -1;       // eingefangene Szene
    int  auf_oben = H, auf_unten = -1;
    char auf_alt[96] = "";
    // BOOT kurz (LIVE): untere Zeile der Pille zeigt CPU-Last, internen RAM und
    // die Chiptemperatur statt des Wetters
    bool  sys_zeigen = false;
    int   sys_cpu = -1, sys_ram = -1;
    float sys_temp = -1000;
    temperature_sensor_handle_t temp_fuehler = nullptr;
    {
        temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&tc, &temp_fuehler) != ESP_OK || temperature_sensor_enable(temp_fuehler) != ESP_OK)
            temp_fuehler = nullptr;
    }
    const auto stempel = [&](float px, float py, float r, float rand) {
        const int R = (int)(r + rand + 1), cx = (int)px, cy = (int)py;
        for (int dy = -R; dy <= R; dy++) {
            const int y = cy + dy;
            if (y < kAufY0 || y >= kAufEnde) continue;
            for (int dx = -R; dx <= R; dx++) {
                const int x = cx + dx;
                if (x < 0 || x >= B) continue;
                const float ex = x + 0.5f - px, ey = y + 0.5f - py, d2 = ex * ex + ey * ey;
                if (d2 > (r + rand) * (r + rand)) continue;
                const uint8_t bit = 0x80 >> (x & 7);
                auf_h[y * BB + (x >> 3)] |= bit;
                if (d2 <= r * r) auf_z[y * BB + (x >> 3)] |= bit;
                if (y < auf_oben) auf_oben = y;
                if (y > auf_unten) auf_unten = y;
            }
        }
    };
    // Ziffern aus Strichen auf einem Raster 44 x 80: Linien und Ellipsenboegen
    // (Winkel in Grad, gegen den Uhrzeiger, 0 rechts), Punkte fuer den Doppelpunkt
    struct Zug { uint8_t z; char art; float a, b, c, d, e, f; };
    static const Zug kZuege[] = {
        { 0, 'A', 22, 40, 18, 36, 0, 360 },
        { 1, 'L', 10, 16, 24, 4 }, { 1, 'L', 24, 4, 24, 76 },
        { 2, 'A', 22, 22, 18, 18, 160, -35 }, { 2, 'L', 36.7f, 32.3f, 4, 76 }, { 2, 'L', 4, 76, 40, 76 },
        { 3, 'A', 22, 21, 17, 17, 155, -90 }, { 3, 'A', 22, 57, 19, 19, 90, -155 },
        { 4, 'L', 30, 76, 30, 4 }, { 4, 'L', 30, 4, 4, 54 }, { 4, 'L', 4, 54, 42, 54 },
        { 5, 'L', 38, 4, 9, 4 }, { 5, 'L', 9, 4, 6, 44 }, { 5, 'A', 22, 55, 19, 19, 150, -150 },
        { 6, 'A', 22, 56, 19, 20, 0, 360 }, { 6, 'L', 5, 48, 28, 4 },
        { 7, 'L', 4, 4, 40, 4 }, { 7, 'L', 40, 4, 16, 76 },
        { 8, 'A', 22, 21, 16, 17, 0, 360 }, { 8, 'A', 22, 58, 19, 18, 0, 360 },
        { 9, 'A', 22, 24, 19, 20, 0, 360 }, { 9, 'L', 39, 32, 16, 76 },
        { 10, 'P', 8, 30 }, { 10, 'P', 8, 56 },
        { 11, 'L', 8, 40, 36, 40 },
        // Symbole wie bei iOS, Raster in Pixeln: Thermometer 10x16, Tropfen 10x15, Wind 17x18
        { 20, 'L', 2.5f, 3, 2.5f, 10 }, { 20, 'L', 7.5f, 3, 7.5f, 10 }, { 20, 'A', 5, 3, 2.5f, 2.5f, 0, 180 },
        { 20, 'P', 5, 12, 3.9f }, { 20, 'L', 5, 6, 5, 11 },
        { 21, 'P', 5, 10.5f, 4.5f }, { 21, 'P', 5, 7.2f, 3.3f }, { 21, 'P', 5, 4.8f, 2.2f }, { 21, 'P', 5, 2.8f, 1.3f },
        { 21, 'P', 5, 1.5f, 0.8f },
        { 22, 'L', 0, 5, 10, 5 }, { 22, 'A', 10, 2.5f, 2.5f, 2.5f, -90, 180 },
        { 22, 'L', 0, 9, 14, 9 }, { 22, 'A', 14, 6.5f, 2.5f, 2.5f, -90, 180 },
        { 22, 'L', 0, 13, 8, 13 }, { 22, 'A', 8, 15.5f, 2.5f, 2.5f, 90, -180 },
        { 23, 'L', 3.5f, 3.5f, 12.5f, 3.5f }, { 23, 'L', 12.5f, 3.5f, 12.5f, 12.5f }, { 23, 'L', 12.5f, 12.5f, 3.5f, 12.5f },
        { 23, 'L', 3.5f, 12.5f, 3.5f, 3.5f }, { 23, 'P', 8, 8, 1.3f },
        { 23, 'L', 6.5f, 0.5f, 6.5f, 3 }, { 23, 'L', 9.5f, 0.5f, 9.5f, 3 }, { 23, 'L', 6.5f, 13, 6.5f, 15.5f }, { 23, 'L', 9.5f, 13, 9.5f, 15.5f },
        { 23, 'L', 0.5f, 6.5f, 3, 6.5f }, { 23, 'L', 0.5f, 9.5f, 3, 9.5f }, { 23, 'L', 13, 6.5f, 15.5f, 6.5f }, { 23, 'L', 13, 9.5f, 15.5f, 9.5f },
        { 24, 'L', 0.5f, 1.5f, 17.5f, 1.5f }, { 24, 'L', 17.5f, 1.5f, 17.5f, 9.5f }, { 24, 'L', 17.5f, 9.5f, 0.5f, 9.5f },
        { 24, 'L', 0.5f, 9.5f, 0.5f, 1.5f }, { 24, 'L', 4, 4.5f, 4, 6.5f }, { 24, 'L', 9, 4.5f, 9, 6.5f }, { 24, 'L', 14, 4.5f, 14, 6.5f },
        { 24, 'L', 3.5f, 10, 3.5f, 12.5f }, { 24, 'L', 7.5f, 10, 7.5f, 12.5f }, { 24, 'L', 11.5f, 10, 11.5f, 12.5f }, { 24, 'L', 15, 10, 15, 12.5f },
    };
    const auto zeichen = [&](int z, float ox, float oy, float k, float kStrich = 4.5f, float kRand = 2.5f) {
        for (const Zug &g : kZuege) {
            if (g.z != z) continue;
            if (g.art == 'P') { stempel(ox + g.a * k, oy + g.b * k, g.c > 0 ? g.c : 5.0f, kRand); continue; }
            if (g.art == 'L') {
                const float lx = (g.c - g.a) * k, ly = (g.d - g.b) * k;
                const int   n  = (int)sqrt(lx * lx + ly * ly) + 2;
                for (int i = 0; i < n; i++)
                    stempel(ox + g.a * k + lx * i / (n - 1), oy + g.b * k + ly * i / (n - 1), kStrich, kRand);
                continue;
            }
            const float span = g.f - g.e;
            const int   n    = (int)(fabs(span) * (float)M_PI / 180 * (g.c > g.d ? g.c : g.d) * k) + 2;
            for (int i = 0; i < n; i++) {
                const float w = (g.e + span * i / (n - 1)) * (float)M_PI / 180;
                stempel(ox + (g.a + g.c * cos(w)) * k, oy + (g.b - g.d * sin(w)) * k, kStrich, kRand);
            }
        }
    };
    // Zeile im 5x7-Font, mittig, Groesse 2
    const auto schrift = [&](const char *s, int y) {
        static const int kS = 2;
        int x = B / 2 - ((int)strlen(s) * kFontAdvance * kS - kS) / 2;
        for (const char *p = s; *p; p++, x += kFontAdvance * kS) {
            unsigned ch = (unsigned char)*p;
            if (ch < 0x20 || ch > 0x7f) ch = '?';
            const uint8_t *g = &kFont5x7[(ch - 0x20) * 5];
            for (int gx = 0; gx < 5; gx++)
                for (int gy = 0; gy < 7; gy++)
                    if ((g[gx] >> gy) & 1)
                        for (int sy = 0; sy < kS; sy++)
                            for (int sx = 0; sx < kS; sx++) stempel(x + gx * kS + sx + 0.5f, y + gy * kS + sy + 0.5f, 0.5f, 2.0f);
        }
    };
    // Zeile in der Schrift wie bei iOS; gibt die Breite zurueck, setzt nur mit malen Pixel
    const auto ios_text = [&](const Schrift &sf, const char *s, int x, int y, bool malen) {
        int stift = 0;
        for (const char *p = s;;) {
            const uint16_t z = schrift_naechstes(p);
            if (z == 0) break;
            const Glyphe *g = schrift_glyphe(sf, z);
            if (g == nullptr) g = schrift_glyphe(sf, '?');
            if (malen) {
                const int zb = (g->breite + 7) / 8;
                for (int gy = 0; gy < sf.hoehe; gy++)
                    for (int gx = 0; gx < g->breite; gx++) {
                        if (!((sf.bits[g->ofs + gy * zb + gx / 8] << (gx % 8)) & 0x80)) continue;
                        const int px = x + stift + g->links + gx, py = y + gy;
                        if (px < 0 || px >= B || py < kAufY0 || py >= kAufEnde) continue;
                        auf_z[py * BB + (px >> 3)] |= 0x80 >> (px & 7);
                        auf_h[py * BB + (px >> 3)] |= 0x80 >> (px & 7);
                    }
            }
            stift += g->vorschub;
        }
        return stift;
    };

    // Einmal je Minute oder wenn sich ein Wert aendert
    const auto auflage = [&]() {
        static const char *const kTage[7] = { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" };
        static const char *const kMonate[12] = { "Januar", "Februar", "M\xc3\xa4rz", "April", "Mai", "Juni", "Juli",
                                                 "August", "September", "Oktober", "November", "Dezember" };
        const time_t tt = time(nullptr);
        struct tm    lt;
        localtime_r(&tt, &lt);
        const bool          zeit_ok = lt.tm_year + 1900 >= 2025;
        const wetter::Stand ws      = wetter::stand();
        char datum[40] = "", werte[40] = "", neu[96];
        if (zeit_ok) snprintf(datum, sizeof datum, "%s, %d. %s", kTage[lt.tm_wday], lt.tm_mday, kMonate[lt.tm_mon]);
        if (sys_zeigen) {
            if (sys_cpu >= 0 && sys_temp > -100)
                snprintf(werte, sizeof werte, "%d %%|%d %%|%d\xc2\xb0" "C", sys_cpu, sys_ram, (int)lround(sys_temp));
            else if (sys_cpu >= 0)
                snprintf(werte, sizeof werte, "%d %%|%d %%", sys_cpu, sys_ram);
        } else if (ws.gueltig)
            snprintf(werte, sizeof werte, "%d\xc2\xb0|%d %%|%d km/h", (int)lround(ws.temp_c100 / 100.0),
                     (int)lround(ws.feuchte_100 / 100.0), (int)lround(ws.wind_kmh10 / 10.0));
        snprintf(neu, sizeof neu, "%d:%02d|%s|%s", zeit_ok ? lt.tm_hour : -1, lt.tm_min, datum, werte);
        if (strcmp(neu, auf_alt) == 0) return;
        strcpy(auf_alt, neu);
        memset(auf_z_speicher, 0, kAufZeilen * BB);
        memset(auf_h_speicher, 0, kAufZeilen * BB);
        auf_oben = H; auf_unten = -1;
        // HH:MM, Ziffern 44 breit, 12 Abstand, Doppelpunkt 16; klein und kraeftig
        static const float k = 0.62f, kOben = 60;
        const float breite = (44 * 4 + 12 * 4 + 16) * k;
        float       x      = B / 2 - breite / 2;
        const int   zif[4] = { zeit_ok ? lt.tm_hour / 10 : 11, zeit_ok ? lt.tm_hour % 10 : 11,
                               zeit_ok ? lt.tm_min / 10 : 11, zeit_ok ? lt.tm_min % 10 : 11 };
        for (int i = 0; i < 4; i++) {
            zeichen(zif[i], x, kOben, k);
            x += (44 + 12) * k;
            if (i == 1) { zeichen(10, x, kOben, k); x += (16 + 12) * k; }
        }
        // Datum und Werte zusammen in einer weissen Pille wie ein iOS-Widget:
        // oben das Datum fett, darunter Symbol und Wert je Messgroesse
        glas_da = false;
        if (datum[0] || werte[0]) {
            static const int kWetterS[3] = { 20, 21, 22 }, kWetterB[3] = { 10, 10, 17 }, kWetterH[3] = { 16, 15, 16 };
            static const int kSystemS[3] = { 23, 24, 20 }, kSystemB[3] = { 16, 18, 10 }, kSystemH[3] = { 16, 13, 16 };
            const int *const kSymbol = sys_zeigen ? kSystemS : kWetterS;
            const int *const kSymbolB = sys_zeigen ? kSystemB : kWetterB;
            const int *const kSymbolH = sys_zeigen ? kSystemH : kWetterH;
            char teil[3][16] = { "", "", "" };
            int  teile = 0;
            if (werte[0]) {
                const char *p = werte;
                teile = 1;
                for (const char *q = werte; *q; q++) teile += *q == '|';
                if (teile > 3) teile = 3;
                for (int i = 0; i < teile; i++) {
                    const char *e = strchr(p, '|');
                    const int   n = e ? (int)(e - p) : (int)strlen(p);
                    snprintf(teil[i], sizeof teil[i], "%.*s", n, p);
                    p = e ? e + 1 : p + n;
                }
            }
            const int bd = datum[0] ? ios_text(kSchriftFett, datum, 0, 0, false) : 0;
            int       bw = 0;
            if (werte[0])
                for (int i = 0; i < teile; i++) bw += (i ? 14 : 0) + kSymbolB[i] + 4 + ios_text(kSchriftNormal, teil[i], 0, 0, false);
            const int   lb  = bd > bw ? bd : bw;
            const float py0 = 124;
            const float py1 = py0 + 7 + (datum[0] ? kSchriftFett.hoehe : 0) + (datum[0] && werte[0] ? 2 : 0)
                            + (werte[0] ? kSchriftNormal.hoehe + 2 : 0) + 5;
            const float pr  = (py1 - py0) / 2, pm = (py0 + py1) / 2;
            const float px0 = B / 2 - lb / 2 - 6, px1 = B / 2 + lb / 2 + 6;
            glas_x0 = (int)(px0 - pr) - 1; glas_x1 = (int)(px1 + pr) + 1;
            glas_y0 = (int)py0 - 1;        glas_y1 = (int)py1 + 1;
            if (glas_x0 < 0) glas_x0 = 0;
            if (glas_x1 >= B) glas_x1 = B - 1;
            if (glas_y1 - glas_y0 >= kGlasH) glas_y1 = glas_y0 + kGlasH - 1;
            fang_x0 = glas_x0 - kGlasRand < 0 ? 0 : glas_x0 - kGlasRand;
            fang_x1 = glas_x1 + kGlasRand >= B ? B - 1 : glas_x1 + kGlasRand;
            fang_y0 = glas_y0 - kGlasRand; fang_y1 = glas_y1 + kGlasRand;
            const int gw = glas_x1 - glas_x0 + 1;
            static const float kKante = 11, kBiegung = 13, kLupe = 0.07f;
            for (int y = glas_y0; y <= glas_y1; y++)
                for (int xx = glas_x0; xx <= glas_x1; xx++) {
                    const int gi = (y - glas_y0) * gw + (xx - glas_x0);
                    glas_art[gi] = 0;
                    const float fx = xx + 0.5f, fy = y + 0.5f - pm;
                    const float ex = fx < px0 ? fx - px0 : (fx > px1 ? fx - px1 : 0);
                    const float len = sqrt(ex * ex + fy * fy), rand = len - pr;
                    if (rand > 0) continue;
                    if (y < auf_oben) auf_oben = y;
                    if (y > auf_unten) auf_unten = y;
                    // Nach aussen gerichtete Normale; je naeher am Rand, desto staerker gebogen
                    const float tiefe = -rand, f = tiefe < kKante ? (1 - tiefe / kKante) : 0;
                    const float nx = len > 0 ? ex / len : 0, ny = len > 0 ? fy / len : 0;
                    float dx = nx * kBiegung * f * f - (fx - B / 2) * kLupe;
                    float dy = ny * kBiegung * f * f - fy * kLupe;
                    glas_dx[gi]  = (int8_t)lround(dx);
                    glas_dy[gi]  = (int8_t)lround(dy);
                    const float fb = tiefe < pr * 0.75f ? 1 - tiefe / (pr * 0.75f) : 0;   // Verlauf ueber drei Viertel
                    glas_art[gi] = (uint8_t)(1 + (int)(fb * 15 + 0.5f));                   // 1 innen sauber .. 16 am Rand
                }
            glas_da = true;
            int ty = (int)py0 + 7;
            if (datum[0]) {
                ios_text(kSchriftFett, datum, B / 2 - bd / 2, ty, true);
                ty += kSchriftFett.hoehe + 2;
            }
            if (werte[0]) {
                int x = B / 2 - bw / 2;
                for (int i = 0; i < teile; i++) {
                    if (i) x += 14;
                    // Symbol unten knapp unter der Grundlinie
                    zeichen(kSymbol[i], (float)x, (float)(ty + kSchriftNormal.basis + 1 - kSymbolH[i]), 1.0f, 1.0f, 0.0f);
                    x += kSymbolB[i] + 4;
                    x += ios_text(kSchriftNormal, teil[i], x, ty, true);
                }
            }
        }
        if (auf_oben < 0) auf_oben = 0;
        if (auf_unten >= H) auf_unten = H - 1;
    };

    struct Werte { float ho, hu, e[kEbenen], wt, wo, rg, sn, gw, wi, sx, sy, sr, si, ha, sichel, sterne; };
    int32_t       boot_zuletzt = boot_kurz;
    int           lage_nr      = 0;
    float         u            = 0.25f;        // Vormittag
    Lage          jetzt_l      = live_lage(), von = jetzt_l;
    bool          test         = false;        // Testlauf durch alle Lagen
    char          etikett[48]  = "";
    int64_t       etikett_bis  = 0;
    static const char *const kLageName[kAnzWetter] = { "Sonnig", "Heiter", "Bew\xc3\xb6lkt", "Nebel", "Niesel",
                                                       "Regen", "Gewitter", "Schnee" };
    int           blende       = kBlende;      // Takte seit dem Wechsel
    float         schneedecke  = 0;            // waechst beim Schneien, taut danach
    float         versatz[kEbenen] = {}, wolken_versatz = 0;
    bool          ort_gebaut   = kBergTest >= 0;
    int           blitz = 0, blitz_warten = 80;
    int16_t       blitz_x[10], blitz_y[10];

    int64_t takt = esp_timer_get_time(), ms_log = takt, rechnen_us = 0;
    int64_t teil_us[5] = {};   // Messung: Vorbereitung, Zeilen, Glas, Rest, Uebertragung
    int32_t takte = 0, zeit = 0;
    // Unter der Teilung zeichnet der Helfer auf Kern 1 mit eigenem Zeilenring;
    // die Teilung wandert dorthin, wo beide gleich lange brauchen
    helfer_los    = xSemaphoreCreateBinary();
    helfer_fertig = xSemaphoreCreateBinary();
    const bool helfer_da = xTaskCreatePinnedToCoreWithCaps(helfer_task, "zeilen", 6144, nullptr, 3, nullptr, 1,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    uint8_t (*const ring2)[400] = (uint8_t (*)[400])heap_caps_calloc(kBogen + 1, 400, MALLOC_CAP_SPIRAM);
    int     teilung = 150;
    int64_t teil_eigen_us = 0, teil_helfer_us = 0;

    while (true) {
        const int64_t jetzt = esp_timer_get_time();
        if (jetzt - takt < kDunstTaktUs) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        const int64_t schritt = jetzt - takt;
        takt = jetzt;
        zeit++;

        if (!ort_gebaut) {
            const wetter::Gelaende g = wetter::gelaende();
            if (g.gueltig) { bauen(g.bergigkeit / 1000.0f, g.keim); ort_gebaut = true; }
        }

        // Wechsel zwischen LIVE und Testlauf; oben links steht kurz, was gilt
        if (test != dunst_test) {
            test = dunst_test;
            von  = jetzt_l;
            if (test) { lage_nr = 0; u = 0.25f; blende = 0; }
            snprintf(etikett, sizeof etikett, test ? "Test: %s, Tag" : "Live", kLageName[lage_nr]);
            etikett_bis = jetzt + 2500000;
            ESP_LOGI(TAG, "Dunst: %s.", test ? "Testlauf" : "LIVE");
        }
        // Tag und Nacht laufen durch; nach einer Nacht oder mit BOOT kommt die naechste Lage
        const bool taste = boot_kurz != boot_zuletzt;
        boot_zuletzt = boot_kurz;
        if (test) {
            const bool nacht_vorher = u >= 1;
            u += (float)schritt / kHaltenUs;
            bool weiter = taste;
            if (u >= 2) { u -= 2; weiter = true; }
            if (weiter) {
                lage_nr = (lage_nr + 1) % kAnzWetter;
                von     = jetzt_l;
                blende  = 0;
                if (taste) u = 0.25f;
                ESP_LOGI(TAG, "Dunst: %s.", kWetterlagen[lage_nr].code);
            }
            if (weiter || nacht_vorher != (u >= 1)) {
                snprintf(etikett, sizeof etikett, "Test: %s, %s", kLageName[lage_nr], u >= 1 ? "Nacht" : "Tag");
                etikett_bis = jetzt + 2500000;
            }
        } else {
            if (taste) sys_zeigen = !sys_zeigen;
            u   = live_u();
            von = jetzt_l;   // Wetter zieht sanft nach, statt zu springen
        }
        const Lage zl = test ? lage(lage_nr) : live_lage();
        float m = blende < kBlende ? (float)blende / kBlende : 1.0f;
        m = m * m * (3 - 2 * m);
        if (blende < kBlende) blende++;
        if (!test) m = 0.02f;
        const auto mix = [m](float a, float b) { return a + (b - a) * m; };
        Lage L;
        L.wt = mix(von.wt, zl.wt); L.wo = mix(von.wo, zl.wo); L.rg = mix(von.rg, zl.rg); L.sn = mix(von.sn, zl.sn);
        L.gw = mix(von.gw, zl.gw); L.wi = mix(von.wi, zl.wi); L.du = mix(von.du, zl.du); L.hi = mix(von.hi, zl.hi);
        jetzt_l = L;
        schneedecke += L.sn > 10 ? 0.006f : -0.01f;
        schneedecke = schneedecke < 0 ? 0 : (schneedecke > 1 ? 1 : schneedecke);

        // Sonne am Tag, Mond in der Nacht: flache Ellipse von links nach rechts,
        // hinter den Bergen auf- und untergehend, tief am Horizont groesser.
        // Die Toene gehen von Daemmerung zu Tag oder Nacht, je hoeher der Bogen
        const bool  tag  = u < 1;
        const float bog  = tag ? u : u - 1, h = sin((float)M_PI * bog);
        float       hoch = h / (tag ? 0.45f : 0.3f);
        hoch = hoch > 1 ? 1 : hoch;
        hoch = hoch * hoch * (3 - 2 * hoch);
        // Ebenen, Wolken, Himmel oben und am Horizont; der Himmel bleibt hell, damit die Formen klar stehen
        static const float kDaemmerung[kEbenen + 3] = { 8, 12, 16, 5, 0, 3 };
        static const float kTagTon[kEbenen + 3]     = { 5, 10, 16, 3, 0, 0 };
        static const float kNachtTon[kEbenen + 3]   = { 12, 14, 16, 8, 3, 1 };
        static const float kAufhellen[kEbenen]      = { 0.65f, 0.4f, 0 };   // Dunst hellt hinten staerker auf
        const float *ziel_ton = tag ? kTagTon : kNachtTon;
        Werte w;
        for (int k = 0; k < kEbenen; k++)
            w.e[k] = (kDaemmerung[k] + (ziel_ton[k] - kDaemmerung[k]) * hoch) * (1 - kAufhellen[k] * L.du);
        const float wt = kDaemmerung[kEbenen] + (ziel_ton[kEbenen] - kDaemmerung[kEbenen]) * hoch + L.wt;
        w.wt = wt < 1 ? 1 : (wt > 12 ? 12 : wt);   // auch im Gewitter nie ganz schwarz
        for (int i = 0; i < 2; i++) {
            float hi = kDaemmerung[kEbenen + 1 + i] + (ziel_ton[kEbenen + 1 + i] - kDaemmerung[kEbenen + 1 + i]) * hoch + L.hi;
            hi = hi > 5 ? 5 : hi;
            (i == 0 ? w.ho : w.hu) = hi;
        }
        if (w.wt < w.hu + 2) w.wt = w.hu + 2;   // Wolken heben sich immer vom Himmel ab
        w.wo = L.wo; w.rg = L.rg; w.sn = L.sn; w.gw = L.gw; w.wi = L.wi;
        w.sx = 200 - 185 * cos((float)M_PI * bog);
        w.sy = 232 - 178 * h;
        w.sr = tag ? 38 - 22 * h : 20 - 8 * h;
        w.si = tag ? w.sr : 0;
        w.ha = tag ? (10 + 4 * h) * (1 + L.du) : 9;
        w.sichel = tag ? 0 : 1;
        w.sterne = tag || L.wo >= 60 || L.du >= 0.5f ? 0 : h;   // je hoeher der Mond, desto mehr Sterne

        for (int k = 0; k < kEbenen; k++) {
            versatz[k] += ebenen[k].tempo;
            if (versatz[k] >= kWeltB) versatz[k] -= kWeltB;
        }
        wolken_versatz += 0.03f + w.wi * 0.012f;   // windstill kaum, bei 40 km/h gut ein halbes Pixel je Takt
        if (wolken_versatz >= kWeltB) wolken_versatz -= kWeltB;

        {
            static int64_t sys_zuletzt = 0;
            if (jetzt - sys_zuletzt > 2000000) {
                static TaskStatus_t *st = (TaskStatus_t *)heap_caps_calloc(32, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
                static uint32_t      leer_vor[2] = {};
                uint32_t             gesamt = 0, leer[2] = {};
                const UBaseType_t    n = uxTaskGetSystemState(st, 32, &gesamt);
                for (UBaseType_t i = 0; i < n; i++) {
                    if (strcmp(st[i].pcTaskName, "IDLE0") == 0) leer[0] = st[i].ulRunTimeCounter;
                    if (strcmp(st[i].pcTaskName, "IDLE1") == 0) leer[1] = st[i].ulRunTimeCounter;
                }
                if (sys_zuletzt > 0) {
                    const int64_t dt   = (jetzt - sys_zuletzt) * 2;   // zwei Kerne, Laufzeit in Mikrosekunden
                    const int64_t frei = (int64_t)(uint32_t)(leer[0] - leer_vor[0]) + (uint32_t)(leer[1] - leer_vor[1]);
                    const int     last = 100 - (int)(frei * 100 / dt);
                    sys_cpu = last < 0 ? 0 : (last > 100 ? 100 : last);
                }
                leer_vor[0] = leer[0]; leer_vor[1] = leer[1];
                const size_t ganz = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                sys_ram = ganz ? 100 - (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) * 100 / ganz) : 0;
                float grad;
                if (temp_fuehler && temperature_sensor_get_celsius(temp_fuehler, &grad) == ESP_OK) sys_temp = grad;
                sys_zuletzt = jetzt;
            }
        }
        auflage();
        const int64_t r0 = esp_timer_get_time();
        int ton[kEbenen], off[kEbenen];
        for (int k = 0; k < kEbenen; k++) { ton[k] = (int)(w.e[k] + 0.5f); off[k] = (int)versatz[k]; }
        const int   sx = (int)w.sx, sy = (int)w.sy, sr = (int)w.sr, si = (int)(w.si + 0.5f);
        const int   hl = (int)(w.sr * w.ha / 10.0f), R = sr + hl < kWurzelMax - 1 ? sr + hl : kWurzelMax - 1;
        const float sonne_schmal = zeile_s[sy < 0 ? 0 : (sy >= H ? H - 1 : sy)] / 65536.0f;   // gleicht das Breitziehen aus
        const float menge = w.wo / 100.0f;
        const uint8_t wolke_ton = (uint8_t)(w.wt + 0.5f), wolke_kappe = (uint8_t)(w.wt + 1.5f);
        for (int d = 0; d <= R; d++) {
            const float f = d <= sr ? 0 : 1.0f - (float)(d - sr) / (hl > 0 ? hl : 1);
            halo_ton[d] = (uint8_t)(f > 0 ? 8.0f * f * f + 0.5f : 0);
        }

        // Sichtbare Wolkenbuckel dieses Takts; eine Wolke waechst mit der Bedeckung aus dem Nichts
        int nb = 0, w_oben = H, w_unten = -1;
        for (int wn = 0; wn < kWolken; wn++) {
            const Wolke &c = wolken[wn];
            float s = (menge * 1.25f - c.schwelle) * 4.0f;
            if (s <= 0.05f) continue;
            if (s > 1) s = 1;
            const float g = s * (0.7f + 0.6f * menge);
            float px = c.x - wolken_versatz;
            while (px < -kWeltB / 2) px += kWeltB;
            while (px >= kWeltB / 2) px -= kWeltB;
            if (px + c.w * g < -30 || px - c.w * g > B + 30) continue;
            for (int i = 0; i < c.n; i++) {
                const int r = (int)(c.r[i] * g);
                if (r < 1) continue;
                Buckel &b = buckel[nb++];
                b.cx = (int16_t)(px + c.dx[i] * g); b.r = (int16_t)r; b.boden = c.y; b.cy = (int16_t)(c.y - r / 3); b.wolke = (int16_t)wn;
                if (b.cy - r < w_oben) w_oben = b.cy - r;
                if (b.boden > w_unten) w_unten = b.boden;
            }
        }

        const auto kippen = [&](int x, int y) {
            if (x < 0 || x >= B || y < 0 || y >= H) return;
            bild[y * BB + (x >> 3)] ^= 0x80 >> (x & 7);
        };
        const auto himmel = [&](int y) {
            const float t = w.ho + (w.hu - w.ho) * (y < 200 ? y / 200.0f : 1.0f);
            return (uint8_t)(t + 0.5f);
        };
        // Spanne einer Tonzeile mit Ton t fuellen, beschnitten
        const auto fuellen = [&](uint8_t *zt, int x0, int x1, uint8_t t) {
            if (x0 < 0) x0 = 0;
            if (x1 >= B) x1 = B - 1;
            if (x0 <= x1) memset(zt + x0, t, x1 - x0 + 1);
        };
        // Zeile y einer Maske mit Versatz off als Ton t in die Tonzeile legen
        const auto maske = [&](const DunstEbene &e, const uint8_t *bits, int y, int o, uint8_t t, uint8_t *zt) {
            if (y < e.oben || y > e.unten) return;
            const uint8_t *q  = bits + y * kWeltBytes;
            const int      j0 = o >> 3, sh = o & 7;
            for (int i = 0; i < BB; i++) {
                int j = j0 + i;
                if (j >= kWeltBytes) j -= kWeltBytes;
                int j2 = j + 1;
                if (j2 >= kWeltBytes) j2 -= kWeltBytes;
                const uint8_t mk = (uint8_t)((q[j] << sh) | (sh ? (q[j2] >> (8 - sh)) : 0));
                if (!mk) continue;
                if (mk == 0xff) { memset(zt + 8 * i, t, 8); continue; }
                for (int b = 0; b < 8; b++) if (mk & (0x80 >> b)) zt[8 * i + b] = t;
            }
        };

        // Wie unter Lichtverschmutzung: ganz oben viele und hellere Punkte, zum
        // Horizont hin werden sie kleiner und verschwinden nach und nach
        struct Sichtbar { int16_t x, y; uint8_t gross; };
        Sichtbar sterne_sicht_pos[40];
        int      sterne_sicht = 0;
        if (w.sterne > 0)
            for (int n = 0; n < 40; n++) {
                const auto &st   = sterne_pos[n];
                const float klar = w.sterne * (1.0f - st.y / 125.0f);
                if (klar <= ((n * 37) % 40) / 40.0f) continue;
                const int ex = st.x - sx, ey = st.y - sy;
                if (ex * ex + ey * ey <= (R + 6) * (R + 6)) continue;
                sterne_sicht_pos[sterne_sicht++] = { st.x, st.y, (uint8_t)(klar > 0.45f ? 1 : 0) };
            }

        int64_t teil = esp_timer_get_time();
        teil_us[0] += teil - r0;
        // Gezeichnet wird in Toenen im flachen Bild, Zeile fuer Zeile; die Bildzeile
        // y braucht die flachen Zeilen y - kBogen .. y, die in einem Ring liegen.
        // Erst beim Legen auf den Planeten wird gerastert, fest am Display, damit
        // die Kruemmung keine Muster in das Raster zieht
        // Zeilen [y_von, y_bis) mit dem Ring rg; die kBogen flachen Zeilen davor
        // werden nur fuer den Ring gerechnet
        const auto zeilen = [&](int y_von, int y_bis, uint8_t (*const rg)[400]) {
        for (int y = y_von - kBogen < 0 ? 0 : y_von - kBogen; y < y_bis; y++) {
            uint8_t  *zt = rg[y % (kBogen + 1)];
            const int dy = y - sy;

            memset(zt, himmel(y), B);

            // Sterne als kleine Pluszeichen
            // Sterne, je Takt vorbereitet (unten vor der Zeilenschleife)
            for (int n = 0; n < sterne_sicht; n++) {
                const Sichtbar &st = sterne_sicht_pos[n];
                if (y < st.y || y > st.y + st.gross) continue;
                zt[st.x] = 16;
                if (st.gross) zt[st.x + 1] = 16;
            }

            // Sonne: weisse Scheibe ohne Rand, nur der Schein zeichnet sie. Mond:
            // schwarze Sichel mit einem Schein in Sichelform. Abstand zur Sichel
            // ist der groessere aus Abstand zur Scheibe und Tiefe im Ausschnitt
            if (dy >= -R && dy <= R) {
                const int halb = (int)(sqrt((float)(R * R - dy * dy)) * sonne_schmal);
                for (int x = sx - halb; x <= sx + halb; x++) {
                    if (x < 0 || x >= B) continue;
                    const float fdx = (x - sx) / sonne_schmal, d = sqrt(fdx * fdx + (float)(dy * dy));
                    if (d >= R) continue;
                    float abstand = d - sr;
                    if (abstand <= 0) {
                        if (d <= sr) zt[x] = (uint8_t)(((w.sichel > 0.5f || d >= si) ? 16 : 0) | 0x40);   // Merker: Scheibe
                        continue;
                    }
                    if (w.sichel > 0.5f) {
                        // Mond: volle schwarze Scheibe, darum ein leichter Schein in
                        // zwei flachen Stufen statt eines Verlaufs, damit nichts flimmert
                        const uint8_t st = abstand < 1 ? 0 : (abstand < hl * 0.45f ? 5 : (abstand < hl ? 3 : 0));
                        if (st > zt[x]) zt[x] = st;
                        continue;
                    }
                    if (abstand < hl && halo_ton[sr + (int)abstand] > zt[x]) zt[x] = halo_ton[sr + (int)abstand];
                }
            }

            // Wolken: flach, jeder Buckel bekommt innen oben eine schmale, nur eine
            // Stufe dunklere Sichel; der Umriss bleibt ringsum im Grundton. Buckel
            // fuer Buckel, damit ein vorderer die Sichel des hinteren verdeckt
            if (y >= w_oben && y <= w_unten) {
                const auto spanne = [](int cx, int cy, int r, int y, int *x0, int *x1) {
                    const int d = y - cy;
                    if (d < -r || d > r) return false;
                    const int halb = (int)sqrt((float)(r * r - d * d));
                    *x0 = cx - halb; *x1 = cx + halb;
                    return true;
                };
                for (int vorn = 0; vorn < 2; vorn++)
                for (int i = 0; i < nb; i++) {
                    const Buckel &bk = buckel[i];
                    if (y > bk.boden || (bk.wolke % 4 == 3) != (vorn == 1)) continue;
                    const uint8_t vf = vorn ? 0x80 : 0;   // vor der Uhr
                    int a0, a1, s0, s1, k0, k1;
                    if (!spanne(bk.cx, bk.cy, bk.r, y, &a0, &a1)) continue;
                    fuellen(zt, a0, a1, (uint8_t)(wolke_ton | vf));
                    const int ri = bk.r * 3 / 4;
                    if (ri < 3 || !spanne(bk.cx, bk.cy, ri, y, &s0, &s1)) continue;
                    fuellen(zt, s0, s1, (uint8_t)(wolke_kappe | vf));
                    if (spanne(bk.cx, bk.cy + bk.r / 4, ri, y, &k0, &k1))
                        fuellen(zt, k0 > s0 ? k0 : s0, k1 < s1 ? k1 : s1, (uint8_t)(wolke_ton | vf));
                }
                // Wo eine Wolke die Mondscheibe verdeckt, scheint er gut ein Drittel durch
                if (w.sichel > 0.5f && dy >= -sr && dy <= sr) {
                    const int halb = (int)(sqrt((float)(sr * sr - dy * dy)) * sonne_schmal);
                    for (int x = sx - halb < 0 ? 0 : sx - halb; x <= sx + halb && x < B; x++) {
                        if (zt[x] & 0x40) continue;
                        const int ct = zt[x] & 0x1f;
                        zt[x] = (uint8_t)((ct + ((16 - ct) * 35 + 50) / 100) | (zt[x] & 0x80));
                    }
                }
            }

            // Ebenen, darauf der Schnee in Weiss
            for (int k = 0; k < kEbenen; k++) {
                maske(ebenen[k], ebenen[k].bits, y, off[k], (uint8_t)(ton[k] | 0x20), zt);   // 0x20: festes Objekt
                if (schneedecke > 0.15f) maske(ebenen[k], ebenen[k].schnee[schneedecke > 0.6f], y, off[k], 0x20, zt);
                if (k == 1) maske(ebenen[1], turm, y, off[1], (uint8_t)((ton[1] + 4 > 16 ? 16 : ton[1] + 4) | 0x20), zt);
            }

            if (y < y_von) continue;
            // Bildzeile y: jede Spalte sinkt um bogen[x], weiter oben wird breiter gezogen
            const uint8_t *bay = kBayer[y & 3];
            const int32_t  s   = zeile_s[y];
            int32_t        fx  = (B / 2 << 16) - (B / 2) * s + (s >> 1);
            uint8_t       *z   = bild + y * BB;
            const uint8_t  oben = himmel(0);
            const bool     ueber = y >= auf_oben && y <= auf_unten;
            const uint8_t *uz = ueber ? auf_z + y * BB : nullptr, *uh = ueber ? auf_h + y * BB : nullptr;
            const bool     fang = glas_da && y >= fang_y0 && y <= fang_y1;
            uint8_t       *fz   = fang ? glas_ton + (y - fang_y0) * 400 : nullptr;
            for (int i = 0; i < BB; i++) {
                uint8_t byte = 0;
                for (int b = 0; b < 8; b++, fx += s) {
                    const int x  = 8 * i + b;
                    const int fy = y - bogen[x];
                    uint8_t t = fy < 0 ? oben : rg[fy % (kBogen + 1)][fx >> 16];
                    if (fang) fz[x] = t;
                    if (ueber && !(t & 0x80)) {
                        // Ueber Sonne oder Mond steht die Uhr umgekehrt, ohne weissen Rand
                        const uint8_t bit = 0x80 >> b;
                        if (uz[i] & bit) t = (t & 0x40) ? (uint8_t)(16 - (t & 0x1f)) : 16;
                        else if ((uh[i] & bit) && !(t & 0x40)) t = 0;
                    }
                    byte = (uint8_t)((byte << 1) | (bay[x & 3] < (t & 0x1f)));
                }
                z[i] = byte;
            }
        }
        };

        if (helfer_da) {
            int64_t    helfer_us = 0;
            const auto arbeit    = [&]() {
                const int64_t a = esp_timer_get_time();
                zeilen(teilung, H, ring2);
                helfer_us = esp_timer_get_time() - a;
            };
            helfer_ctx = (void *)&arbeit;
            helfer_fn  = [](void *p) { (*static_cast<decltype(arbeit) *>(p))(); };
            xSemaphoreGive(helfer_los);
            const int64_t a = esp_timer_get_time();
            zeilen(0, teilung, ring);
            const int64_t eigen_us = esp_timer_get_time() - a;
            xSemaphoreTake(helfer_fertig, portMAX_DELAY);
            // Je groesser der Unterschied, desto weiter springt die Teilung
            const int schritt = (int)((eigen_us > helfer_us ? eigen_us - helfer_us : helfer_us - eigen_us) / 400);
            if (eigen_us > helfer_us + 1500) teilung -= schritt < 20 ? schritt : 20;
            else if (helfer_us > eigen_us + 1500) teilung += schritt < 20 ? schritt : 20;
            teilung = teilung < 20 ? 20 : (teilung > 290 ? 290 : teilung);
            teil_eigen_us += eigen_us;
            teil_helfer_us += helfer_us;
        } else {
            zeilen(0, H, ring);
        }

        teil_us[1] += esp_timer_get_time() - teil;
        teil = esp_timer_get_time();
        // Glaspille: jedes Pixel holt seinen Ton ueber den Versatz aus der
        // eingefangenen Szene, ohne Raster: nur Dunkles scheint durch; oben ein
        // weisser Glanzrand, unten ein schmaler schwarzer Bogen. Wolken vor der Pille bleiben davor
        if (glas_da) {
            const int gw = glas_x1 - glas_x0 + 1;
            for (int y = glas_y0 < 0 ? 0 : glas_y0; y <= glas_y1 && y < H; y++) {
                const uint8_t *bay = kBayer[y & 3];
                for (int x = glas_x0; x <= glas_x1; x++) {
                    const int     gi  = (y - glas_y0) * gw + (x - glas_x0);
                    const uint8_t art = glas_art[gi];
                    if (!art) continue;
                    if (glas_ton[(y - fang_y0) * 400 + x] & 0x80) continue;
                    const uint8_t bit = 0x80 >> (x & 7);
                    int tt;
                    if (auf_z[y * BB + (x >> 3)] & bit) tt = 16;
                    else {
                        int qx = x + glas_dx[gi], qy = y + glas_dy[gi];
                        qx = qx < fang_x0 ? fang_x0 : (qx > fang_x1 ? fang_x1 : qx);
                        qy = qy < fang_y0 ? fang_y0 : (qy > fang_y1 ? fang_y1 : qy);
                        if (qy < 0) qy = 0;
                        if (qy >= H) qy = H - 1;
                        const uint8_t s = glas_ton[(qy - fang_y0) * 400 + qx];
                        // Glas ohne eigenes Raster: Himmel und Wolken verschwinden,
                        // feste Objekte scheinen gebrochen und halb so dunkel durch
                        tt = (s & 0x20) ? (s & 0x1f) / 2 : 0;
                    }
                    uint8_t &z = bild[y * BB + (x >> 3)];
                    z = bay[x & 3] < tt ? (uint8_t)(z | bit) : (uint8_t)(z & ~bit);
                }
            }
        }

        teil_us[2] += esp_timer_get_time() - teil;
        teil = esp_timer_get_time();
        // Punkt im flachen Bild auf den Planeten: Spalte zurueckrechnen, dann absinken
        const auto gebogen = [&](int px, int py, int *x, int *y) {
            const int zy = py < 0 ? 0 : (py >= H ? H - 1 : py);
            *x = B / 2 + (int)(((int64_t)(px - B / 2) << 16) / zeile_s[zy]);
            *y = py + bogen[*x < 0 ? 0 : (*x >= B ? B - 1 : *x)];
        };

        // Blaetter: aus sichtbaren Kronen loesen, pendelnd fallen, im Gras verschwinden
        if (zufall(w.wi > 20 ? 2 : 4) == 0 && kronen_n > 0) {
            const Krone &kr = kronen[zufall(kronen_n)];
            int bx = kr.x - off[kVorn];
            if (bx < 0) bx += kWeltB;
            if (bx < B + kr.r)
                for (auto &b : blaetter)
                    if (!b.aktiv) {
                        b = { (float)(kr.x - kr.r / 2 + zufall(kr.r)), (float)(kr.y + zufall(kr.r / 2)),
                              0.35f + zufall(35) / 100.0f, zufall(628) / 100.0f, true };
                        break;
                    }
        }
        for (auto &b : blaetter) {
            if (!b.aktiv) continue;
            const float pendel = sin(b.phase + zeit * 0.07f);
            b.y += b.vy;
            b.x += pendel * 0.8f - 0.15f - w.wi * 0.02f;
            int wx = (int)b.x % kWeltB;
            if (wx < 0) wx += kWeltB;
            if (b.y >= grund[kVorn][wx] + 2) { b.aktiv = false; continue; }
            int px = wx - off[kVorn];
            if (px < -kWeltB / 2) px += kWeltB;
            if (px > kWeltB / 2) px -= kWeltB;
            int x0, y0;
            gebogen(px, (int)b.y, &x0, &y0);
            // Blatt, das beim Pendeln kippt: breit nach links, hochkant, breit nach rechts
            static const char *const kBlatt[3][4] = {
                { "..###.", ".#####", "####..", ".#...." },
                { ".##...", ".###..", "..##..", "..#..." },
                { ".###..", "#####.", "..####", "....#." },
            };
            const int form = pendel < -0.4f ? 0 : (pendel > 0.4f ? 2 : 1);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 6; i++)
                    if (kBlatt[form][j][i] == '#') kippen(x0 + i, y0 + j);
        }

        {
            static const int kWolfB = 25, kWolfH = 14;
            static const char *const kRumpf[10] = {
                "...................#..#..", "...................####..", "..................#######",
                "..................######.", "..................####...", ".....#################...",
                "...#################.....", "..##################.....", ".##.################.....",
                "##...###############.....",
            };
            static const char *const kBeine[2][4] = {
                { "....##...........##......", "....#.#.........#.#......", "...#...#.......#...#.....", "..##...##.....##...##...." },
                { ".....##..........##......", ".....##..........##......", ".....##..........##......", ".....###.........###....." },
            };
            static const char *const kHeulen[kWolfH] = {
                "....................##...", "...................###...", "..................####...", ".................####....",
                ".................#.##....", "................#####....", "...............######....", "..............#######....",
                "............#########....", "..........###########....", "........#############....", "#.....###############....",
                "##..#################....", ".###################.##..",
            };
            const bool nacht = !tag;
            for (Wolf &wf : woelfe) {
                if (!wf.aktiv) {
                    if (!nacht || zufall(200) != 0) continue;
                    wf.richtung = zufall(2) ? 1 : -1;
                    wf.x        = (float)((wf.richtung > 0 ? -30 : B + 30) + off[kVorn]);
                    wf.tempo    = 0.45f + zufall(30) / 100.0f;
                    wf.pause    = 0;
                    wf.heult    = false;
                    wf.aktiv    = true;
                }
                if (wf.pause > 0) {
                    wf.pause--;
                    if (wf.pause == 0) { wf.heult = false; if (zufall(3) == 0) wf.richtung = (int8_t)-wf.richtung; }
                } else {
                    wf.x += wf.richtung * wf.tempo;
                    if (nacht && zufall(260) == 0) {
                        wf.pause = (int16_t)(40 + zufall(120));
                        wf.heult = w.sterne > 0.5f && zufall(2) == 0;
                    }
                }
                int px = (int)wf.x - off[kVorn];
                while (px < -kWeltB / 2) { px += kWeltB; wf.x += kWeltB; }
                while (px >= kWeltB / 2) { px -= kWeltB; wf.x -= kWeltB; }
                if (px < -80 || px > B + 80) {   // weit draussen: tagsueber verschwinden, nachts umkehren
                    if (!nacht) { wf.aktiv = false; continue; }
                    wf.richtung = px < 0 ? 1 : -1;
                }
                int wx = (int)wf.x % kWeltB;
                if (wx < 0) wx += kWeltB;
                int fx, fy;
                gebogen(px, grund[kVorn][wx] + 2, &fx, &fy);
                const int bein = (wf.pause > 0) ? 1 : (zeit / 5) % 2;
                for (int j = 0; j < kWolfH; j++) {
                    const char *zeile = wf.heult ? kHeulen[j] : (j < 10 ? kRumpf[j] : kBeine[bein][j - 10]);
                    for (int i = 0; i < kWolfB; i++) {
                        if (zeile[i] != '#') continue;
                        const int x = fx - kWolfB / 2 + (wf.richtung > 0 ? i : kWolfB - 1 - i), y = fy - kWolfH + j;
                        if (x >= 0 && x < B && y >= 0 && y < H) bild[y * BB + (x >> 3)] |= 0x80 >> (x & 7);
                    }
                }
            }
        }

        // Boden der vorderen Ebene unter Bildspalte x, schon gebogen: dort enden Regen und Schnee
        const auto boden = [&](int x) {
            int wx = (x + off[kVorn]) % kWeltB;
            if (wx < 0) wx += kWeltB;
            return (int)grund[kVorn][wx] + bogen[x < 0 ? 0 : (x >= B ? B - 1 : x)];
        };

        // Regen: schraege Striche, umgekehrt gezeichnet, damit sie auch vor
        // den schwarzen Baeumen zu sehen sind
        const int   tropfen_n = (int)(w.rg / 100.0f * kTropfen);
        const float schraeg   = w.wi * 0.012f;
        for (int i = 0; i < tropfen_n; i++) {
            Tropfen &t = tropfen[i];
            t.y += 6.0f + t.laenge * 0.4f;
            t.x -= (6.0f + t.laenge * 0.4f) * schraeg;
            if (t.y >= boden((int)t.x) || t.x < -20) {
                t.y = (float)(60 + zufall(80));
                t.x = (float)zufall(B + (int)(200 * schraeg) + 10);
            }
            const int laenge = (int)(t.laenge * (0.35f + 0.65f * w.rg / 100.0f));   // Niesel kurz, Regen lang
            for (int j = 0; j < laenge; j++) {
                const int x = (int)(t.x - j * schraeg), y = (int)t.y + j;
                if (y >= boden(x)) break;
                kippen(x, y);
            }
        }

        // Schnee: drei Groessen fuer drei Entfernungen, langsam und pendelnd
        const int flocken_n = (int)(w.sn / 100.0f * kFlocken);
        for (int i = 0; i < flocken_n; i++) {
            Flocke &f = flocken[i];
            const int groesse = i % 3;
            f.y += 0.5f + groesse * 0.35f;
            f.x += sin(f.phase + zeit * 0.05f) * 0.35f - w.wi * 0.01f * (1 + groesse);
            if (f.x < -4) f.x += B + 8;
            if (f.y >= boden((int)f.x) - groesse) { f.y = -3; f.x = (float)zufall(B + 40); }
            const int x = (int)f.x, y = (int)f.y;
            kippen(x, y);
            if (groesse >= 1) { kippen(x + 1, y); kippen(x, y + 1); kippen(x + 1, y + 1); }
            if (groesse == 2) { kippen(x - 1, y); kippen(x + 2, y + 1); kippen(x + 1, y - 1); kippen(x, y + 2); }
        }

        // Gewitter: ab und zu ein Blitz, der erste Takt hellt alles auf
        if (w.gw > 0.5f) {
            if (blitz == 0 && --blitz_warten <= 0) {
                blitz = 5;
                blitz_warten = 60 + zufall(200);
                blitz_x[0] = (int16_t)(60 + zufall(280));
                blitz_y[0] = 105;
                for (int i = 1; i < 10; i++) {
                    blitz_x[i] = (int16_t)(blitz_x[i - 1] + zufall(25) - 12);
                    blitz_y[i] = (int16_t)(blitz_y[i - 1] + 10 + zufall(8));
                }
            }
        }
        if (blitz > 0) {
            for (int i = 1; i < 10; i++) {
                const int dx = blitz_x[i] - blitz_x[i - 1], dy = blitz_y[i] - blitz_y[i - 1];
                for (int j = 0; j < dy; j++) {
                    const int x = blitz_x[i - 1] + dx * j / dy;
                    kippen(x, blitz_y[i - 1] + j); kippen(x + 1, blitz_y[i - 1] + j);
                }
            }
            if (blitz == 5) for (int i = 0; i < BB * H; i++) bild[i] ^= 0xff;
            blitz--;
        }
        // Funksignal: bei WLAN laufen von jeder Turmspitze Kreise nach aussen, so
        // viele nacheinander wie der Empfang Balken hat (0 bis 3). Jeder Kreis
        // ist ein Band, in dem die Luft wie Glas bricht: entlang des Strahls
        // wird das Bild dahinter gestaucht. Anfangs liegt eine duenne Linie
        // darauf, die mit dem Weg nach aussen verschwindet
        static int     wlan_stufe   = 0;
        static int64_t wlan_gefragt = 0;
        if (jetzt - wlan_gefragt > 2000000) {
            wlan_gefragt = jetzt;
            wifi_ap_record_t ap = {};
            wlan_stufe = (net::connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                       ? (ap.rssi > -60 ? 3 : (ap.rssi > -70 ? 2 : (ap.rssi > -80 ? 1 : 0))) : 0;
        }
        if (wlan_stufe > 0) {
            static const int kTakte = 170, kLeben = 100, kBand = 4;   // Runde etwa 13 s
            static uint8_t  *kopie = (uint8_t *)heap_caps_malloc(BB * H, MALLOC_CAP_SPIRAM);
            bool             kopiert = false;
            static const int kDeckung = 13;   // Linien zu gut 80 % gerastert
            const int        gw       = glas_x1 - glas_x0 + 1;
            for (int i = 0; i < kTuerme; i++) {
                int px = turm_x[i] - off[1];
                while (px < -kWeltB / 2) px += kWeltB;
                while (px >= kWeltB / 2) px -= kWeltB;
                if (px < -100 || px > B + 100) continue;
                int tx, ty;
                gebogen(px, turm_y[i], &tx, &ty);
                const int zyklus = (zeit + i * 47) % kTakte;
                int       ring_r[3], ring_a16[3], ring_linie[3], ringe = 0, rmax = 0, rmin = 999;
                for (int k = 0; k < wlan_stufe; k++) {
                    const int alter = zyklus - k * 24;
                    if (alter < 0 || alter >= kLeben) continue;
                    ring_r[ringe]     = 4 + 60 * alter / kLeben;
                    ring_a16[ringe]   = 13 - 13 * alter / kLeben;                            // Staerke der Brechung
                    ring_linie[ringe] = alter < kLeben * 45 / 100 ? 1 : (alter < kLeben * 70 / 100 ? 0 : -1);   // Linienbreite - 1
                    if (ring_r[ringe] > rmax) rmax = ring_r[ringe];
                    if (ring_r[ringe] < rmin) rmin = ring_r[ringe];
                    ringe++;
                }
                if (ringe == 0) continue;
                if (!kopiert) { memcpy(kopie, bild, BB * H); kopiert = true; }
                // Luft: nur Zeilenstuecke zwischen innerstem und aeusserstem Band
                const int ra = rmax + kBand, ri = rmin - kBand - 1;
                for (int dy = -ra; dy <= ra; dy++) {
                    const int y = ty + dy;
                    if (y < 0 || y >= H) continue;
                    const uint8_t *bay = kBayer[y & 3];
                    const int      xo  = wurzel[ra * ra - dy * dy];
                    const int      xi  = ri > abs(dy) ? wurzel[ri * ri - dy * dy] : -1;
                    for (int dx = -xo; dx <= xo; dx++) {
                        if (dx > -xi && dx < xi) dx = xi;
                        const int x = tx + dx;
                        if (x < 0 || x >= B) continue;
                        const uint8_t bit = 0x80 >> (x & 7);
                        if (y >= auf_oben && y <= auf_unten && ((auf_z[y * BB + (x >> 3)] | auf_h[y * BB + (x >> 3)]) & bit)) continue;   // Uhr und Schrift
                        if (glas_da && x >= glas_x0 && x <= glas_x1 && y >= glas_y0 && y <= glas_y1
                            && glas_art[(y - glas_y0) * gw + (x - glas_x0)]) continue;               // Pille: unten
                        const int d = wurzel[dx * dx + dy * dy];
                        for (int k = 0; k < ringe; k++) {
                            const int o = d - ring_r[k];
                            if (o < -kBand || o > kBand) continue;
                            // Quelle weiter aussen bzw. innen auf demselben Strahl
                            const int sd = d + o * ring_a16[k] / 16;
                            int       qx = d ? tx + dx * sd / d : tx, qy = d ? ty + dy * sd / d : ty;
                            qx = qx < 0 ? 0 : (qx >= B ? B - 1 : qx);
                            qy = qy < 0 ? 0 : (qy >= H ? H - 1 : qy);
                            bool an = (kopie[qy * BB + (qx >> 3)] >> (7 - (qx & 7))) & 1;
                            if (o >= 0 && o <= ring_linie[k] && bay[x & 3] < kDeckung) an = true;
                            uint8_t &z = bild[y * BB + (x >> 3)];
                            z = an ? (uint8_t)(z | bit) : (uint8_t)(z & ~bit);
                            break;
                        }
                    }
                }
                // Hinter der Pille: die Linie am gebrochenen Ort, halb so deutlich
                if (glas_da) {
                    const int xa = tx - ra - 16 > glas_x0 ? tx - ra - 16 : glas_x0, xe = tx + ra + 16 < glas_x1 ? tx + ra + 16 : glas_x1;
                    const int ya = ty - ra - 16 > glas_y0 ? ty - ra - 16 : glas_y0, ye = ty + ra + 16 < glas_y1 ? ty + ra + 16 : glas_y1;
                    for (int y = ya < 0 ? 0 : ya; y <= ye && y < H; y++) {
                        const uint8_t *bay = kBayer[y & 3];
                        for (int x = xa; x <= xe; x++) {
                            const int gi = (y - glas_y0) * gw + (x - glas_x0);
                            if (!glas_art[gi] || bay[x & 3] >= kDeckung / 2) continue;
                            const uint8_t bit = 0x80 >> (x & 7);
                            if (auf_z[y * BB + (x >> 3)] & bit) continue;
                            if (glas_ton[(y - fang_y0) * 400 + x] & 0x80) continue;
                            const int qdx = x + glas_dx[gi] - tx, qdy = y + glas_dy[gi] - ty, q2 = qdx * qdx + qdy * qdy;
                            if (q2 >= kWurzelMax * kWurzelMax) continue;
                            const int dq = wurzel[q2];
                            for (int k = 0; k < ringe; k++) {
                                const int o = dq - ring_r[k];
                                if (o >= 0 && o <= ring_linie[k]) { bild[y * BB + (x >> 3)] |= bit; break; }
                            }
                        }
                    }
                }
            }
        }

        // Mikrofon ueber der Uhr: stumm gar nichts. Hoert es zu oder spricht
        // HoiHoi, laeuft eine schlichte Welle, deren Hoehe dem Pegel folgt und
        // die zu den Enden ausklingt; beim Nachdenken springen drei Punkte
        // versetzt auf und ab. Alles mit weissem Rand wie die Uhr
        {
            static const int kMitteX = 200, kMitteY = 45, kHalb = 30;
            const oberflaeche::Zustand zu    = ui_zustand();
            const bool                 stumm = (!kWeckwort || mikro_stumm != 0) && zu == oberflaeche::Zustand::Ruhe;
            static float anzeige = 0, phase = 0;
            const float  p = stumm ? 0.0f : (zu == oberflaeche::Zustand::Antwortet ? stimm_anteil() : mikro_anteil());
            anzeige = p > anzeige ? p : anzeige * 0.85f;   // schnell hoch, langsam zurueck
            phase += 0.35f;
            const auto setze = [&](int x, int y, bool schwarz) {
                if (x < 0 || x >= B || y < 0 || y >= H) return;
                uint8_t &z = bild[y * BB + (x >> 3)];
                z = schwarz ? (uint8_t)(z | (0x80 >> (x & 7))) : (uint8_t)(z & ~(0x80 >> (x & 7)));
            };
            // HoiHoi erkannt: ein Ring springt aus der Mitte auf und verblasst
            static oberflaeche::Zustand zu_vorher = oberflaeche::Zustand::Ruhe;
            static int                  blip      = 99;
            if (zu_vorher == oberflaeche::Zustand::Ruhe && zu == oberflaeche::Zustand::HoertZu) blip = 0;
            zu_vorher = zu;
            if (blip < 12) {
                const float r    = 6 + blip * 2.6f;
                const int   deck = 16 - blip;   // wird zum Ende luftiger
                const int   ra   = (int)r + 3;
                for (int yy = -ra; yy <= ra; yy++) {
                    const int y = kMitteY + yy;
                    if (y < 0 || y >= H) continue;
                    const uint8_t *bay = kBayer[y & 3];
                    for (int xx = -ra; xx <= ra; xx++) {
                        const float d = sqrt((float)(xx * xx + yy * yy)) - r;
                        if (d < -2.5f || d > 2.5f) continue;
                        const int x = kMitteX + xx;
                        if (d >= -1 && d <= 1) { if (bay[x & 3] < deck) setze(x, y, true); }
                        else setze(x, y, false);   // weisser Rand
                    }
                }
                blip++;
            }
            if (stumm) {
                // nichts
            } else if (zu == oberflaeche::Zustand::DenktNach) {
                static const float kPunkt = 2.6f;
                for (int pass = 0; pass < 2; pass++)   // erst Rand, dann Punkte
                    for (int i = 0; i < 3; i++) {
                        const float sp = sin(phase * 0.8f - i * 0.9f);
                        const int   cx = kMitteX + (i - 1) * 11, cy = kMitteY - (int)(sp > 0 ? 5 * sp + 0.5f : 0);
                        const float r  = pass == 0 ? kPunkt + 2 : kPunkt;
                        for (int yy = -5; yy <= 5; yy++) for (int xx = -5; xx <= 5; xx++)
                            if (xx * xx + yy * yy <= r * r) setze(cx + xx, cy + yy, pass == 1);
                    }
            } else {
                const float amp = 1.5f + 9 * anzeige;
                const auto  mitte = [&](int x) {
                    const float u = (float)(x - (kMitteX - kHalb)) / (2 * kHalb);
                    return kMitteY - amp * sin((float)M_PI * u) * sin(tau * (x - kMitteX) / 20.0f - phase);
                };
                for (int pass = 0; pass < 2; pass++)   // erst Rand, dann Linie
                    for (int x = kMitteX - kHalb - 2; x <= kMitteX + kHalb + 2; x++) {
                        const int   xi = x < kMitteX - kHalb ? kMitteX - kHalb : (x > kMitteX + kHalb ? kMitteX + kHalb : x);
                        const float a = mitte(xi), b = mitte(xi < kMitteX + kHalb ? xi + 1 : xi);
                        const int   y0 = (int)floor(a < b ? a : b), y1 = (int)ceil(a < b ? b : a);
                        const int   rand = pass == 0 ? 2 : 0;
                        if (pass == 1 && xi != x) continue;
                        for (int y = y0 - rand; y <= y1 + rand; y++) setze(x, y, pass == 1);
                    }
            }
        }
        if (jetzt < etikett_bis) {
            int bx = 0;
            for (const char *p = etikett;;) {
                const uint16_t z = schrift_naechstes(p);
                if (z == 0) break;
                const Glyphe *g = schrift_glyphe(kSchriftNormal, z);
                bx += (g ? g : schrift_glyphe(kSchriftNormal, '?'))->vorschub;
            }
            static const int kX = 6, kY = 6;
            for (int y = kY; y < kY + kSchriftNormal.hoehe + 4; y++)
                for (int x = kX; x < kX + bx + 10 && x < B; x++) bild[y * BB + (x >> 3)] &= ~(0x80 >> (x & 7));
            int stift = kX + 5;
            for (const char *p = etikett;;) {
                const uint16_t z = schrift_naechstes(p);
                if (z == 0) break;
                const Glyphe *g = schrift_glyphe(kSchriftNormal, z);
                if (g == nullptr) g = schrift_glyphe(kSchriftNormal, '?');
                const int zb = (g->breite + 7) / 8;
                for (int gy = 0; gy < kSchriftNormal.hoehe; gy++)
                    for (int gx = 0; gx < g->breite; gx++) {
                        if (!((kSchriftNormal.bits[g->ofs + gy * zb + gx / 8] << (gx % 8)) & 0x80)) continue;
                        const int px = stift + g->links + gx, py = kY + 2 + gy;
                        if (px >= 0 && px < B) bild[py * BB + (px >> 3)] |= 0x80 >> (px & 7);
                    }
                stift += g->vorschub;
            }
        }
        teil_us[3] += esp_timer_get_time() - teil;
        rechnen_us += esp_timer_get_time() - r0;
        teil = esp_timer_get_time();

        display->bitmap(0, 0, B, H, bild);
        display->flush();
        teil_us[4] += esp_timer_get_time() - teil;

        if (++takte == 250) {
            {
                static TaskStatus_t *ts      = (TaskStatus_t *)heap_caps_calloc(32, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
                static TaskStatus_t *vor     = (TaskStatus_t *)heap_caps_calloc(32, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
                static UBaseType_t   vor_n   = 0;
                static int64_t       vor_us  = 0;
                uint32_t             gesamt  = 0;
                const UBaseType_t    n       = uxTaskGetSystemState(ts, 32, &gesamt);
                const int64_t        dt      = jetzt - vor_us;
                char zeile[256];
                int  p = snprintf(zeile, sizeof zeile, "Zustand %d:", (int)ui_zustand());
                for (UBaseType_t i = 0; i < n && p < (int)sizeof zeile - 24; i++)
                    for (UBaseType_t k = 0; k < vor_n; k++) {
                        if (vor[k].xHandle != ts[i].xHandle) continue;
                        const int proz = (int)((int64_t)(ts[i].ulRunTimeCounter - vor[k].ulRunTimeCounter) * 100 / (dt > 0 ? dt : 1));
                        if (proz >= 3) p += snprintf(zeile + p, sizeof zeile - p, " %s=%d%%", ts[i].pcTaskName, proz);
                        break;
                    }
                memcpy(vor, ts, n * sizeof(TaskStatus_t));
                vor_n  = n;
                vor_us = jetzt;
                ESP_LOGI(TAG, "Dunst: %s", zeile);
            }
            ESP_LOGI(TAG, "Dunst: %" PRId64 " ms je Takt, davon %" PRId64 " ms Rechnen; intern frei %u Byte.",
                     (jetzt - ms_log) / 1000 / takte, rechnen_us / 1000 / takte,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            ESP_LOGI(TAG, "Dunst: Zeilen oben %d ms, unten %d ms, Teilung bei %d.", (int)(teil_eigen_us / 1000 / takte),
                     (int)(teil_helfer_us / 1000 / takte), teilung);
            teil_eigen_us = teil_helfer_us = 0;
            ESP_LOGI(TAG, "Dunst: Teile %d/%d/%d/%d ms, Bild %d ms.", (int)(teil_us[0] / 1000 / takte),
                     (int)(teil_us[1] / 1000 / takte), (int)(teil_us[2] / 1000 / takte), (int)(teil_us[3] / 1000 / takte),
                     (int)(teil_us[4] / 1000 / takte));
            for (int64_t &v : teil_us) v = 0;
            ms_log = jetzt; takte = 0; rechnen_us = 0;
        }
    }
}

#pragma GCC pop_options

static void ui_task(void *)
{
    if (kDunstProbe) dunst_laufen();
    if (kPilzProbe) pilz_laufen();

    oberflaeche::Stand s;

    int64_t gezeichnet = 0;
    int64_t wlan_zuletzt = 0;
    int32_t ms_max = 0;
    int64_t ms_log = esp_timer_get_time();

    while (true) {
        const int64_t jetzt = esp_timer_get_time();
        if (jetzt - gezeichnet < kWelleUs) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        gezeichnet = jetzt;

        s.zustand = ui_zustand();
        s.stumm   = mikro_stumm != 0;
        switch (s.zustand) {
        case oberflaeche::Zustand::HoertZu:   s.pegel = mikro_anteil(); break;
        case oberflaeche::Zustand::Antwortet: s.pegel = stimm_anteil(); break;
        default:                              s.pegel = 0.0f;           break;
        }

        // Vor der ersten Antwort vom Zeitserver steht die Uhr auf 1970.
        const time_t now = time(nullptr);
        struct tm    tm  = {};
        localtime_r(&now, &tm);
        s.zeit_gueltig = tm.tm_year + 1900 >= 2025;
        s.stunde       = tm.tm_hour;
        s.minute       = tm.tm_min;
        s.wochentag    = tm.tm_wday;
        s.tag          = tm.tm_mday;
        s.monat        = tm.tm_mon + 1;
        s.jahr         = tm.tm_year + 1900;

        s.klima_gueltig = env_valid != 0;
        s.temp_c100     = env_temp_c100;
        s.feuchte_100   = env_hum_100;
        s.akku_prozent  = akku_prozent();

        // Der Empfang schwankt von Abfrage zu Abfrage; alle zwei Sekunden
        // reicht, und jede neue Zahl zeichnet das ganze Bild.
        if (jetzt - wlan_zuletzt > 2000000) {
            wlan_zuletzt = jetzt;
            wifi_ap_record_t ap = {};
            if (net::provisioning()) {
                s.wlan = oberflaeche::Stand::Wlan::Einrichten;
            } else if (net::connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                s.wlan = oberflaeche::Stand::Wlan::Verbunden;
                s.rssi = ap.rssi;
            } else {
                s.wlan = oberflaeche::Stand::Wlan::Aus;
            }
        }

        const int64_t t0 = esp_timer_get_time();
        oberflaeche::zeichnen(*display, s);
        const int32_t ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
        if (ms > ms_max) ms_max = ms;
        display->flush();

        if (jetzt - ms_log > 60000000) {
            ESP_LOGI(TAG, "Oberflaeche: Zeichnen hoechstens %d ms je Bild. Intern frei %u, "
                          "am knappsten %u, groesster Block %u, Stack uebrig %u.",
                     (int)ms_max, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            ms_log = jetzt;
            ms_max = 0;
        }
    }
}

static void audio_task(void *);

// Nachgemessen und damit erledigt: der Aufnahmetask wird nie verdraengt. Ein
// Wachtask auf Kern 0 hat ueber zwei Runden kein einziges Mal gesehen, dass er
// bereit war und nicht drankam — wenn er steht, wartet er auf Ton, der nicht
// kommt. An den Prioritaeten liegt es also nicht. Der Handschlag, der die
// Luecke verursacht, faellt seit dem Vorhalten der Sitzung nicht mehr in die
// Aufnahme.

// --- Stau-Melder ----------------------------------------------------------
//
// Zweimal gemessen und beide Male unerklaert: der Aufnahmetask auf Kern 1
// steht waehrend des Verbindungsaufbaus der Erkennung ueber eine Sekunde
// still, obwohl er mit Prioritaet 6 ueber allem liegt, was diese Firmware
// selbst anlegt — und der Anzeigetask daneben ebenso. Aus dem Log allein ist
// nicht zu sehen, wer ihn verdraengt; aus der Laufzeitstatistik von FreeRTOS
// schon. Bei jedem Durchgang eine Momentaufnahme, und wenn zwischen zwei
// Durchgaengen eine Luecke klafft, die Differenz dazu.
static const int    kStauTasks = 24;
static TaskStatus_t s_stau_vorher[kStauTasks];
static UBaseType_t  s_stau_n = 0;

static void stau_schnappschuss(void)
{
    uint32_t gesamt = 0;
    s_stau_n = uxTaskGetSystemState(s_stau_vorher, kStauTasks, &gesamt);
}

static void stau_melden(int luecke_ms, int vor_ms, int lesen_ms, int nach_ms)
{
    static TaskStatus_t jetzt[kStauTasks];

    uint32_t          gesamt = 0;
    const UBaseType_t n      = uxTaskGetSystemState(jetzt, kStauTasks, &gesamt);

    char zeile[220];
    int  p = 0;
    for (UBaseType_t i = 0; i < n && p < (int)sizeof(zeile) - 28; i++) {
        uint32_t vorher = 0;
        bool     kannte = false;
        for (UBaseType_t k = 0; k < s_stau_n; k++) {
            if (s_stau_vorher[k].xHandle == jetzt[i].xHandle) {
                vorher = s_stau_vorher[k].ulRunTimeCounter;
                kannte = true;
                break;
            }
        }
        if (!kannte) continue;

        const int32_t delta = (int32_t)(jetzt[i].ulRunTimeCounter - vorher) / 1000;
        if (delta < 20) continue;
        p += snprintf(&zeile[p], sizeof(zeile) - p, " %s=%d",
                      jetzt[i].pcTaskName, (int)delta);
    }
    nachtrag::schreiben('W', TAG, "Stau %d ms (davor %d, lesen %d, danach %d):%s",
                        luecke_ms, vor_ms, lesen_ms, nach_ms, zeile);
}

// --- Hineinsprechen: Worte oder nicht? --------------------------------------
//
// Die Pruefaufnahme aus echo.h kommt hier mit ihrem Endtext an. Zwei Dinge
// sollen nicht als Hineinsprechen gelten:
//
//   - Geraeusche. Die Erkennung macht daraus meist gar keinen Text, manchmal
//     ein "Hm". Verlangt wird deshalb mindestens ein Wort mit drei Zeichen.
//   - Das eigene Echo. Was die Echounterdrueckung durchlaesst, ist die Stimme
//     der Antwort, und die Erkennung schreibt es brav mit — im ersten Test
//     kam so "Leute haben daran gearbeitet." zustande. Solcher Text steht
//     in der Antwort, und zwar Wort fuer Wort in derselben Reihenfolge. Eine
//     echte Zwischenfrage kann einzelne Worte der Antwort enthalten, drei in
//     Folge aber kaum.

struct Wortstelle {
    size_t von;
    size_t len;
};

static bool ist_buchstabe(char c)
{
    const unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
           || u >= 0x80;   // Umlaute und alles andere aus UTF-8 zaehlt mit
}

// Zerlegt s in Worte und schreibt es dabei klein (nur ASCII; Umlaute kommen
// von der Erkennung und aus dem Chat gleich geschrieben).
static int worte_zerlegen(char *s, Wortstelle *w, int max)
{
    int    n = 0;
    size_t i = 0;
    while (s[i] != '\0' && n < max) {
        while (s[i] != '\0' && !ist_buchstabe(s[i])) i++;
        const size_t a = i;
        while (s[i] != '\0' && ist_buchstabe(s[i])) {
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
            i++;
        }
        if (i > a) w[n++] = {a, i - a};
    }
    return n;
}

static bool wort_gleich(const char *a, const Wortstelle &x, const char *b, const Wortstelle &y)
{
    return x.len == y.len && memcmp(a + x.von, b + y.von, x.len) == 0;
}

// Laeuft im WebSocket-Task der Erkennung, siehe Stt::pruefer().
static bool hineingesprochen(const char *text)
{
    static char       satz[Stt::kMaxText];
    static char       antwort[Chat::kMaxAntwort];
    static Wortstelle sw[64];
    static Wortstelle aw[256];

    snprintf(satz, sizeof(satz), "%s", text);
    chat.copy_antwort(antwort, sizeof(antwort));
    const int ns = worte_zerlegen(satz, sw, 64);
    const int na = worte_zerlegen(antwort, aw, 256);

    int lang = 0;
    for (int i = 0; i < ns; i++) {
        if (sw[i].len >= 3) lang++;
    }

    // Die laengste Folge von Worten, die genau so auch in der Antwort steht.
    int folge = 0;
    for (int i = 0; i < ns; i++) {
        for (int j = 0; j < na; j++) {
            int k = 0;
            while (i + k < ns && j + k < na
                   && wort_gleich(satz, sw[i + k], antwort, aw[j + k])) {
                k++;
            }
            if (k > folge) folge = k;
        }
    }

    const bool leer = (lang == 0);
    const bool echo = !leer && (folge >= 3 || (ns <= 2 && folge == ns) || folge * 10 >= ns * 6);

    ESP_LOGI(TAG, "Pruefaufnahme: %d Worte, %d davon ab drei Zeichen, %d in Folge aus "
                  "der Antwort — %s.",
             ns, lang, folge,
             leer ? "keine Worte" : echo ? "Echo der Antwort" : "hineingesprochen");

    if (leer || echo) return false;
    tts.abbrechen();
    return true;
}

// Richtet alles ein, was zum Aufnehmen, Erkennen, Antworten und Sprechen
// gehoert, und uebergibt den Takt danach an audio_task(). true heisst: es
// laeuft, der Haupttask wird nicht mehr gebraucht.
static bool visualize_mic(void)
{
    ESP_LOGI(TAG, "--- Mikrofon-Visualisierung ---");

    if (mic.begin(i2c_bus, kSampleRate, kMicDb) != ESP_OK) {
        ESP_LOGE(TAG, "  Mikrofon nicht verfuegbar, Visualisierung entfaellt.");
        return false;
    }

    ESP_LOGI(TAG, "  %d Hz, Bloecke von %d Frames.", (int)mic.sample_rate(), kReadFrames);

    const esp_err_t lerr = listener.begin(mic.sample_rate());
    if (lerr == ESP_OK) {
        ESP_LOGI(TAG, "  Das Weckwort startet die Aufnahme; sie endet nach "
                      "%d ms Stille, spaetestens nach %d s. Ohne erstes Wort "
                      "bricht sie nach %d ms ab.",
                 (int)Listener::kStilleMs, Listener::kMaxSeconds,
                 (int)Listener::kWartenMs);
        ESP_LOGI(TAG, "  KEY (GPIO%d) bricht eine laufende Antwort ab.",
                 KEY_BUTTON_PIN);
    } else {
        ESP_LOGW(TAG, "  Kein Aufnahmepuffer (%s) — das Weckwort schaltet nur "
                      "den Zustand um, ohne Mitschnitt.",
                 esp_err_to_name(lerr));
    }

    // Fuer Weckwort-Tests: Erkennung, Chat und Stimme bleiben aus. Wer
    // zwanzigmal "ha ha" sagt, um Fehlausloesungen zu zaehlen, will dafuer
    // nicht zwanzig Transkriptionen bezahlen — und jede ausgeloeste Aufnahme
    // schickt auch drei Sekunden Stille zur Erkennung. Aufgenommen wird
    // trotzdem, damit Abbruch und Satzende weiter zu sehen sind.
    //
    // Umgesetzt als leerer Schluessel: das ist der Weg, den alle drei ohnehin
    // sauber gehen, wenn secrets.h fehlt.
    const char *const ki_schluessel = kOhneKi ? "" : OPENAI_API_KEY;
    if (kOhneKi) {
        ESP_LOGW(TAG, "  kOhneKi: Erkennung, Chat und Stimme bleiben aus, "
                      "keine Anfragen an OpenAI.");
    }

    // Spracherkennung. Ohne Schluessel oder ohne WLAN bleibt sie aus, und
    // alles andere laeuft unveraendert weiter — die Firmware soll auch auf
    // einem Geraet ohne secrets.h benutzbar bleiben.
    const esp_err_t serr = stt.begin(&listener, mic.sample_rate(),
                                     ki_schluessel, STT_MODEL, STT_LANGUAGE);
    if (serr == ESP_OK) {
        stt.pruefer(&hineingesprochen);
        ESP_LOGI(TAG, "  Transkription aktiv: %s, Sprache %s.",
                 STT_MODEL, STT_LANGUAGE);
    } else if (serr == ESP_ERR_INVALID_ARG) {
        if (!kOhneKi) ESP_LOGW(TAG, "  Kein OPENAI_API_KEY in secrets.h — es wird "
                      "aufgenommen, aber nicht erkannt.");
    } else {
        ESP_LOGE(TAG, "  Transkription nicht gestartet (%s).",
                 esp_err_to_name(serr));
    }

    // Antwort auf die erkannte Frage. Haengt am Stt und braucht denselben
    // Schluessel; ohne ihn bleibt es bei Aufnahme und Transkript.
    const esp_err_t cerr = chat.begin(&stt, ki_schluessel, CHAT_MODEL);
    if (cerr == ESP_OK) {
        ESP_LOGI(TAG, "  Chat aktiv: %s.", CHAT_MODEL);
    } else if (cerr == ESP_ERR_INVALID_ARG) {
        if (!kOhneKi) ESP_LOGW(TAG, "  Kein OPENAI_API_KEY in secrets.h — keine Antworten.");
    } else {
        ESP_LOGE(TAG, "  Chat nicht gestartet (%s).", esp_err_to_name(cerr));
    }

    // Hohe Prioritaet, obwohl hier nichts eilt: dieser Task
    // fasst den I2C-Bus an, und den teilt er sich mit Mikrofon und
    // Lautsprecher. Die Bussperre der IDF vererbt keine Prioritaet — ein
    // Sensortask, der mitten in einer Uebertragung verdraengt wird, laesst
    // das Mikrofon so lange warten, wie er selbst wartet. Gerechnet wird hier
    // nichts; die zwanzig Millisekunden einer Messung sind vTaskDelay.
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, nullptr, 6, nullptr, 0);

    // Die Anzeige auf Kern 1 unter dem Aufnahmetakt: ein ausgelassenes Bild
    // faellt nicht auf, eine verlorene Silbe schon.
    if (!kAnimationen) {
        // Stack im PSRAM, siehe Stt::begin(); gezeichnet wird in den
        // internen Bildpuffer, der Stack traegt nur Zahlen und Text.
        // Kern 0 und Prioritaet 3: Kern 1 gehoert der Aufnahme, deren Echounterdrueckung
        // waehrend einer Antwort ein Drittel davon braucht; auf Kern 0 steht die
        // Stimme (tts_spiel, 4) weiter vor dem Bild
        xTaskCreatePinnedToCoreWithCaps(ui_task, "display", 8192, nullptr, 3, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else if (wach.begin(wach_start, wach_ende) == ESP_OK && zu.begin(zu_start, zu_ende) == ESP_OK) {
        xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 4, nullptr, 1);
    }

    // Der Lautsprecher bleibt, die Wiedergabe der eigenen Aufnahme nicht.
    // Sie war ein Diagnosemittel fuer die Aufnahmequalitaet, und die ist
    // geklaert; ab jetzt gehoert der Wandler der gesprochenen Antwort.
    if (speaker.begin(i2c_bus, mic.sample_rate()) == ESP_OK) {
        ESP_LOGI(TAG, "  Lautsprecher bereit.");
    } else {
        ESP_LOGW(TAG, "  Kein Lautsprecher — die Antwort bleibt stumm.");
    }

    // Sprachausgabe zuletzt: sie braucht den Lautsprecher und haengt am Chat.
    const esp_err_t terr = tts.begin(&chat, &speaker, &listener,
                                     ki_schluessel, TTS_MODEL, TTS_VOICE);
    if (terr == ESP_OK) {
        ESP_LOGI(TAG, "  Sprachausgabe aktiv: %s, Stimme %s.", TTS_MODEL, TTS_VOICE);
    } else if (terr == ESP_ERR_INVALID_ARG) {
        if (!kOhneKi) ESP_LOGW(TAG, "  Sprachausgabe aus — Antwort erscheint nur als Text.");
    } else {
        ESP_LOGE(TAG, "  Sprachausgabe nicht gestartet (%s).",
                 esp_err_to_name(terr));
    }

    // Das Weckwort braucht seine Tabellen, bevor der erste Block hereinkommt:
    // im Aufnahmetask darf nichts mehr belegt werden.
    const esp_err_t werr = wachwort::bereit();
    if (werr == ESP_OK && vergleich::eingelernt() > 0) {
        ESP_LOGI(TAG, "  Weckwort hoert mit, %d Vorlagen stehen.",
                 vergleich::eingelernt());
    } else if (werr == ESP_OK) {
        ESP_LOGI(TAG, "  Weckwort hoert mit. Noch keine Vorlage — BOOT lang "
                      "halten und das Wort %d mal sagen, %d nah und %d aus 2 m.",
                 (int)vergleich::kVorlagen, (int)vergleich::kJeLage,
                 (int)vergleich::kJeLage);
    } else {
        ESP_LOGW(TAG, "  Weckwort aus (%s).", esp_err_to_name(werr));
    }

    // Ohne Echounterdrueckung laeuft alles wie frueher weiter, nur dass man
    // waehrend einer Antwort nicht hineinsprechen kann.
    if (echo::bereit() == ESP_OK) {
        ESP_LOGI(TAG, "  Echounterdrueckung aktiv: in eine Antwort hineinsprechen "
                      "bricht sie ab (Mikrofon waehrenddessen %.1f dB).", kMicDbAntwort);
    } else {
        ESP_LOGW(TAG, "  Keine Echounterdrueckung — eine Antwort ist nur mit KEY "
                      "abzubrechen.");
    }

    // Der Aufnahmetakt bekommt einen eigenen Task, auf dem zweiten Kern und
    // ueber der Anzeige.
    //
    // Er lief bisher im Haupttask, und der hat Prioritaet 1 auf Kern 0 —
    // unter allem, was das Netz anfasst. Waehrend einer Aufnahme laufen dort
    // bis zu drei TLS-Handschlaege gleichzeitig (Erkennung, Chat, Stimme),
    // jeder ueber eine Sekunde reine Rechenarbeit auf Prioritaet 3. Der
    // Aufnahmetakt kam in dieser Zeit nicht mehr dran, der I2S-Ring lief
    // ueber, und eine Aufnahme von zweieinhalb Sekunden endete als
    //
    //   listen: Zuhoeren beendet (Taste losgelassen): 2477 ms, 0 Frames
    //   stt: Dienstfehler: ... buffer only has 0.00ms of audio.
    //
    // obwohl der Pegelmesser im selben Augenblick Sprache zeigte: die paar
    // Bloecke, die durchkamen, reichten fuer den Messwert, nicht fuer den
    // Mitschnitt. Der Ring fasst 60 ms — wer ihn leert, darf nicht warten
    // muessen. Kern 1 hat ausser der Anzeige nichts zu tun, und dort steht
    // der Aufnahmetakt ueber ihr: ein ausgelassenes Bild faellt nicht auf,
    // eine verlorene Silbe schon.
    // 8 KB Stapel: esp_aec rechnet in diesem Task mit.
    ESP_LOGI(TAG, "  Intern frei: %u Byte, groesster Block %u Byte.",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 6, nullptr, 1)
            != pdPASS) {
        ESP_LOGE(TAG, "  Aufnahmetask konnte nicht angelegt werden.");
        return false;
    }
    return true;
}

// Der Aufnahmetakt selbst: Taste abfragen, einen Block lesen, ihn an den
// Mitschnitt weiterreichen, Wellenbild und Pegel nachfuehren. Zwanzig
// Millisekunden je Durchgang, und die bleiben es auch unter Last.
static void audio_task(void *)
{
    static int16_t block[kReadFrames];
    static int16_t ref_block[kReadFrames];

    // Gereinigter Ton aus der Echounterdrueckung: je Block hoechstens ein
    // esp_aec-Block hochgetaktet, reichlich bemessen. Der Vorspann gehoert in
    // den PSRAM, er ist knapp 40 KB gross.
    static const size_t kSauberFrames   = 2048;
    static int16_t      sauber[kSauberFrames];
    const size_t        kVorspannFrames = kSampleRate * echo::kVorspannMs / 1000;
    int16_t *const      vorspann = (int16_t *)heap_caps_malloc(kVorspannFrames * sizeof(int16_t),
                                                              MALLOC_CAP_SPIRAM);
    if (vorspann == nullptr) {
        nachtrag::schreiben('E', TAG, "Kein Speicher fuer den Vorspann.");
        vTaskDelete(nullptr);
    }

    int64_t        sum_sq    = 0;
    int32_t        sum_count = 0;
    int32_t        window_pk = 0;
    int64_t        last_log  = esp_timer_get_time();

    int32_t rms = 0;

    while (true) {
        const int64_t t_a = esp_timer_get_time();

        // Das Mikrofon laeuft durch, auch waehrend einer Antwort: der
        // Lautsprecher belegt den Port nicht mehr, siehe audio.h. Was sich
        // mit einer Antwort aendert, ist nur, wohin der Ton geht — waehrend
        // sie laeuft, durch die Echounterdrueckung statt ans Weckwort.
        static bool nachfrage_offen = false;
        static bool antwort_lief    = false;
        static bool    geprueft      = false;   // zum offenen Verdacht lief schon eine Pruefaufnahme
        static int64_t pruefung_ende = 0;
        static int64_t hoeren_seit   = 0;   // seit wann hoeren_gleich steht
        const int64_t  kPruefPauseUs = 1500000;
        static int64_t stimme_zuletzt = 0;   // wann der Lautsprecher zuletzt lief

        if (!mic.running()) {
            mic.start();
            wachwort::ruhe();
        }

        // --- KEY ---
        //
        // Das Weckwort startet, die Taste haelt an — je nachdem, was gerade
        // laeuft:
        //   Zuhoeren       -> Aufnahme verwerfen, keine Frage, zurueck in Ruhe
        //   Denken/Antwort -> Erkennung, Chat und Stimme abbrechen und gleich
        //                     zuhoeren, wie nach einer Antwort (Nachfrage)
        //   Ruhe           -> Mikrofon stumm bzw. wieder an
        //
        // Gedrueckt zaehlt nur nach mindestens drei offenen Runden (60 ms)
        // davor: so prellt weder das Druecken noch das Loslassen doppelt.
        {
            static int offen = 0;
            const int  key   = gpio_get_level(KEY_BUTTON_PIN);
            if (key != 0) {
                if (offen < 1000) offen++;
            } else {
                if (offen >= 3) {
                    const oberflaeche::Zustand z = ui_zustand();
                    if (hoeren_gleich || (listener.listening() && !listener.pruefung())) {
                        listener.abbrechen();
                        stt.verwerfen();
                        hoeren_gleich   = 0;
                        nachfrage_offen = false;
                        nachtrag::schreiben('I', TAG, "Taste: Zuhoeren abgebrochen.");
                    } else if (!kWeckwort && z == oberflaeche::Zustand::Ruhe && !tts.spricht()) {
                        listener.wecken(wachwort::ruhepegel());
                        nachfrage_offen = false;
                        nachtrag::schreiben('I', TAG, "Taste: hoert zu.");
                    } else if (z == oberflaeche::Zustand::Ruhe && !tts.spricht()) {
                        mikro_stumm     = !mikro_stumm;
                        nachfrage_offen = false;
                        if (!mikro_stumm) wachwort::ruhe();   // frisch einschwingen
                        nachtrag::schreiben('I', TAG, "Taste: Mikrofon %s.",
                                            mikro_stumm ? "stumm" : "wieder an");
                    } else {
                        listener.abbrechen();   // eine laufende Pruefaufnahme
                        stt.verwerfen();
                        chat.abbrechen();
                        tts.abbrechen();
                        // Laeuft die Stimme noch, oeffnet ihr Ende die
                        // Nachfrage (unten); sonst geht es sofort los.
                        hoeren_gleich   = 1;
                        hoeren_seit     = esp_timer_get_time();
                        nachfrage_offen = !tts.spricht();
                        nachtrag::schreiben('I', TAG, "Taste: Antwort abgebrochen, hoert zu.");
                    }
                }
                offen = 0;
            }
        }

        // Die Anzeige wartet nicht ewig: kam das Zuhoeren nicht zustande,
        // etwa weil ein Weckwort-Einschwinger nie fertig wurde, zurueck.
        if (hoeren_gleich
            && (listener.listening() || esp_timer_get_time() - hoeren_seit > 3000000)) {
            hoeren_gleich = 0;
        }

        const bool antwort = tts.spricht();
        if (antwort) stimme_zuletzt = esp_timer_get_time();
        if (antwort && !antwort_lief) {
            // Anschlagzaehler seit dem letzten Mal: das ist die Ruhe davor,
            // und darin muss MIC4 still sein und die Referenz fast auch —
            // sonst stimmt die Schlitzreihenfolge nicht.
            nachtrag::schreiben('I', TAG, "Antwort beginnt. Spitzen davor: MIC1 %d, "
                                          "MIC2 %d, Referenz %d, MIC4 %d.",
                                (int)mic.spitze(MicInput::kMic1), (int)mic.spitze(MicInput::kMic2),
                                (int)mic.spitze(MicInput::kRef), (int)mic.spitze(MicInput::kMic4));
            mic.messung_leeren();
            if (echo::aktiv()) {
                mic.verstaerkung(kMicDbAntwort);
                echo::beginnen(wachwort::ruhepegel(),
                               powf(10.0f, (kMicDb - kMicDbAntwort) / 20.0f));
            }
            nachfrage_offen = false;
            geprueft        = false;
        } else if (!antwort && antwort_lief) {
            mic.verstaerkung(kMicDb);
            echo::beenden();
            nachtrag::schreiben('I', TAG, "Spitzen waehrend der Antwort: MIC1 %d (%d am "
                                          "Anschlag), MIC2 %d (%d), Referenz %d (%d), MIC4 %d.",
                                (int)mic.spitze(MicInput::kMic1), (int)mic.anschlag(MicInput::kMic1),
                                (int)mic.spitze(MicInput::kMic2), (int)mic.anschlag(MicInput::kMic2),
                                (int)mic.spitze(MicInput::kRef), (int)mic.anschlag(MicInput::kRef),
                                (int)mic.spitze(MicInput::kMic4));
            mic.messung_leeren();

            // Das Weckwort hat waehrend der Antwort nichts bekommen und
            // faengt neu an; seine Einschwingzeit deckt auch das Umschalten
            // der Verstaerkung und den Nachhall ab. Wurde nicht
            // hineingesprochen, wird danach ohne Weckwort weiter zugehoert,
            // siehe unten.
            wachwort::ruhe();
            nachfrage_offen = !listener.listening();
        }
        antwort_lief = antwort;
        tts.leiser(antwort && listener.listening() && listener.pruefung());

        // Faengt eine Aufnahme an, waehrend der Lautsprecher gerade noch lief,
        // klingt er ins Mikrofon nach. Sonst ist der Wandler vom ersten Block
        // an sauber, und jede abgeschnittene Millisekunde fehlt am ersten
        // Wort. Der Abstand ist grosszuegig: zwischen dem Abbruch der Antwort
        // und dem Anfang der Aufnahme liegen das Zumachen und das Aufmachen
        // des Ports.
        {
            static bool    hoerte       = false;
            static int64_t stimme_bis   = 0;
            if (tts.spricht()) stimme_bis = esp_timer_get_time();

            // Die Nachfrage beginnt erst nach dem Einschwinger, siehe unten.
            // Da ist nichts mehr abzuschneiden, und jeder Schnitt fehlte am
            // ersten Wort.
            const bool hoert = listener.listening();
            if (hoert && !hoerte && !listener.nachfrage()
                && esp_timer_get_time() - stimme_bis < 500000) {
                listener.nachklang_erwarten();
            }
            hoerte = hoert;
        }

        const int64_t t_b = esp_timer_get_time();
        int64_t       t_c = t_b;

        if (!mic.running()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            t_c = esp_timer_get_time();
        } else if (mic.read(block, ref_block, kReadFrames) != ESP_OK) {
            nachtrag::schreiben('W', TAG, "  Lesefehler, naechster Versuch.");
            vTaskDelay(pdMS_TO_TICKS(20));
            t_c = esp_timer_get_time();
        } else {
            t_c = esp_timer_get_time();

            if (antwort) {
                // Waehrend einer Antwort. Die Aufnahme bekommt den gereinigten
                // Ton — sie laeuft hier nur, wenn in die Antwort
                // hineingesprochen wurde, und dann klingt der Lautsprecher
                // noch einen Moment nach.
                const size_t m = echo::verarbeiten(block, ref_block, kReadFrames,
                                                   sauber, kSauberFrames);
                if (listener.listening()) {
                    listener.feed(sauber, m);
                    pruefung_ende = esp_timer_get_time();
                } else if (echo::unterbrochen() && geprueft) {
                    // Die Pruefaufnahme ist vorbei, und die Antwort laeuft
                    // noch. Ihr Text braucht nach dem Ende einen Moment bis
                    // zur Entscheidung (hineingesprochen()); erst danach darf
                    // ein neuer Verdacht eine neue Aufnahme starten.
                    if (esp_timer_get_time() - pruefung_ende > kPruefPauseUs) {
                        echo::weiter();
                        geprueft = false;
                    }
                } else if (echo::unterbrochen()) {
                    // Aufnehmen wie nach einer Antwort, aber als
                    // Pruefaufnahme: die Antwort laeuft leiser weiter, bis
                    // die Erkennung Worte gefunden hat. Siehe echo.h.
                    listener.pruefen(wachwort::ruhepegel());
                    geprueft      = true;
                    pruefung_ende = esp_timer_get_time();

                    // Den Anfang des Satzes nachreichen, in Aufnahmebloecken,
                    // damit Stilleuhr und Lautzaehler dieselben Stuecke sehen
                    // wie sonst.
                    const size_t v = echo::vorspann(vorspann, kVorspannFrames);
                    for (size_t i = 0; i < v && listener.listening(); i += kReadFrames) {
                        const size_t k = (v - i < (size_t)kReadFrames) ? v - i
                                                                       : (size_t)kReadFrames;
                        listener.feed(vorspann + i, k);
                    }
                }
            } else {
                listener.feed(block, kReadFrames);

                // Waehrend einer Aufnahme hoert das Weckwort nicht mit: wer
                // schon spricht, muss nicht geweckt werden — und "HoiHoi"
                // mitten in der Frage soll die laufende Aufnahme nicht von
                // vorn beginnen.
                //
                // Der Ruhepegel, den es dabei nachfuehrt, gilt weiter: er
                // stammt aus der Zeit unmittelbar vor dem Weckwort, und das
                // ist die letzte, in der im Raum nachweislich niemand
                // gesprochen hat.
                if (!kWeckwort) {
                    // Ohne Weckwort: nach einer Antwort (oder ihrem Abbruch per
                    // Taste) wird noch kurz zugehoert, sobald der Lautsprecher
                    // eine halbe Sekunde still ist und nicht mehr nachklingt
                    if (nachfrage_offen && !listener.listening()
                        && esp_timer_get_time() - stimme_zuletzt > 500000) {
                        nachfrage_offen = false;
                        listener.nachfragen(wachwort::ruhepegel());
                    }
                } else if (mikro_stumm) {
                    // Stumm: nichts weckt, nichts fragt nach.
                    nachfrage_offen = false;
                } else if (!listener.listening()) {
                    wachwort::feed(block, kReadFrames, kSampleRate);
                    if (wachwort::geweckt()) {
                        nachfrage_offen = false;
                        listener.wecken(wachwort::ruhepegel());
                    } else if (nachfrage_offen && wachwort::eingeschwungen()) {
                        // Die Antwort ist vorbei und der Wandler wieder
                        // ruhig: zuhoeren, als waere das Weckwort gefallen.
                        // Kommt in kNachfrageMs kein Wort, geht es zurueck
                        // zum Weckwort.
                        nachfrage_offen = false;
                        listener.nachfragen(wachwort::ruhepegel());
                    }
                }
            }

            for (int k = 0; k < kReadFrames; k++) {
                const int16_t s = block[k];
                const int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
                if (a > window_pk) window_pk = a;
                sum_sq += (int64_t)s * s;
                sum_count++;
            }

            rms = (sum_count > 0) ? (int32_t)sqrt((double)(sum_sq / sum_count)) : 0;

            // Pegel dieses Blocks fuer die Welle: sofort hinauf, und je Block
            // um ein Fuenftel hinab, also in etwa 200 ms zurueck.
            {
                int64_t q = 0;
                for (int k = 0; k < kReadFrames; k++) q += (int64_t)block[k] * block[k];
                const int32_t blk = (int32_t)sqrt((double)(q / kReadFrames));
                const int32_t alt = mic_pegel * 4 / 5;
                mic_pegel = blk > alt ? blk : alt;
            }
        }

        // Wer diesen Task verdraengt hat, waehrend er nicht lief.
        {
            static int64_t letzte_runde = 0;
            const int64_t  jetzt_us     = esp_timer_get_time();
            if (letzte_runde != 0 && jetzt_us - letzte_runde > 150000) {
                stau_melden((int)((jetzt_us - letzte_runde) / 1000),
                            (int)((t_b - t_a) / 1000),
                            (int)((t_c - t_b) / 1000),
                            (int)((jetzt_us - t_c) / 1000));
            }
            letzte_runde = jetzt_us;
            stau_schnappschuss();
        }

        // Einmal pro Sekunde eine Zeile ins Log, mit Tasten und Batterie.
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            last_log = now;
            nachtrag::schreiben('I', TAG,
                     "Pegel rms=%5d (%.1f dBFS)  peak=%5d  |  "
                     "MIK=%s BOOT=%s KEY=%s  Batterie=%d",
                     (int)rms, 20.0f * log10f(((float)rms + 1.0f) / 32768.0f),
                     (int)window_pk,
                     mic.running() ? "AN" : "aus",
                     gpio_get_level(BOOT_BUTTON_PIN) ? "offen" : "GEDRUECKT",
                     gpio_get_level(KEY_BUTTON_PIN) ? "offen" : "GEDRUECKT",
                     (int)battery_raw);
            sum_sq       = 0;
            sum_count    = 0;
            window_pk    = 0;
        }
    }
}

// --- app_main -------------------------------------------------------------

extern "C" void app_main(void)
{
    // Das Log stand frueher auch auf dem Display. Es ist dort wieder
    // verschwunden, und zwar nicht aus Platzgruenden: es beantwortete die
    // falsche Frage. Wer vor dem Geraet steht und es zum Sprechen bringen
    // will, sucht nicht nach der letzten Meldung, sondern nach dem naechsten
    // Handgriff — und muss dabei sehen, ob er ueberhaupt gehoert wird. Das
    // Log bleibt der seriellen Schnittstelle, wo es hingehoert und wo es
    // rueckwaerts lesbar ist.
    ESP_LOGI(TAG, "===== ESP32-S3-RLCD-4.2 Bring-up =====");

    // NVS zuerst: der WLAN-Treiber legt dort seine Kalibrierdaten ab und
    // verweigert sonst den Start. Ist die Partition aus einer aelteren
    // Firmware belegt oder voll, hilft nur loeschen — die Daten darin sind
    // ohnehin nur Zwischenstand, kein Zustand, den jemand vermisst.
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nerr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nerr);

    // WLAN frueh und nebenlaeufig: bis der Bring-up durch ist, steht die
    // Verbindung meist schon. Ist kein Netz bekannt oder keines erreichbar,
    // geht stattdessen die Bereitstellung ueber Bluetooth auf — Geraetename
    // und Nachweis fuer die App stehen dann im Log, siehe prov.cpp.
    cfg::begin();
    net::begin();
    wetter::begin(nullptr);   // Ort, Gelaende fuer die Landschaft, Wetter

    // Die Uhr der Oberflaeche. Der Zeitserver wird gefragt, sobald das Netz
    // steht, danach stuendlich; bis zur ersten Antwort zeigt sie "--:--".
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    {
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp.sync_cb = [](struct timeval *) { ESP_LOGI(TAG, "Uhrzeit vom Zeitserver gestellt."); };
        if (esp_netif_sntp_init(&sntp) != ESP_OK) ESP_LOGW(TAG, "Zeitserver nicht erreichbar, die Uhr bleibt leer.");
    }

    report_chip();
    init_i2c();
    scan_i2c();
    read_shtc3();
    test_display();
    init_inputs();

    // Laeuft die Aufnahme in ihrem eigenen Task, hat der Haupttask nichts
    // mehr zu tun. Er darf zurueckkehren; die IDF raeumt ihn dann samt
    // seinen acht Kilobyte Stack ab.
    if (visualize_mic()) return;

    // Nur erreichbar, wenn das Mikrofon nicht ansprechbar war.
    ESP_LOGI(TAG, "--- Laufende Ueberwachung (Tasten + Batterie) ---");

    while (true) {
        int raw = 0;
        adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw);

        ESP_LOGI(TAG, "BOOT=%s  KEY=%s  Batterie-ADC roh=%d",
                 gpio_get_level(BOOT_BUTTON_PIN) ? "offen" : "GEDRUECKT",
                 gpio_get_level(KEY_BUTTON_PIN)  ? "offen" : "GEDRUECKT",
                 raw);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
