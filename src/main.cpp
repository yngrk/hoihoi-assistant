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
#include <nvs_flash.h>
#include <math.h>

#include "display_bsp.h"
#include "display_sync.h"
#include "gfx.h"
#include "font5x7.h"
#include "audio.h"
#include "listen.h"
#include "logview.h"
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
#define CHAT_MODEL "gpt-4o-mini"
#endif
#ifndef TTS_MODEL
#define TTS_MODEL "gpt-4o-mini-tts"
#endif
#ifndef TTS_VOICE
#define TTS_VOICE "alloy"
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

static MicInput      mic;
static SpeakerOutput speaker;

// Aufteilung der Flaeche nach dem Entwurf: drei gestapelte Baender ueber die
// volle Breite — Kennzahlen oben, Wellenbild in der Mitte, Pegelskala unten.
// Die Anteile stammen aus dem Figma-Frame (76 / 19 / 4 Prozent bei 4:3) und
// sind hier auf ganze Pixel gelegt; zwischen den Baendern liegt je eine
// Trennlinie.
static const int kStatsTop    = 0;
static const int kStatsBottom = 227;

static const int kScopeTop    = 230;
static const int kScopeBottom = 286;
static const int kScopeCenter = (kScopeTop + kScopeBottom) / 2;
static const int kScopeHalf   = (kScopeBottom - kScopeTop) / 2;

static const int kMeterTop    = 290;
static const int kMeterBottom = 299;

// Innerhalb des Stats-Bandes: Kopfzeile, dann zwei Spalten.
static const int kHeaderBottom = 15;
static const int kColLeftX     = 8;
static const int kDividerX     = 200;
static const int kColRightX    = 210;

// Waehrend einer Aufnahme belegt dasselbe Band eine andere Einteilung:
// Aufnahmezeichen und Zeitbalken oben, darunter der Text, der gerade
// erkannt wird. Kennzahlen treten so lange zurueck — wer spricht, schaut
// auf den Text und nicht auf die Batteriespannung.
static const int kDotCX      = kColLeftX + 11;
static const int kDotCY      = 34;
static const int kDotR       = 11;
static const int kBarX0      = kDotCX + kDotR + 10;
static const int kBarX1      = LCD_WIDTH - kColLeftX - 1;
static const int kBarY0      = 25;
static const int kBarY1      = 43;
static const int kPhaseY     = 54;
static const int kTextY      = 74;
static const int kTextScale  = 2;
static const int kTextStep   = 18;
static const int kTextLines  = (kStatsBottom - kTextY) / kTextStep;   // 8

// So lange bleibt das Ergebnis nach dem Loslassen stehen, bevor die Anzeige
// zu den Kennzahlen zurueckkehrt. Kuerzer waere der erkannte Satz weg, bevor
// er gelesen ist.
static const int64_t kResultHoldUs = 8 * 1000000;

// Die Logansicht nimmt das ganze Bild. Das Wellenbild zeigt ohne laufendes
// Mikrofon nur die Nulllinie, und das Mikrofon laeuft nur bei gedrueckter
// Taste — waehrend einer Aufnahme uebernimmt aber ohnehin die
// Aufnahmeansicht. Im Ruhezustand ist unter der Kopfzeile also nichts, was
// dem Log den Platz streitig machen koennte.
static const int kLogX    = 4;
static const int kLogTop  = kHeaderBottom + 5;
static const int kLogStep = 10;                 // 7 Pixel Schrift, 3 Luft
static const int kLogCols = (LCD_WIDTH - 2 * kLogX) / kFontAdvance;   // 65
static const int kLogRows = (LCD_HEIGHT - kLogTop) / kLogStep;

// Mehr Zeilen als Reihen braucht niemand zu holen: jede Zeile belegt
// mindestens eine Reihe.
static const int kLogFetch = (kLogRows < logview::kLines) ? kLogRows
                                                          : logview::kLines;

// Welche der beiden Ruheansichten gilt. Das Log steht vorn, weil es die
// Frage beantwortet, die man vor dem Geraet tatsaechlich hat: was macht es
// gerade? Die Kennzahlen sind einen Tastendruck entfernt.
static volatile int log_ansicht = 1;

// Antwortansicht: nimmt wie das Log das ganze Bild. Das Wellenbild zeigt
// waehrenddessen nur die Nulllinie — das Mikrofon ist laengst wieder zu —,
// und die Antwort ist das, was jetzt zaehlt.
static const int kFrageY     = kPhaseY + 20;
static const int kFrageStep  = 10;
static const int kFrageLines = 3;
static const int kAntwortY   = kFrageY + kFrageStep * kFrageLines + 8;
static const int kAntwortLines = (LCD_HEIGHT - kAntwortY) / kTextStep;   // 10

// 24 kHz, nicht 16: die Realtime-API bekommt den Ton so, wie er aufgenommen
// wurde, und 24 kHz ist dort die Rate, auf die alles ausgelegt ist.
// Umrechnen auf dem Geraet waere zusaetzlicher Code an einer Stelle, an der
// ein Fehler nur als schlechtere Erkennung auffiele.
static const uint32_t kSampleRate = 24000;

// 60 Frames je Spalte bei 24 kHz und 400 Spalten ergeben genau eine Sekunde
// Signal ueber die volle Bildbreite.
static const int kFramesPerColumn = 60;
static const int kColumnsPerRead  = 8;
static const int kReadFrames      = kFramesPerColumn * kColumnsPerRead;   // 480 = 20 ms

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

// Kennzahlen der Anzeige, fuer die Logzeile und das Stats-Band.
static volatile int32_t frame_us_sum = 0;
static volatile int32_t frame_us_max = 0;
static volatile int32_t frame_count  = 0;
static volatile int32_t frames_per_s = 0;

// Umwelt- und Systemwerte fuer das Stats-Band. Bewusst ohne Mutex: es sind
// ausgerichtete 32-Bit-Worte, die ein Schreiber unteilbar setzt und ein Leser
// unteilbar liest. Ein halb geschriebener Wert kann hier also nicht entstehen,
// und ob die Anzeige einen Messwert ein Bild spaeter uebernimmt, spielt bei
// zwei Sekunden Messabstand keine Rolle.
static volatile int32_t env_temp_c100 = 0;
static volatile int32_t env_hum_100   = 0;
static volatile int32_t env_valid     = 0;
static volatile int32_t battery_raw   = 0;

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

// Festkommaausgabe mit einer Nachkommastelle. Ueber snprintf("%.1f") ginge es
// auch, das zoege aber die Gleitkomma-Formatierung der libc in jedes Bild.
static void fmt_tenths(char *buf, size_t n, int32_t v100, const char *unit)
{
    const bool    neg = (v100 < 0);
    const int32_t a   = neg ? -v100 : v100;
    snprintf(buf, n, "%s%d.%d%s", neg ? "-" : "",
             (int)(a / 100), (int)((a / 10) % 10), unit);
}

// Beschriftung klein darueber, Wert gross darunter — bei einem Bit je Pixel
// traegt die Groesse die Hierarchie, weil Graustufen dafuer fehlen.
static void draw_field(int x, int y, const char *label, const char *value,
                       int value_scale)
{
    display->text(x, y, label, ColorBlack, 1);
    display->text(x, y + 12, value, ColorBlack, value_scale);
}

// Kopfzeile invers: der einzige Weg, auf dieser Anzeige etwas hervorzuheben,
// ohne Flaeche zu verschwenden. Links immer der Name, rechts der Zustand.
static void draw_header(const char *links, const char *rechts)
{
    display->fill_rect(0, kStatsTop, LCD_WIDTH - 1, kHeaderBottom, ColorBlack);
    display->text(6, kStatsTop + 5, links, ColorWhite, 1);
    display->text(LCD_WIDTH - 6 - Canvas::text_width(rechts, 1), kStatsTop + 5,
                  rechts, ColorWhite, 1);
}

// Rechts in der Kopfzeile steht, was einem Tastendruck im Weg stehen
// koennte. "BEREIT" allein waere eine Behauptung, die ohne WLAN oder ohne
// Schluessel nicht stimmt. Beide Ruheansichten zeigen dieselbe Zeile — der
// Zustand des Geraets haengt nicht davon ab, wohin man gerade schaut.
static void status_text(char *buf, size_t n)
{
    if (net::provisioning()) {
        snprintf(buf, n, "WLAN EINRICHTEN");
    } else if (stt.phase() == Stt::Phase::Bereit) {
        snprintf(buf, n, "BEREIT - KEY HALTEN");
    } else if (stt.phase() == Stt::Phase::Fehler) {
        snprintf(buf, n, "ERKENNUNG GESTOERT");
    } else {
        snprintf(buf, n, "%s", net::status());
    }
}

// Logansicht: die juengsten Meldungen, neueste unten. Lange Zeilen laufen in
// der naechsten Reihe weiter statt abgeschnitten zu werden — gerade bei
// Fehlermeldungen steht das Entscheidende oft hinten.
static void draw_log(void)
{
    static char zeilen[kLogFetch][logview::kCols + 1];

    const int n = logview::snapshot(&zeilen[0][0], kLogFetch);

    // Von hinten her so viele Zeilen nehmen, wie in die Flaeche passen. Die
    // neueste Meldung ist die wichtigste und muss in jedem Fall aufs Bild,
    // deshalb wird rueckwaerts gezaehlt und nicht vorwaerts gerechnet.
    int erste  = n;
    int reihen = 0;
    while (erste > 0) {
        const int len = (int)strlen(zeilen[erste - 1]);
        int       r   = (len + kLogCols - 1) / kLogCols;
        if (r < 1) r = 1;
        if (reihen + r > kLogRows) break;
        reihen += r;
        erste--;
    }

    char stueck[kLogCols + 1];
    int  y = kLogTop;

    for (int i = erste; i < n; i++) {
        const char *quelle = zeilen[i];
        const int   len    = (int)strlen(quelle);

        for (int off = 0; off == 0 || off < len; off += kLogCols) {
            int m = len - off;
            if (m > kLogCols) m = kLogCols;
            if (m < 0) m = 0;
            memcpy(stueck, quelle + off, (size_t)m);
            stueck[m] = ' ';

            display->text(kLogX, y, stueck, ColorBlack, 1);
            y += kLogStep;
        }
    }
}

static void draw_stats(int32_t rms)
{
    char buf[32];

    status_text(buf, sizeof(buf));
    draw_header("KENNZAHLEN - BOOT ZEIGT LOG", buf);

    // Waehrend der Bereitstellung zaehlt nur eines: welches Geraet die App
    // suchen soll und welchen Nachweis sie verlangt. Temperatur und Bildrate
    // haben in diesem Moment niemanden, der sie braucht.
    if (net::provisioning()) {
        draw_field(kColLeftX, 40, "IN DER APP SUCHEN NACH", prov::device_name(), 3);
        draw_field(kColLeftX, 104, "KENNWORTNACHWEIS", prov::pop(), 3);
        display->text_wrapped(kColLeftX, 168, LCD_WIDTH - 2 * kColLeftX,
                              "App: ESP BLE Provisioning von Espressif. "
                              "Sobald ein Netz angenommen ist, geht es normal "
                              "weiter - ein Neustart ist nicht noetig.",
                              ColorBlack, 2, 18, 3);
        return;
    }

    display->vline(kDividerX, kHeaderBottom + 7, kStatsBottom - 6, ColorBlack);

    // --- Linke Spalte: Umweltwerte, gross ---
    if (env_valid) {
        fmt_tenths(buf, sizeof(buf), env_temp_c100, "\x7F");   // Gradzeichen
        draw_field(kColLeftX, 24, "TEMPERATUR", buf, 4);
        fmt_tenths(buf, sizeof(buf), env_hum_100, "%");
        draw_field(kColLeftX, 76, "FEUCHTE", buf, 4);
    } else {
        draw_field(kColLeftX, 24, "TEMPERATUR", "--", 4);
        draw_field(kColLeftX, 76, "FEUCHTE", "--", 4);
    }

    // Pegel in dBFS gehoert fachlich zum Ton, steht aber als Zahl hier oben,
    // weil das Wellenband dafuer keinen Platz hat.
    if (mic.running()) {
        const int32_t amp = (rms < 1) ? 1 : rms;
        snprintf(buf, sizeof(buf), "%ddB",
                 (int)(20.0f * log10f((float)amp / 32768.0f)));
    } else {
        // Kein Messwert, weil der Wandler zu ist. "-90 dB" zu zeigen waere
        // eine Zahl, die Stille behauptet, wo gar nicht gemessen wird.
        snprintf(buf, sizeof(buf), "AUS");
    }
    draw_field(kColLeftX, 128, "PEGEL", buf, 4);

    // Ergebnis des letzten Tastendrucks. Ohne diese Rueckmeldung waere nach
    // dem Loslassen nicht zu sehen, ob ueberhaupt etwas angekommen ist.
    if (listener.last_ms() > 0) {
        const int32_t pk = (listener.last_peak() < 1) ? 1 : listener.last_peak();
        const int32_t ef = (listener.last_rms() < 1) ? 1 : listener.last_rms();
        snprintf(buf, sizeof(buf), "%d.%ds  %d/%ddB",
                 (int)(listener.last_ms() / 1000),
                 (int)((listener.last_ms() / 100) % 10),
                 (int)(20.0f * log10f((float)pk / 32768.0f)),
                 (int)(20.0f * log10f((float)ef / 32768.0f)));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    draw_field(kColLeftX, 180, "LETZTE AUFNAHME  SPITZE/EFFEKTIV", buf, 2);

    // --- Rechte Spalte: Systemzustand, klein ---
    const int64_t up = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%d:%02d:%02d",
             (int)(up / 3600), (int)((up / 60) % 60), (int)(up % 60));
    draw_field(kColRightX, 24, "LAUFZEIT", buf, 2);

    snprintf(buf, sizeof(buf), "%d/s", (int)frames_per_s);
    draw_field(kColRightX, 60, "BILDRATE", buf, 2);

    const uint32_t te = display->te_period_us();
    snprintf(buf, sizeof(buf), "%d Hz", (int)(te ? 1000000 / te : 0));
    draw_field(kColRightX, 96, "PANEL", buf, 2);

    snprintf(buf, sizeof(buf), "%d", (int)battery_raw);
    draw_field(kColRightX, 132, "BATTERIE (ROH)", buf, 2);

    snprintf(buf, sizeof(buf), "%s %s",
             gpio_get_level(BOOT_BUTTON_PIN) ? "----" : "BOOT",
             gpio_get_level(KEY_BUTTON_PIN) ? "---" : "KEY");
    draw_field(kColRightX, 168, "TASTEN", buf, 2);
}

// Aufnahmeansicht: belegt dasselbe Band wie die Kennzahlen, zeigt aber nur
// dreierlei — dass aufgenommen wird, wie lange noch Platz ist, und was
// bisher verstanden wurde.
static void draw_listening(void)
{
    char buf[48];
    static char text[Stt::kMaxText];   // static: 512 Byte gehoeren nicht auf
                                       // den Stack des Anzeigetasks

    const bool    hoert = listener.listening();
    const int32_t ms    = hoert ? listener.elapsed_ms() : listener.last_ms();
    const int32_t voll  = Listener::kMaxSeconds * 1000;

    snprintf(buf, sizeof(buf), "%d.%d s / %d s",
             (int)(ms / 1000), (int)((ms / 100) % 10), Listener::kMaxSeconds);
    draw_header(hoert ? "AUFNAHME" : "AUFNAHME BEENDET", buf);

    // Aufnahmezeichen: gefuellter Punkt, im Sekundentakt blinkend. Das
    // Blinken ist der Teil, der auch aus dem Augenwinkel ankommt — ein
    // stehender Punkt sieht aus wie ein gedrucktes Symbol.
    if (hoert && ((ms / 400) % 2) == 0) {
        display->fill_circle(kDotCX, kDotCY, kDotR, ColorBlack);
    } else {
        display->circle(kDotCX, kDotCY, kDotR, ColorBlack);
        display->circle(kDotCX, kDotCY, kDotR - 1, ColorBlack);
    }

    display->rect(kBarX0, kBarY0, kBarX1, kBarY1, ColorBlack);

    int32_t   anteil = (ms > voll) ? voll : ms;
    const int innen  = kBarX1 - kBarX0 - 4;
    const int w      = (int)((int64_t)innen * anteil / voll);
    if (w > 0) {
        display->fill_rect(kBarX0 + 2, kBarY0 + 2, kBarX0 + 2 + w - 1,
                           kBarY1 - 2, ColorBlack);
    }
    // Sekundenmarken, damit der Balken eine Skala hat und nicht nur eine
    // Laenge. Sie werden invertiert, wo der Balken schon steht.
    const int marken = Listener::kMaxSeconds;
    for (int s = 1; s < marken; s++) {
        const int x = kBarX0 + 2 + innen * s / marken;
        display->vline(x, kBarY0 + 2, kBarY1 - 2,
                       (x < kBarX0 + 2 + w) ? ColorWhite : ColorBlack);
    }

    // Zustand der Erkennung, klein: das ist die Zeile, an der man sieht, ob
    // ausbleibender Text am Netz liegt oder daran, dass nichts gesagt wurde.
    snprintf(buf, sizeof(buf), "ERKENNUNG: %s", stt.phase_text());
    display->text(kColLeftX, kPhaseY, buf, ColorBlack, 1);
    display->text(LCD_WIDTH - kColLeftX - Canvas::text_width(net::status(), 1),
                  kPhaseY, net::status(), ColorBlack, 1);
    display->hline(kColLeftX, LCD_WIDTH - kColLeftX - 1, kPhaseY + 12, ColorBlack);

    stt.copy_text(text, sizeof(text));
    if (text[0] != '\0') {
        display->text_wrapped(kColLeftX, kTextY,
                              LCD_WIDTH - 2 * kColLeftX, text, ColorBlack,
                              kTextScale, kTextStep, kTextLines);
    } else {
        // Ohne Text nicht einfach leer bleiben: eine leere Flaeche sieht aus
        // wie ein Fehler, auch wenn gerade nur niemand gesprochen hat.
        const char *hinweis;
        switch (stt.phase()) {
            case Stt::Phase::Aus:       hinweis = "OHNE NETZ KEIN TEXT"; break;
            case Stt::Phase::Verbindet: hinweis = "VERBINDET ..."; break;
            case Stt::Phase::Fehler:    hinweis = "ERKENNUNG GESTOERT"; break;
            default:                    hinweis = hoert ? "SPRECHEN ..."
                                                        : "NICHTS VERSTANDEN"; break;
        }
        display->text(kColLeftX, kTextY, hinweis, ColorBlack, kTextScale);
    }
}

// Welche Ansicht das obere Band zeigt. Waehrend und kurz nach einer Aufnahme
// die Aufnahmeansicht, sonst die Kennzahlen.
static bool aufnahme_ansicht(void)
{
    static int64_t bis = 0;

    const bool aktiv = listener.listening()
                       || stt.phase() == Stt::Phase::Verbindet
                       || stt.phase() == Stt::Phase::Hoert
                       || stt.phase() == Stt::Phase::Wartet;

    const int64_t jetzt = esp_timer_get_time();
    if (aktiv) {
        bis = jetzt + kResultHoldUs;
        return true;
    }
    return jetzt < bis;
}

// Solange eine Antwort entsteht oder frisch ist, gehoert ihr das Bild. Sie
// loest die Aufnahmeansicht ab, sobald die Frage draussen ist — die Aufnahme
// ist dann vorbei, und wer gerade gesprochen hat, wartet auf die Antwort und
// nicht auf einen Zeitbalken.
static bool antwort_ansicht(void)
{
    static int64_t  bis     = 0;
    static uint32_t gesehen = 0;

    const Chat::Phase p = chat.phase();
    const Tts::Phase  t = tts.phase();
    const bool aktiv = (p == Chat::Phase::Fragt) || (p == Chat::Phase::Antwortet)
                       || (t == Tts::Phase::Holt) || (t == Tts::Phase::Spricht);

    const int64_t jetzt = esp_timer_get_time();
    if (aktiv) {
        bis = jetzt + kResultHoldUs;
        return true;
    }

    // Nach dem letzten Stueck bleibt sie stehen, damit die Antwort gelesen
    // werden kann und nicht im selben Augenblick verschwindet, in dem sie
    // fertig ist.
    const uint32_t seq = chat.antwort_seq();
    if (seq != gesehen) {
        gesehen = seq;
        bis     = jetzt + kResultHoldUs;
    }
    return jetzt < bis;
}

static void draw_answer(void)
{
    char        buf[48];
    static char frage[Chat::kMaxFrage];
    static char antwort[Chat::kMaxAntwort];

    // Rechts in der Kopfzeile steht der Schritt, der gerade laeuft: erst der
    // Chat, dann die Stimme. Steht keiner mehr aus, die gebrauchte Zeit.
    const Tts::Phase tp = tts.phase();
    if (tp == Tts::Phase::Holt || tp == Tts::Phase::Spricht) {
        snprintf(buf, sizeof(buf), "%s", tts.phase_text());
    } else if (chat.phase() == Chat::Phase::Fragt
               || chat.phase() == Chat::Phase::Antwortet) {
        snprintf(buf, sizeof(buf), "%s", chat.phase_text());
    } else {
        const int32_t ms = chat.last_ms();
        snprintf(buf, sizeof(buf), "%d.%d s",
                 (int)(ms / 1000), (int)((ms / 100) % 10));
    }
    draw_header("ANTWORT", buf);

    snprintf(buf, sizeof(buf), "CHAT: %s   STIMME: %s",
             chat.phase_text(), tts.phase_text());
    display->text(kColLeftX, kPhaseY, buf, ColorBlack, 1);
    display->text(LCD_WIDTH - kColLeftX - Canvas::text_width(net::status(), 1),
                  kPhaseY, net::status(), ColorBlack, 1);
    display->hline(kColLeftX, LCD_WIDTH - kColLeftX - 1, kPhaseY + 12, ColorBlack);

    // Die Frage klein darueber: ohne sie steht die Antwort ohne Bezug da, und
    // bei einer falsch verstandenen Frage ist genau das die Erklaerung.
    chat.copy_frage(frage, sizeof(frage));
    if (frage[0] != ' ') {
        display->text_wrapped(kColLeftX, kFrageY, LCD_WIDTH - 2 * kColLeftX,
                              frage, ColorBlack, 1, kFrageStep, kFrageLines);
    }

    chat.copy_antwort(antwort, sizeof(antwort));
    if (antwort[0] != ' ') {
        display->text_wrapped(kColLeftX, kAntwortY, LCD_WIDTH - 2 * kColLeftX,
                              antwort, ColorBlack, kTextScale, kTextStep,
                              kAntwortLines);
    } else {
        const char *hinweis = (chat.phase() == Chat::Phase::Fehler)
                                  ? "CHAT GESTOERT"
                                  : "DENKT NACH ...";
        display->text(kColLeftX, kAntwortY, hinweis, ColorBlack, kTextScale);
    }
}

// BOOT schaltet zwischen Log und Kennzahlen um. Die Taste haengt an keinem
// Interrupt, sondern wird einmal je Bild abgefragt — 37 ms Abstand sind
// zugleich die Entprellung, dieselbe Ueberlegung wie bei der KEY-Taste.
// Ausgewertet wird die fallende Flanke: sonst liefe die Ansicht durch,
// solange jemand die Taste haelt.
static void poll_view_button(void)
{
    static int vorher = 1;

    const int jetzt = gpio_get_level(BOOT_BUTTON_PIN);
    if (vorher != 0 && jetzt == 0) log_ansicht = !log_ansicht;
    vorher = jetzt;
}

static void draw_scope(const int16_t *lo, const int16_t *hi, int head,
                       int32_t scale, int32_t rms, int32_t peak)
{
    display->clear(ColorWhite);

    // Die Antwort geht vor: sie kommt spaeter als die Aufnahme und loest sie
    // damit sauber ab, ohne dass eine der beiden Ansichten von der anderen
    // wissen muesste.
    if (antwort_ansicht() && !net::provisioning()) {
        draw_answer();
        display->flush();
        return;
    }

    const bool aufnahme = aufnahme_ansicht();

    // Waehrend der Bereitstellung gewinnt die Kennzahlenansicht, egal was
    // eingestellt ist: dort und nur dort stehen Geraetename und Nachweis,
    // ohne die in der App nichts zu finden ist.
    if (!aufnahme && log_ansicht && !net::provisioning()) {
        char buf[32];
        status_text(buf, sizeof(buf));
        draw_header("LOG - BOOT ZEIGT KENNZAHLEN", buf);
        draw_log();
        display->flush();
        return;
    }

    if (aufnahme) {
        draw_listening();
    } else {
        draw_stats(rms);
    }

    // Trennlinien zwischen den drei Baendern.
    display->hline(0, LCD_WIDTH - 1, kStatsBottom + 1, ColorBlack);
    display->hline(0, LCD_WIDTH - 1, kScopeBottom + 2, ColorBlack);

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

    // Ohne laufendes Mikrofon bleibt von der Kurve nur die Nulllinie. Die
    // Beschriftung sagt, warum — eine gerade Linie allein saehe aus wie ein
    // Defekt.
    if (!mic.running()) {
        const char *s = "MIKROFON AUS - KEY GEDRUECKT HALTEN";
        const int   w = Canvas::text_width(s, 1);
        display->fill_rect((LCD_WIDTH - w) / 2 - 4, kScopeCenter - 6,
                           (LCD_WIDTH + w) / 2 + 3, kScopeCenter + 8, ColorWhite);
        display->text((LCD_WIDTH - w) / 2, kScopeCenter - 3, s, ColorBlack, 1);
    }

    display->rect(0, kMeterTop, LCD_WIDTH - 1, kMeterBottom, ColorBlack);
    const int w = level_to_width(rms);
    if (w > 0) {
        display->fill_rect(2, kMeterTop + 2, 2 + w - 1, kMeterBottom - 2, ColorBlack);
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

        poll_view_button();

        const int64_t t0 = esp_timer_get_time();
        draw_scope(lo, hi, head, scale, rms, peak);
        const int32_t dt = (int32_t)(esp_timer_get_time() - t0);

        if (dt > frame_us_max) frame_us_max = dt;
        frame_us_sum += dt;
        frame_count++;
    }
}

// Sensortask: liest langsam veraenderliche Werte fuer das Stats-Band. Eigener
// Task, weil eine SHTC3-Messung 16 ms wartet — im Aufnahmetask wuerde das
// Abtastwerte kosten, im Anzeigetask ein halbes Bild.
static void stats_task(void *)
{
    while (true) {
        int32_t t100 = 0, h100 = 0;
        if (shtc3_measure(&t100, &h100) == ESP_OK) {
            env_temp_c100 = t100;
            env_hum_100   = h100;
            env_valid     = 1;
        } else {
            env_valid = 0;
        }

        int raw = 0;
        if (adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw) == ESP_OK) {
            battery_raw = raw;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}


static void visualize_mic(void)
{
    ESP_LOGI(TAG, "--- Mikrofon-Visualisierung ---");

    if (mic.begin(i2c_bus, kSampleRate) != ESP_OK) {
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

    const esp_err_t lerr = listener.begin(mic.sample_rate());
    if (lerr == ESP_OK) {
        ESP_LOGI(TAG, "  KEY (GPIO%d) gedrueckt halten nimmt auf, loslassen "
                      "beendet; hoechstens %d s am Stueck.",
                 KEY_BUTTON_PIN, Listener::kMaxSeconds);
    } else {
        ESP_LOGW(TAG, "  Kein Aufnahmepuffer (%s) — KEY schaltet nur den "
                      "Zustand um, ohne Mitschnitt.",
                 esp_err_to_name(lerr));
    }

    // Spracherkennung. Ohne Schluessel oder ohne WLAN bleibt sie aus, und
    // alles andere laeuft unveraendert weiter — die Firmware soll auch auf
    // einem Geraet ohne secrets.h benutzbar bleiben.
    const esp_err_t serr = stt.begin(&listener, mic.sample_rate(),
                                     OPENAI_API_KEY, STT_MODEL, STT_LANGUAGE);
    if (serr == ESP_OK) {
        ESP_LOGI(TAG, "  Transkription aktiv: %s, Sprache %s.",
                 STT_MODEL, STT_LANGUAGE);
    } else if (serr == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "  Kein OPENAI_API_KEY in secrets.h — es wird "
                      "aufgenommen, aber nicht erkannt.");
    } else {
        ESP_LOGE(TAG, "  Transkription nicht gestartet (%s).",
                 esp_err_to_name(serr));
    }

    // Antwort auf die erkannte Frage. Haengt am Stt und braucht denselben
    // Schluessel; ohne ihn bleibt es bei Aufnahme und Transkript.
    const esp_err_t cerr = chat.begin(&stt, OPENAI_API_KEY, CHAT_MODEL);
    if (cerr == ESP_OK) {
        ESP_LOGI(TAG, "  Chat aktiv: %s.", CHAT_MODEL);
    } else if (cerr == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "  Kein OPENAI_API_KEY in secrets.h — keine Antworten.");
    } else {
        ESP_LOGE(TAG, "  Chat nicht gestartet (%s).", esp_err_to_name(cerr));
    }

    // Anzeige auf den zweiten Kern, damit das Zeichnen die Aufnahme nicht
    // verdraengt und umgekehrt.
    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 4, nullptr, 1);

    // Niedrige Prioritaet: die Sensorwerte duerfen warten, Bild und Ton nicht.
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, nullptr, 2, nullptr, 0);

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
                                     OPENAI_API_KEY, TTS_MODEL, TTS_VOICE);
    if (terr == ESP_OK) {
        ESP_LOGI(TAG, "  Sprachausgabe aktiv: %s, Stimme %s.", TTS_MODEL, TTS_VOICE);
    } else if (terr == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "  Sprachausgabe aus — Antwort erscheint nur als Text.");
    } else {
        ESP_LOGE(TAG, "  Sprachausgabe nicht gestartet (%s).",
                 esp_err_to_name(terr));
    }

    static int16_t block[kReadFrames];
    int64_t        sum_sq    = 0;
    int32_t        sum_count = 0;
    int32_t        window_pk = 0;
    int64_t        last_log  = esp_timer_get_time();

    int32_t rms = 0;

    while (true) {
        // Die Taste zuerst, unabhaengig vom Mikrofon: solange nicht
        // aufgenommen wird, gibt es keinen Audioblock, an dem sich die
        // Abfrage aufhaengen koennte. Der 20-ms-Takt bleibt derselbe, und
        // damit auch die Entprellung.
        listener.poll_key(gpio_get_level(KEY_BUTTON_PIN) == 0);

        // Der Wandler laeuft nur waehrend einer Aufnahme. Ein Geraet mit
        // Mikrofon soll nicht dauerhaft zuhoeren — und das ist nichts, was
        // man glauben muessen darf: zwischen den Aufnahmen ist der ES7210
        // zugeklappt, nicht nur ungelesen.
        if (listener.listening() && !mic.running()) {
            mic.start();
        } else if (!listener.listening() && mic.running()) {
            mic.stop();

            // Wellenbild auf die Nulllinie zuruecksetzen. Das stehengelassene
            // Bild der letzten Aufnahme sähe aus wie ein laufendes Signal.
            xSemaphoreTake(scope_lock, portMAX_DELAY);
            memset(col_min, 0, sizeof(col_min));
            memset(col_max, 0, sizeof(col_max));
            scope_scale = kScaleFloor;
            shared_rms  = 0;
            shared_peak = 0;
            xSemaphoreGive(scope_lock);

            rms       = 0;
            sum_sq    = 0;
            sum_count = 0;
            window_pk = 0;
        }

        if (!mic.running()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (mic.read_mono(block, kReadFrames) != ESP_OK) {
            ESP_LOGW(TAG, "  Lesefehler, naechster Versuch.");
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            listener.feed(block, kReadFrames);

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

            rms = (sum_count > 0) ? (int32_t)sqrt((double)(sum_sq / sum_count)) : 0;

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
        }

        // Einmal pro Sekunde eine Zeile ins Log, mit Tasten und Batterie.
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            last_log = now;
            const int raw = (int)battery_raw;   // gemessen im stats_task
            const int32_t fc = frame_count;
            frames_per_s = fc;                  // fuer das Stats-Band
            ESP_LOGI(TAG,
                     "Pegel rms=%5d (%.1f dBFS)  peak=%5d  Skala=%5d  |  "
                     "Bild %d/%d ms, %d/s  |  TE %d us (%d Hz, %d Timeouts, DMA %d)"
                     "  |  MIK=%s BOOT=%s KEY=%s  Batterie=%d",
                     (int)rms, 20.0f * log10f(((float)rms + 1.0f) / 32768.0f),
                     (int)window_pk, (int)scope_scale,
                     (int)(fc ? (frame_us_sum / fc / 1000) : 0),
                     (int)(frame_us_max / 1000), (int)fc,
                     (int)display->te_period_us(),
                     (int)(display->te_period_us()
                               ? 1000000 / display->te_period_us()
                               : 0),
                     (int)display->te_timeouts(),
                     (int)display->dma_timeouts(),
                     mic.running() ? "AN" : "aus",
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
    // Vor der ersten eigenen Meldung: alles, was ab hier geloggt wird, soll
    // spaeter auch auf dem Display stehen. Bootloader und fruehe
    // IDF-Initialisierung liegen davor und bleiben der seriellen
    // Schnittstelle vorbehalten.
    logview::begin();

    // Die eigene Taktmeldung bleibt der seriellen Schnittstelle vorbehalten.
    // Sie ist rund zweihundert Zeichen lang und kommt jede Sekunde; auf dem
    // Display haette nach einer halben Minute nichts anderes mehr Platz —
    // und alles, was darin steht, zeigt die Kennzahlenansicht ohnehin.
    logview::mute("Pegel rms=");

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
    // geht stattdessen die Bereitstellung ueber Bluetooth auf — dann steht
    // der Geraetename auf dem Display, siehe draw_stats().
    cfg::begin();
    net::begin();

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
