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
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>

#include "display_bsp.h"
#include "user_config.h"

static const char *TAG = "bringup";

static i2c_master_bus_handle_t i2c_bus = nullptr;

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

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "--- Chip ---");
    ESP_LOGI(TAG, "Kerne      : %d", info.cores);
    ESP_LOGI(TAG, "Revision   : %d", info.revision);
    ESP_LOGI(TAG, "Flash      : %" PRIu32 " MB", flash_size / (1024 * 1024));

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

static void read_shtc3(void)
{
    ESP_LOGI(TAG, "--- SHTC3 ---");

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = 0x70;
    dev_cfg.scl_speed_hz    = 100000;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "  Geraet konnte nicht angelegt werden.");
        return;
    }

    const uint8_t cmd_wakeup[]  = {0x35, 0x17};
    const uint8_t cmd_measure[] = {0x78, 0x66};  // Normalmodus, Temperatur zuerst
    const uint8_t cmd_sleep[]   = {0xB0, 0x98};

    if (i2c_master_transmit(dev, cmd_wakeup, sizeof(cmd_wakeup), 100) != ESP_OK) {
        ESP_LOGE(TAG, "  Wakeup fehlgeschlagen.");
        i2c_master_bus_rm_device(dev);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    if (i2c_master_transmit(dev, cmd_measure, sizeof(cmd_measure), 100) != ESP_OK) {
        ESP_LOGE(TAG, "  Messbefehl fehlgeschlagen.");
        i2c_master_bus_rm_device(dev);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t raw[6] = {0};
    if (i2c_master_receive(dev, raw, sizeof(raw), 100) == ESP_OK) {
        uint16_t t_raw = (raw[0] << 8) | raw[1];
        uint16_t h_raw = (raw[3] << 8) | raw[4];
        float temperature = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
        float humidity    = 100.0f * ((float)h_raw / 65535.0f);
        ESP_LOGI(TAG, "  Temperatur : %.2f C", temperature);
        ESP_LOGI(TAG, "  Feuchte    : %.2f %%rH", humidity);
    } else {
        ESP_LOGE(TAG, "  Messwerte konnten nicht gelesen werden.");
    }

    i2c_master_transmit(dev, cmd_sleep, sizeof(cmd_sleep), 100);
    i2c_master_bus_rm_device(dev);
}

// --- 4. Display -----------------------------------------------------------

static void test_display(void)
{
    ESP_LOGI(TAG, "--- Display (ST7305, %dx%d) ---", LCD_WIDTH, LCD_HEIGHT);

    // Bewusst erst hier instanziiert, nicht als globales Objekt: der Treiber
    // allokiert Puffer, und zum Zeitpunkt globaler Konstruktoren laesst sich
    // ueber die Heap-Situation weniger sicher urteilen als hier in app_main.
    static DisplayPort rlcd(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                            RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

    rlcd.RLCD_Init();
    rlcd.RLCD_ColorClear(ColorWhite);
    rlcd.RLCD_Display();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Testbild: Rahmen, Diagonalen, Schachbrett. Jedes Element prueft etwas
    // anderes — der Rahmen die Raender, die Diagonalen die Adressierung,
    // das Schachbrett die Bit-Packung innerhalb eines Bytes.
    for (int x = 0; x < LCD_WIDTH; x++) {
        rlcd.RLCD_SetPixel(x, 0, ColorBlack);
        rlcd.RLCD_SetPixel(x, LCD_HEIGHT - 1, ColorBlack);
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        rlcd.RLCD_SetPixel(0, y, ColorBlack);
        rlcd.RLCD_SetPixel(LCD_WIDTH - 1, y, ColorBlack);
    }
    for (int i = 0; i < LCD_HEIGHT; i++) {
        int x = i * LCD_WIDTH / LCD_HEIGHT;
        rlcd.RLCD_SetPixel(x, i, ColorBlack);
        rlcd.RLCD_SetPixel(LCD_WIDTH - 1 - x, i, ColorBlack);
    }
    for (int y = 100; y < 200; y++) {
        for (int x = 150; x < 250; x++) {
            if (((x / 10) + (y / 10)) % 2 == 0) {
                rlcd.RLCD_SetPixel(x, y, ColorBlack);
            }
        }
    }

    rlcd.RLCD_Display();
    ESP_LOGI(TAG, "  Testbild ausgegeben: Rahmen, zwei Diagonalen, Schachbrett mittig.");
}

// --- 5. Tasten und Batterie ----------------------------------------------

static adc_oneshot_unit_handle_t adc_handle = nullptr;

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
}

// --- app_main -------------------------------------------------------------

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "===== ESP32-S3-RLCD-4.2 Bring-up =====");

    report_chip();
    init_i2c();
    scan_i2c();
    read_shtc3();
    test_display();
    init_inputs();

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
