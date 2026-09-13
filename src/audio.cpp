#include "audio.h"

#include "nachtrag.h"

#include <string.h>

#include <driver/gpio.h>
#include <driver/i2s_tdm.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esp_codec_dev_defaults.h"
#include "user_config.h"

static const char *TAG = "mic";
static const char *SPK = "spk";

// --- Gemeinsamer I2S-Port ---------------------------------------------------
//
// Aufnahme und Wiedergabe teilen sich einen Controller im Vollduplex, weil die
// Platine nur ein Taktpaar fuehrt. Senden laeuft im Standardformat, Empfangen
// im TDM-Format mit vier Schlitzen. Der Sendekanal wird zuerst eingerichtet
// und bleibt Taktgeber; der Empfangskanal haengt sich an ihn.
//
// Dass die beiden Formate zusammenpassen, regelt esp_codec_dev: oeffnet das
// Mikrofon mit vier mal 16 Bit, stellt es den Sendekanal auf 32-Bit-Schlitze
// um, damit beide Richtungen denselben Bittakt haben (64 Bit je Abtastwert).
// Die Konfiguration folgt xiaozhi-esp32 fuer diese Platine.
//
// Aus demselben Grund gibt es auch nur *eine* Datenschnittstelle fuer beide
// Wandler. esp_codec_dev fuehrt darin Buch, welche Richtung gerade laeuft,
// und braucht das: im Vollduplex haengt der Empfangskanal am Takt des
// Sendekanals, also darf das Schliessen der Wiedergabe den Sendekanal nicht
// abschalten, solange aufgenommen wird. Mit zwei getrennten Schnittstellen
// wuesste keine von der anderen — genau das hat hier dazu gefuehrt, dass
// nach der ersten Wiedergabe jede weitere Aufnahme leer blieb.

namespace {

struct {
    i2s_chan_handle_t            tx      = nullptr;
    i2s_chan_handle_t            rx      = nullptr;
    const audio_codec_data_if_t *data_if = nullptr;
    uint32_t                     rate    = 0;
} s_port;

// Und weil es nur eine Datenschnittstelle gibt, darf auch nur einer zur Zeit
// daran drehen. Aufnahme und Wiedergabe liegen in verschiedenen Tasks.
SemaphoreHandle_t s_port_lock = nullptr;

inline void port_sperren()   { if (s_port_lock) xSemaphoreTake(s_port_lock, portMAX_DELAY); }
inline void port_freigeben() { if (s_port_lock) xSemaphoreGive(s_port_lock); }

esp_err_t port_begin(uint32_t sample_rate)
{
    if (s_port.rate != 0) {
        // Ein zweiter Aufruf mit anderer Rate waere kein Detail: der Port ist
        // einer, und der zweite Baustein bekaeme stillschweigend die Rate des
        // ersten.
        if (s_port.rate != sample_rate) {
            ESP_LOGE(TAG, "I2S laeuft bereits auf %" PRIu32 " Hz, nicht %" PRIu32 ".",
                     s_port.rate, sample_rate);
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    s_port_lock = xSemaphoreCreateMutex();
    if (s_port_lock == nullptr) return ESP_ERR_NO_MEM;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Acht statt sechs DMA-Bloecke: der Aufnahmetask rechnet jetzt auch die
    // Echounterdrueckung, und der Ring ist das, was er dabei verspaeten darf.
    chan_cfg.dma_desc_num = 8;
    // Laeuft der Sendering leer, soll Stille herauskommen und nicht der
    // letzte Block in Schleife. Der Lautsprecher bleibt jetzt dauerhaft offen,
    // und die Referenz sieht jeden Wiederholer als Signal.
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_port.tx, &s_port.rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK_PIN;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_LRCLK_PIN;
    std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate);
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm_cfg.clk_cfg.bclk_div      = 8;
    tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
        (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tdm_cfg.slot_cfg.left_align = false;
    tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    tdm_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK_PIN;
    tdm_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK_PIN;
    tdm_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_LRCLK_PIN;
    tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DIN_PIN;

    // Reihenfolge zaehlt: der zuerst eingerichtete Kanal bleibt Master.
    err = i2s_channel_init_std_mode(s_port.tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_tdm_mode(s_port.rx, &tdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S-Kanaele einrichten: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_port.tx);
    if (err == ESP_OK) err = i2s_channel_enable(s_port.rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    audio_codec_i2s_cfg_t i2s_if_cfg = {};
    i2s_if_cfg.port      = I2S_NUM_0;
    i2s_if_cfg.rx_handle = s_port.rx;
    i2s_if_cfg.tx_handle = s_port.tx;
    s_port.data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    if (s_port.data_if == nullptr) {
        ESP_LOGE(TAG, "I2S-Datenschnittstelle konnte nicht angelegt werden.");
        return ESP_FAIL;
    }

    s_port.rate = sample_rate;
    ESP_LOGI(TAG, "I2S-Vollduplex (Senden STD, Empfang TDM 4): MCLK=%d BCLK=%d "
                  "LRCLK=%d DIN=%d DOUT=%d",
             I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DIN_PIN, I2S_DOUT_PIN);
    return ESP_OK;
}

}  // namespace

esp_err_t MicInput::begin(i2c_master_bus_handle_t bus, uint32_t sample_rate,
                          float gain_db, float ref_db)
{
    if (bus == nullptr) {
        ESP_LOGE(TAG, "Kein I2C-Bus uebergeben.");
        return ESP_ERR_INVALID_ARG;
    }
    sample_rate_ = sample_rate;

    esp_err_t err = port_begin(sample_rate);
    if (err != ESP_OK) return err;

    // Die Komponente rechnet intern addr >> 1, deshalb die 8-Bit-Adresse 0x80
    // — das ist dieselbe Einheit wie die 0x40 aus dem I2C-Scan.
    audio_codec_i2c_cfg_t i2c_if_cfg = {};
    i2c_if_cfg.port       = I2C_NUM_0;
    i2c_if_cfg.addr       = ES7210_CODEC_DEFAULT_ADDR;
    i2c_if_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(TAG, "I2C-Steuerschnittstelle konnte nicht angelegt werden.");
        return ESP_FAIL;
    }

    // Alle vier Eingaenge: erst ab drei schaltet der ES7210 auf TDM, und der
    // dritte ist die Referenz.
    es7210_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if      = ctrl_if;
    es_cfg.master_mode  = false;    // der ESP gibt den Takt vor
    es_cfg.mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4;
    const audio_codec_if_t *codec_if = es7210_codec_new(&es_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "ES7210 antwortet nicht.");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if  = s_port.data_if;
    codec_ = esp_codec_dev_new(&dev_cfg);
    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "esp_codec_dev_new fehlgeschlagen.");
        return ESP_FAIL;
    }

    gain_db_ = gain_db;
    ref_db_  = ref_db;

    scratch_ = (int16_t *)heap_caps_malloc(kMaxFrames * kSchlitze * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (scratch_ == nullptr) {
        ESP_LOGE(TAG, "Zwischenpuffer konnte nicht allokiert werden.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES7210 bereit: %" PRIu32 " Hz, 4 Schlitze, 16 Bit, Mikrofone %.1f dB, "
                  "Referenz %.1f dB (noch geschlossen)",
             sample_rate, gain_db, ref_db);
    return ESP_OK;
}

esp_err_t MicInput::start()
{
    if (codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (running_)          return ESP_OK;

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = kSchlitze;
    fs.channel_mask    = 0;         // 0 = alle Kanaele
    fs.sample_rate     = sample_rate_;

    port_sperren();
    int rc = esp_codec_dev_open(codec_, &fs);
    port_freigeben();
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open: %d", rc);
        return ESP_FAIL;
    }

    // Erst alle vier auf den Mikrofonwert, dann die Referenz zurueck. Die
    // Masken zaehlen nach Eingang, nicht nach Schlitz: MIC3 ist Kanal 2.
    if (esp_codec_dev_set_in_gain(codec_, gain_db_) != 0) {
        ESP_LOGW(TAG, "Verstaerkung nicht setzbar, Standardwert bleibt.");
    }
    if (esp_codec_dev_set_in_channel_gain(codec_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                                          ref_db_) != 0) {
        ESP_LOGW(TAG, "Verstaerkung der Referenz nicht setzbar.");
    }
    running_ = true;
    messung_leeren();

    // Ueber nachtrag: start() kommt aus dem Aufnahmetask.
    nachtrag::schreiben('I', TAG, "Mikrofon an (%.1f dB, Referenz %.1f dB).", gain_db_, ref_db_);
    return ESP_OK;
}

void MicInput::stop()
{
    if (codec_ == nullptr || !running_) return;

    port_sperren();
    esp_codec_dev_close(codec_);
    port_freigeben();
    running_ = false;
    ESP_LOGI(TAG, "Mikrofon aus.");
}

esp_err_t MicInput::verstaerkung(float gain_db)
{
    if (codec_ == nullptr || !running_) return ESP_ERR_INVALID_STATE;
    if (gain_db == gain_db_)            return ESP_OK;

    const uint16_t maske = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    if (esp_codec_dev_set_in_channel_gain(codec_, maske, gain_db) != 0) return ESP_FAIL;
    gain_db_ = gain_db;
    return ESP_OK;
}

void MicInput::messung_leeren()
{
    for (int i = 0; i < kSchlitze; i++) {
        peak_[i] = 0;
        clip_[i] = 0;
    }
}

esp_err_t MicInput::read(int16_t *mono, int16_t *ref, size_t frames)
{
    if (codec_ == nullptr || scratch_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!running_)                               return ESP_ERR_INVALID_STATE;
    if (frames == 0 || frames > kMaxFrames)      return ESP_ERR_INVALID_ARG;

    const int bytes = (int)(frames * kSchlitze * sizeof(int16_t));
    int       rc    = esp_codec_dev_read(codec_, scratch_, bytes);
    if (rc != 0) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < frames; i++) {
        const int16_t *r = &scratch_[i * kSchlitze];

        // Mittelwert der beiden Mikrofone. In int32 gerechnet, sonst laeuft
        // die Summe zweier Vollausschlaege ueber.
        mono[i] = (int16_t)(((int32_t)r[kMic1] + (int32_t)r[kMic2]) / 2);
        if (ref != nullptr) ref[i] = r[kRef];

        for (int s = 0; s < kSchlitze; s++) {
            const int32_t a = (r[s] < 0) ? -(int32_t)r[s] : (int32_t)r[s];
            if (a > peak_[s]) peak_[s] = a;
            if (a >= 32000)   clip_[s]++;
        }
    }
    return ESP_OK;
}

// --- Wiedergabe ueber den ES8311 -------------------------------------------

esp_err_t SpeakerOutput::begin(i2c_master_bus_handle_t bus, uint32_t sample_rate,
                               int volume)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    sample_rate_ = sample_rate;
    volume_      = volume;

    esp_err_t err = port_begin(sample_rate);
    if (err != ESP_OK) return err;

    // Den Verstaerker schaltet diese Klasse selbst und nicht esp_codec_dev:
    // dort haengt er am Oeffnen und Schliessen des Wandlers, und genau das
    // soll nicht mehr passieren.
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << AMP_ENABLE_PIN;
    pa.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)AMP_ENABLE_PIN, 0);

    audio_codec_i2c_cfg_t i2c_if_cfg = {};
    i2c_if_cfg.port       = I2C_NUM_0;
    i2c_if_cfg.addr       = ES8311_CODEC_DEFAULT_ADDR;   // 0x30 = 0x18 << 1
    i2c_if_cfg.bus_handle = bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(SPK, "I2C-Steuerschnittstelle konnte nicht angelegt werden.");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if     = ctrl_if;
    es_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin      = -1;                        // siehe oben
    es_cfg.master_mode = false;                     // der ESP gibt den Takt vor
    es_cfg.use_mclk    = true;
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(SPK, "ES8311 antwortet nicht.");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if  = s_port.data_if;
    codec_ = esp_codec_dev_new(&dev_cfg);
    if (codec_ == nullptr) {
        ESP_LOGE(SPK, "esp_codec_dev_new fehlgeschlagen.");
        return ESP_FAIL;
    }

    scratch_ = (int16_t *)heap_caps_malloc(kMaxFrames * kChannels * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (scratch_ == nullptr) {
        ESP_LOGE(SPK, "Zwischenpuffer konnte nicht allokiert werden.");
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = kChannels;
    fs.channel_mask    = 0;
    fs.sample_rate     = sample_rate_;

    port_sperren();
    const int rc = esp_codec_dev_open(codec_, &fs);
    port_freigeben();
    if (rc != 0) {
        ESP_LOGE(SPK, "esp_codec_dev_open: %d", rc);
        codec_ = nullptr;
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(codec_, volume_) != 0) {
        ESP_LOGW(SPK, "Lautstaerke nicht setzbar, Standardwert bleibt.");
    }

    ESP_LOGI(SPK, "ES8311 offen: %" PRIu32 " Hz, Lautstaerke %d %%, Verstaerker aus",
             sample_rate, volume);
    return ESP_OK;
}

esp_err_t SpeakerOutput::start()
{
    if (codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (running_)          return ESP_OK;

    gpio_set_level((gpio_num_t)AMP_ENABLE_PIN, 1);
    running_ = true;
    return ESP_OK;
}

void SpeakerOutput::stop()
{
    if (codec_ == nullptr || !running_) return;

    gpio_set_level((gpio_num_t)AMP_ENABLE_PIN, 0);
    running_ = false;
}

esp_err_t SpeakerOutput::write_mono(const int16_t *pcm, size_t frames)
{
    if (codec_ == nullptr || scratch_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!running_)                               return ESP_ERR_INVALID_STATE;
    if (frames == 0 || frames > kMaxFrames)      return ESP_ERR_INVALID_ARG;

    // Der Schlitz auf dem Bus ist stereo. Ein Monosignal einfach
    // hineinzuschreiben hiesse, jeden zweiten Wert als anderen Kanal zu
    // deuten — das Ergebnis liefe halb so schnell und eine Oktave zu tief.
    for (size_t i = 0; i < frames; i++) {
        scratch_[i * kChannels]     = pcm[i];
        scratch_[i * kChannels + 1] = pcm[i];
    }

    const int bytes = (int)(frames * kChannels * sizeof(int16_t));
    return (esp_codec_dev_write(codec_, scratch_, bytes) == 0) ? ESP_OK : ESP_FAIL;
}
