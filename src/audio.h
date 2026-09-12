#pragma once

// ---------------------------------------------------------------------------
// Mikrofoneingang ueber den ES7210 (I2C 0x40) und I2S.
//
// Der ES7210 ist ein reiner ADC: er digitalisiert die beiden Onboard-Mikrofone
// und schiebt sie als I2S-Stream heraus. Takt und Wortsynchronisation kommen
// vom ESP32-S3 (I2S-Master), der ES7210 laeuft als Slave — deshalb muss MCLK
// bespielt werden, sonst laeuft der Wandler ohne Referenz.
//
// Die Registerprogrammierung uebernimmt Espressifs Komponente esp_codec_dev.
// Die Alternative waere, die Registerwerte selbst herzuleiten; die gepflegte
// Komponente ist dabei deutlich verlaesslicher als geratene Konstanten.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_err.h>

#include "esp_codec_dev.h"

class MicInput {
  public:
    // Groesster Block, den read_mono() auf einmal liefern kann. Begrenzt den
    // intern gehaltenen Zwischenpuffer fuer die verschraenkten Rohdaten.
    static const size_t kMaxFrames = 1024;

    // bus muss ein bereits angelegter I2C-Master-Bus sein — der ES7210 haengt
    // am selben Bus wie die uebrigen Bausteine, ein zweiter waere ein Konflikt.
    esp_err_t begin(i2c_master_bus_handle_t bus,
                    uint32_t sample_rate = 16000,
                    float    gain_db     = 30.0f);

    // Liest frames Frames und mittelt die beiden Kanaele zu einem Monosignal.
    // Blockiert, bis so viele Frames vorliegen — bei 16 kHz sind 320 Frames
    // also 20 ms.
    esp_err_t read_mono(int16_t *out, size_t frames);

    uint32_t sample_rate() const { return sample_rate_; }
    uint8_t  channels() const { return kChannels; }

  private:
    static const uint8_t kChannels = 2;

    i2s_chan_handle_t      rx_    = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    uint32_t               sample_rate_ = 0;
    int16_t               *scratch_ = nullptr;   // kMaxFrames * kChannels
};
