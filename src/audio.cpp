#include "audio.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "esp_codec_dev_defaults.h"
#include "user_config.h"

static const char *TAG = "mic";

esp_err_t MicInput::begin(i2c_master_bus_handle_t bus, uint32_t sample_rate, float gain_db)
{
    if (bus == nullptr) {
        ESP_LOGE(TAG, "Kein I2C-Bus uebergeben.");
        return ESP_ERR_INVALID_ARG;
    }
    sample_rate_ = sample_rate;

    // --- I2S-Empfangskanal, ESP als Master -------------------------------
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t         err      = i2s_new_channel(&chan_cfg, nullptr, &rx_);
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
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;       // reiner Eingang
    std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DIN_PIN;

    err = i2s_channel_init_std_mode(rx_, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(rx_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    // --- ES7210 ueber esp_codec_dev --------------------------------------
    audio_codec_i2s_cfg_t i2s_if_cfg = {};
    i2s_if_cfg.port      = I2S_NUM_0;
    i2s_if_cfg.rx_handle = rx_;
    i2s_if_cfg.tx_handle = nullptr;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    if (data_if == nullptr) {
        ESP_LOGE(TAG, "I2S-Datenschnittstelle konnte nicht angelegt werden.");
        return ESP_FAIL;
    }

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
    dev_cfg.data_if  = data_if;
    codec_ = esp_codec_dev_new(&dev_cfg);
    if (codec_ == nullptr) {
        ESP_LOGE(TAG, "esp_codec_dev_new fehlgeschlagen.");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = kChannels;
    fs.channel_mask    = 0;         // 0 = alle Kanaele
    fs.sample_rate     = sample_rate;
    int rc = esp_codec_dev_open(codec_, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open: %d", rc);
        return ESP_FAIL;
    }

    rc = esp_codec_dev_set_in_gain(codec_, gain_db);
    if (rc != 0) {
        ESP_LOGW(TAG, "Verstaerkung nicht setzbar (%d), Standardwert bleibt.", rc);
    }

    scratch_ = (int16_t *)heap_caps_malloc(kMaxFrames * kChannels * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (scratch_ == nullptr) {
        ESP_LOGE(TAG, "Zwischenpuffer konnte nicht allokiert werden.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES7210 bereit: %" PRIu32 " Hz, %d Kanaele, 16 Bit, %.1f dB",
             sample_rate, (int)kChannels, gain_db);
    ESP_LOGI(TAG, "I2S: MCLK=%d BCLK=%d LRCLK=%d DIN=%d",
             I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DIN_PIN);
    return ESP_OK;
}

esp_err_t MicInput::read_mono(int16_t *out, size_t frames)
{
    if (codec_ == nullptr || scratch_ == nullptr) return ESP_ERR_INVALID_STATE;
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
    }
    return ESP_OK;
}
