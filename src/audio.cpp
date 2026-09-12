#include "audio.h"

#include <string.h>

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
// Platine nur ein Taktpaar fuehrt. Der Treiber erkennt den Vollduplex daran,
// dass beide Kanaele *byteweise dieselbe* i2s_std_config_t bekommen — er
// vergleicht die Strukturen per memcmp. Deshalb stehen hier dout und din
// gemeinsam in einer Konfiguration, obwohl jeder Kanal nur eine davon
// benutzt: waeren sie je Richtung verschieden, hielte der Treiber die beiden
// fuer unabhaengige Halbduplexkanaele und liesse zwei Taktteiler auf dieselben
// Pins los.
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
// daran drehen. Aufnahme und Wiedergabe liegen in verschiedenen Tasks; wer
// die Sprechtaste waehrend einer Antwort drueckt, laesst beide im selben
// Augenblick los: das Mikrofon oeffnet den Empfangskanal, waehrend die Stimme
// den Sendekanal zurueckgibt. Beide Vorgaenge aendern denselben Stand.
// Im Mitschnitt stand das als "i2s_channel_disable: the channel has not been
// enabled yet", und die Aufnahme danach hatte null Frames.
SemaphoreHandle_t s_port_lock = nullptr;

// Kein RAII-Wrapper: die vier Stellen sind kurz und stehen beieinander, und
// ein eigener Typ dafuer waere mehr Code als die Sache gross ist.
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
    std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DIN_PIN;

    // Reihenfolge zaehlt: der zuerst eingerichtete Kanal bleibt Master, der
    // zweite wird vom Treiber selbst auf Slave gesetzt und haengt sich an
    // dessen Takt.
    err = i2s_channel_init_std_mode(s_port.tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_port.rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
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
    ESP_LOGI(TAG, "I2S-Vollduplex: MCLK=%d BCLK=%d LRCLK=%d DIN=%d DOUT=%d",
             I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DIN_PIN, I2S_DOUT_PIN);
    return ESP_OK;
}

}  // namespace

esp_err_t MicInput::begin(i2c_master_bus_handle_t bus, uint32_t sample_rate, float gain_db)
{
    if (bus == nullptr) {
        ESP_LOGE(TAG, "Kein I2C-Bus uebergeben.");
        return ESP_ERR_INVALID_ARG;
    }
    sample_rate_ = sample_rate;

    esp_err_t err = port_begin(sample_rate);
    if (err != ESP_OK) return err;
    rx_ = s_port.rx;

    // --- ES7210 ueber esp_codec_dev --------------------------------------
    // Die Datenschnittstelle ist dieselbe wie beim Lautsprecher, siehe oben.

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

    es7210_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if      = ctrl_if;
    es_cfg.master_mode  = false;    // der ESP gibt den Takt vor
    es_cfg.mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2;
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

    scratch_ = (int16_t *)heap_caps_malloc(kMaxFrames * kChannels * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (scratch_ == nullptr) {
        ESP_LOGE(TAG, "Zwischenpuffer konnte nicht allokiert werden.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES7210 bereit: %" PRIu32 " Hz, %d Kanaele, 16 Bit, %.1f dB "
                  "(noch geschlossen)",
             sample_rate, (int)kChannels, gain_db);
    return ESP_OK;
}

esp_err_t MicInput::start()
{
    if (codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (running_)          return ESP_OK;

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = kChannels;
    fs.channel_mask    = 0;         // 0 = alle Kanaele
    fs.sample_rate     = sample_rate_;

    const int64_t t0 = esp_timer_get_time();
    port_sperren();
    int rc = esp_codec_dev_open(codec_, &fs);
    port_freigeben();
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open: %d", rc);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_gain(codec_, gain_db_) != 0) {
        ESP_LOGW(TAG, "Verstaerkung nicht setzbar, Standardwert bleibt.");
    }
    running_ = true;
    peak_l_  = 0;
    peak_r_  = 0;

    ESP_LOGI(TAG, "Mikrofon an (%d ms, %.1f dB).",
             (int)((esp_timer_get_time() - t0) / 1000), gain_db_);
    return ESP_OK;
}

void MicInput::stop()
{
    if (codec_ == nullptr || !running_) return;

    port_sperren();
    esp_codec_dev_close(codec_);
    port_freigeben();
    running_ = false;
    ESP_LOGI(TAG, "Mikrofon aus (Spitze links %d, rechts %d).",
             (int)peak_l_, (int)peak_r_);
}

esp_err_t MicInput::read_mono(int16_t *out, size_t frames)
{
    if (codec_ == nullptr || scratch_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!running_)                               return ESP_ERR_INVALID_STATE;
    if (frames == 0 || frames > kMaxFrames)      return ESP_ERR_INVALID_ARG;

    const int bytes = (int)(frames * kChannels * sizeof(int16_t));
    int       rc    = esp_codec_dev_read(codec_, scratch_, bytes);
    if (rc != 0) {
        return ESP_FAIL;
    }

    // Mittelwert der beiden Mikrofone. In int32 gerechnet, sonst laeuft die
    // Summe zweier Vollausschlaege ueber.
    for (size_t i = 0; i < frames; i++) {
        int32_t l = scratch_[i * kChannels];
        int32_t r = scratch_[i * kChannels + 1];
        out[i]    = (int16_t)((l + r) / 2);

        const int32_t al = (l < 0) ? -l : l;
        const int32_t ar = (r < 0) ? -r : r;
        if (al > peak_l_) peak_l_ = al;
        if (ar > peak_r_) peak_r_ = ar;
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
    tx_ = s_port.tx;

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
    es_cfg.gpio_if     = audio_codec_new_gpio();   // schaltet den Verstaerker
    es_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin      = AMP_ENABLE_PIN;
    es_cfg.master_mode = false;                    // der ESP gibt den Takt vor
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

    ESP_LOGI(SPK, "ES8311 bereit: %" PRIu32 " Hz, Lautstaerke %d %% "
                  "(noch geschlossen)", sample_rate, volume);
    return ESP_OK;
}

esp_err_t SpeakerOutput::start()
{
    if (codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (running_)          return ESP_OK;

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = kChannels;
    fs.channel_mask    = 0;
    fs.sample_rate     = sample_rate_;

    // Das Oeffnen schaltet ueber pa_pin auch den Verstaerker ein. Er bleibt
    // deshalb nur so lange an, wie tatsaechlich etwas abgespielt wird — ein
    // Verstaerker ohne Signal rauscht hoerbar.
    port_sperren();
    const int rc = esp_codec_dev_open(codec_, &fs);
    port_freigeben();
    if (rc != 0) {
        ESP_LOGE(SPK, "esp_codec_dev_open: %d", rc);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(codec_, volume_) != 0) {
        ESP_LOGW(SPK, "Lautstaerke nicht setzbar, Standardwert bleibt.");
    }
    running_ = true;
    return ESP_OK;
}

void SpeakerOutput::stop()
{
    if (codec_ == nullptr || !running_) return;

    port_sperren();
    esp_codec_dev_close(codec_);
    port_freigeben();
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
