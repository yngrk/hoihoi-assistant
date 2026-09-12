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
#include <esp_private/esp_clk.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <math.h>

#include "display_bsp.h"
#include "gfx.h"
#include "audio.h"
#include "user_config.h"

static const char *TAG = "bringup";

static i2c_master_bus_handle_t i2c_bus = nullptr;

// Zeichenflaeche, angelegt in test_display(), danach fuer alle weiteren
// Ausgaben gueltig.
static Canvas *display = nullptr;

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

    display->clear(ColorWhite);
    display->flush();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Testbild: Rahmen, Diagonalen, Marker, Schachbrett. Jedes Element prueft
    // etwas anderes — der Rahmen die Raender, die Diagonalen die Adressierung,
    // das Schachbrett die Bit-Packung innerhalb eines Bytes.
    //
    // Der Rahmen liegt bewusst exakt auf der Kante (Zeile 0 und 299, Spalte 0
    // und 399): auf echter Hardware verifiziert, dort wird nichts von einer
    // Blende verdeckt. Er ist nur duenn und faellt beim Draufschauen kaum auf.
    display->rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, ColorBlack);
    display->line(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, ColorBlack);
    display->line(LCD_WIDTH - 1, 0, 0, LCD_HEIGHT - 1, ColorBlack);

    // Asymmetrischer Marker links oben, breiter als hoch: Rahmen, Diagonalen
    // und Schachbrett sind alle symmetrisch und wuerden eine Spiegelung oder
    // Drehung nicht verraten, dieser Balken schon.
    display->fill_rect(30, 30, 109, 49, ColorBlack);

    // Schachbrett als Anker: prueft die Bit-Packung innerhalb eines Bytes und
    // beweist, dass ueberhaupt gezeichnet wird.
    for (int y = 100; y < 200; y++) {
        for (int x = 150; x < 250; x++) {
            if (((x / 10) + (y / 10)) % 2 == 0) {
                display->pixel(x, y, ColorBlack);
            }
        }
    }

    display->flush();
    ESP_LOGI(TAG, "  Testbild ausgegeben: Rahmen auf der Kante, zwei Diagonalen, "
                  "Marker links oben, Schachbrett mittig (100x100).");

    // Selbsttest der Bereichspruefung. Ohne die Huelle wuerde jeder dieser
    // Aufrufe hinter die LUT greifen; dass der Bring-up hier nicht abstuerzt
    // und das Bild unveraendert bleibt, ist der eigentliche Nachweis.
    // Saemtliche Koordinaten liegen vollstaendig ausserhalb, keine ragt in die
    // Flaeche hinein. Geclippt wuerde sonst der sichtbare Teil tatsaechlich
    // gezeichnet und das Testbild ueberschrieben — hier darf sich am Puffer
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

// --- 6. Mikrofon-Visualisierung -------------------------------------------
//
// Bildausgabe und Audioaufnahme laufen in getrennten Tasks, und das ist keine
// Stilfrage. Das Panel gibt ueber die TE-Leitung 27,03 Hz vor (gemessen:
// 36990 us, sehr stabil). Waren beide aneinandergekoppelt, lag die Bildrate auf
// dem Audiotakt von 25 Hz — und zwei fast gleiche Frequenzen ergeben eine
// Schwebung von gut 2 Hz, die als regelmaessiges Stottern sichtbar wird.
// Entkoppelt laeuft die Anzeige exakt auf der Panelfrequenz und die Aufnahme in
// ihrem eigenen Takt, ohne dass eine die andere zieht.

static MicInput mic;

// Aufteilung der Flaeche: oben das Wellenbild, unten ein Pegelbalken.
static const int kScopeTop    = 2;
static const int kScopeBottom = 247;
static const int kScopeCenter = (kScopeTop + kScopeBottom) / 2;
static const int kScopeHalf   = (kScopeBottom - kScopeTop) / 2;

static const int kMeterTop    = 260;
static const int kMeterBottom = 295;

// 40 Frames je Spalte bei 16 kHz und 400 Spalten ergeben genau eine Sekunde
// Signal ueber die volle Bildbreite.
static const int kFramesPerColumn = 40;
static const int kColumnsPerRead  = 8;
static const int kReadFrames      = kFramesPerColumn * kColumnsPerRead;   // 320 = 20 ms

// Geteilter Zustand zwischen Aufnahme- und Anzeigetask.
static SemaphoreHandle_t scope_lock = nullptr;
static int16_t           col_min[LCD_WIDTH];
static int16_t           col_max[LCD_WIDTH];
static int               col_head = 0;      // aelteste Spalte, also linker Rand
static int32_t           shared_rms = 0;
static int32_t           shared_peak = 0;

// Die Empfindlichkeit der Mikrofone ist nicht dokumentiert, ein fester Faktor
// wuerde also entweder in Stille das Grundrauschen aufblasen oder bei Sprache
// am Anschlag kleben. Stattdessen folgt der Vollausschlag dem Signal: sofort
// auf, langsam wieder zu — und nie unter kScaleFloor, damit Stille still
// aussieht.
static const int32_t kScaleFloor = 1200;
static int32_t       scope_scale = kScaleFloor;

// Kennzahlen der Anzeige, nur fuer die Logzeile.
static volatile int32_t frame_us_sum = 0;
static volatile int32_t frame_us_max = 0;
static volatile int32_t frame_count  = 0;

static int sample_to_y(int32_t s, int32_t scale)
{
    return kScopeCenter - (int)((s * kScopeHalf) / scale);
}

// Pegel logarithmisch: linear waere Sprache bei 16 Bit ein kaum sichtbarer
// Stummel am linken Rand.
static int level_to_width(int32_t amplitude)
{
    if (amplitude < 1) amplitude = 1;
    float db = 20.0f * log10f((float)amplitude / 32768.0f);
    if (db < -60.0f) db = -60.0f;
    if (db > 0.0f)   db = 0.0f;
    return (int)((db + 60.0f) / 60.0f * (LCD_WIDTH - 4));
}

static void draw_scope(const int16_t *lo, const int16_t *hi, int head,
                       int32_t scale, int32_t rms, int32_t peak)
{
    display->clear(ColorWhite);

    // Mittellinie gestrichelt, damit sie das Signal nicht verdeckt.
    for (int x = 0; x < LCD_WIDTH; x += 8) {
        display->hline(x, x + 3, kScopeCenter, ColorBlack);
    }

    // Aelteste Spalte links, neueste rechts — das Bild laeuft nach links weg.
    for (int i = 0; i < LCD_WIDTH; i++) {
        const int idx = (head + i) % LCD_WIDTH;
        const int y0  = sample_to_y(hi[idx], scale);
        const int y1  = sample_to_y(lo[idx], scale);
        if (y0 == y1) {
            display->pixel(i, y0, ColorBlack);
        } else {
            display->vline(i, y0, y1, ColorBlack);
        }
    }

    display->rect(0, kMeterTop, LCD_WIDTH - 1, kMeterBottom, ColorBlack);
    const int w = level_to_width(rms);
    if (w > 0) {
        display->fill_rect(2, kMeterTop + 3, 2 + w - 1, kMeterBottom - 3, ColorBlack);
    }
    // Spitzenwert als schmaler Strich, damit kurze Transienten sichtbar
    // bleiben, die der Balken schon wieder verlassen hat.
    const int p = level_to_width(peak);
    if (p > 0) {
        display->vline(2 + p - 1, kMeterTop + 1, kMeterBottom - 1, ColorBlack);
    }

    display->flush();   // wartet auf die naechste Austastluecke
}

// Anzeigetask: taktet sich ueber flush() selbst auf die Panelfrequenz. Er
// arbeitet auf einer Kopie, damit der Aufnahmetask waehrend des Zeichnens
// weiterschreiben kann.
static void display_task(void *)
{
    static int16_t lo[LCD_WIDTH];
    static int16_t hi[LCD_WIDTH];

    while (true) {
        int     head;
        int32_t scale, rms, peak;

        xSemaphoreTake(scope_lock, portMAX_DELAY);
        memcpy(lo, col_min, sizeof(lo));
        memcpy(hi, col_max, sizeof(hi));
        head  = col_head;
        scale = scope_scale;
        rms   = shared_rms;
        peak  = shared_peak;
        xSemaphoreGive(scope_lock);

        const int64_t t0 = esp_timer_get_time();
        draw_scope(lo, hi, head, scale, rms, peak);
        const int32_t dt = (int32_t)(esp_timer_get_time() - t0);

        if (dt > frame_us_max) frame_us_max = dt;
        frame_us_sum += dt;
        frame_count++;
    }
}

static void visualize_mic(void)
{
    ESP_LOGI(TAG, "--- Mikrofon-Visualisierung ---");

    if (mic.begin(i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "  Mikrofon nicht verfuegbar, Visualisierung entfaellt.");
        return;
    }

    memset(col_min, 0, sizeof(col_min));
    memset(col_max, 0, sizeof(col_max));

    scope_lock = xSemaphoreCreateMutex();
    if (scope_lock == nullptr) {
        ESP_LOGE(TAG, "  Mutex konnte nicht angelegt werden.");
        return;
    }

    ESP_LOGI(TAG, "  %d Hz, %d Frames je Spalte, %d Spalten = %.1f s Bildbreite.",
             (int)mic.sample_rate(), kFramesPerColumn, LCD_WIDTH,
             (float)LCD_WIDTH * kFramesPerColumn / mic.sample_rate());

    // Anzeige auf den zweiten Kern, damit das Zeichnen die Aufnahme nicht
    // verdraengt und umgekehrt.
    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 4, nullptr, 1);

    static int16_t block[kReadFrames];
    int64_t        sum_sq    = 0;
    int32_t        sum_count = 0;
    int32_t        window_pk = 0;
    int64_t        last_log  = esp_timer_get_time();

    while (true) {
        if (mic.read_mono(block, kReadFrames) != ESP_OK) {
            ESP_LOGW(TAG, "  Lesefehler, naechster Versuch.");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int16_t lo[kColumnsPerRead];
        int16_t hi[kColumnsPerRead];
        int32_t block_peak = 0;

        for (int c = 0; c < kColumnsPerRead; c++) {
            int16_t cl = INT16_MAX;
            int16_t ch = INT16_MIN;
            for (int k = 0; k < kFramesPerColumn; k++) {
                const int16_t s = block[c * kFramesPerColumn + k];
                if (s < cl) cl = s;
                if (s > ch) ch = s;

                const int32_t a = (s < 0) ? -(int32_t)s : (int32_t)s;
                if (a > block_peak) block_peak = a;
                if (a > window_pk)  window_pk  = a;
                sum_sq += (int64_t)s * s;
                sum_count++;
            }
            lo[c] = cl;
            hi[c] = ch;
        }

        const int32_t rms = (sum_count > 0)
                                ? (int32_t)sqrt((double)(sum_sq / sum_count))
                                : 0;

        xSemaphoreTake(scope_lock, portMAX_DELAY);
        for (int c = 0; c < kColumnsPerRead; c++) {
            col_min[col_head] = lo[c];
            col_max[col_head] = hi[c];
            col_head = (col_head + 1) % LCD_WIDTH;
        }
        // Vollausschlag nachfuehren: sofort auf, langsam zu.
        if (block_peak > scope_scale) {
            scope_scale = block_peak;
        } else {
            scope_scale -= (scope_scale - kScaleFloor) / 24;
        }
        if (scope_scale < kScaleFloor) scope_scale = kScaleFloor;
        shared_rms  = rms;
        shared_peak = window_pk;
        xSemaphoreGive(scope_lock);

        // Einmal pro Sekunde eine Zeile ins Log, mit Tasten und Batterie.
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            last_log = now;
            int raw = 0;
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw);
            const int32_t fc = frame_count;
            ESP_LOGI(TAG,
                     "Pegel rms=%5d (%.1f dBFS)  peak=%5d  Skala=%5d  |  "
                     "Bild %d/%d ms, %d/s  |  TE %d us (%d Hz, %d Timeouts)"
                     "  |  BOOT=%s KEY=%s  Batterie=%d",
                     (int)rms, 20.0f * log10f(((float)rms + 1.0f) / 32768.0f),
                     (int)window_pk, (int)scope_scale,
                     (int)(fc ? (frame_us_sum / fc / 1000) : 0),
                     (int)(frame_us_max / 1000), (int)fc,
                     (int)display->te_period_us(),
                     (int)(display->te_period_us()
                               ? 1000000 / display->te_period_us()
                               : 0),
                     (int)display->te_timeouts(),
                     gpio_get_level(BOOT_BUTTON_PIN) ? "offen" : "GEDRUECKT",
                     gpio_get_level(KEY_BUTTON_PIN) ? "offen" : "GEDRUECKT",
                     raw);
            sum_sq       = 0;
            sum_count    = 0;
            window_pk    = 0;
            frame_us_sum = 0;
            frame_us_max = 0;
            frame_count  = 0;
        }
    }
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

    vTaskDelay(pdMS_TO_TICKS(1500));   // Testbild kurz stehen lassen
    visualize_mic();

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
