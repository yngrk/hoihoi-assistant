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
//
// Dazu die Wiedergabe ueber den ES8311 (I2C 0x18). Beide Bausteine haengen am
// selben Taktpaar — es gibt auf der Platine nur ein BCLK, ein LRCLK und ein
// MCLK —, also muessen sie sich einen I2S-Port im Vollduplex teilen. Zwei
// Controller koennten dieselben Pins nicht gemeinsam treiben. Der Port wird
// deshalb einmal angelegt und von beiden Klassen benutzt; wer zuerst begin()
// ruft, legt die Abtastrate fest.
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
    // Richtet Bus und Wandler ein, schaltet das Mikrofon aber noch nicht
    // scharf — dafuer ist start() da.
    //
    // gain_db geht an den Verstaerker im ES7210. Dessen Stufen sind 0 bis 33 dB
    // in Dreierschritten, danach 34.5, 36 und 37.5 dB — mehr gibt der Baustein
    // nicht her. Der Standardwert der Komponente waeren 30 dB; das war hier
    // hoerbar zu leise, Sprache landete bei etwa -19 dBFS effektiv.
    esp_err_t begin(i2c_master_bus_handle_t bus,
                    uint32_t sample_rate = 24000,
                    float    gain_db     = 37.5f);

    // Mikrofon an und aus. Nur zwischen start() und stop() digitalisiert der
    // ES7210 ueberhaupt etwas; ausserhalb ist er zugeklappt. Ein Geraet mit
    // Mikrofon soll nicht dauerhaft zuhoeren, und ob es das tut, darf man
    // nicht glauben muessen — es ist derselbe Baustein, der sonst laeuft.
    esp_err_t start();
    void      stop();
    bool      running() const { return running_; }

    // Liest frames Frames und mittelt die beiden Kanaele zu einem Monosignal.
    // Blockiert, bis so viele Frames vorliegen — bei 24 kHz sind 480 Frames
    // also 20 ms.
    esp_err_t read_mono(int16_t *out, size_t frames);

    uint32_t sample_rate() const { return sample_rate_; }
    uint8_t  channels() const { return kChannels; }

    // Spitzenwerte der beiden Wandlerkanaele seit start(), unvermischt. Wenn
    // nur ein Mikrofon bestueckt ist, steht der zweite Kanal auf Null — und
    // die Mittelung in read_mono() kostet dann genau 6 dB, ohne dass man es
    // dem Mischsignal ansieht.
    int32_t peak_left() const { return peak_l_; }
    int32_t peak_right() const { return peak_r_; }

  private:
    static const uint8_t kChannels = 2;

    i2s_chan_handle_t      rx_    = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    uint32_t               sample_rate_ = 0;
    float                  gain_db_ = 0.0f;
    bool                   running_ = false;
    int32_t                peak_l_  = 0;
    int32_t                peak_r_  = 0;
    int16_t               *scratch_ = nullptr;   // kMaxFrames * kChannels
};

class SpeakerOutput {
  public:
    // Groesster Block, den write_mono() auf einmal annimmt.
    static const size_t kMaxFrames = 1024;

    // Derselbe I2C-Bus wie beim Mikrofon, dieselbe Abtastrate.
    //
    // volume ist kein Anteil der Leistung, sondern ein Punkt auf der
    // Standardkurve von esp_codec_dev: die bildet 0 bis 100 linear auf
    // -50 bis 0 dB ab. 70 waeren also nicht "etwas leiser", sondern -15 dB,
    // und genau so klingt es auch. 100 ist deshalb der Normalfall und nicht
    // die Ausnahme — lauter geht ueber den DAC ohnehin nicht ohne Clipping.
    esp_err_t begin(i2c_master_bus_handle_t bus,
                    uint32_t sample_rate = 24000,
                    int      volume      = 100);

    // Spielt ein Monosignal ab. Blockiert, bis die Frames im DMA sind, und
    // verdoppelt sie dabei auf beide Kanaele — der Schlitz auf dem Bus ist
    // stereo, ein Monosignal darin waere halb so schnell und eine Oktave
    // zu tief.
    esp_err_t write_mono(const int16_t *pcm, size_t frames);

    // Wiedergabe an und aus. start() schaltet dabei ueber den PA-Pin auch den
    // Verstaerker ein, stop() wieder aus — ein eingeschalteter Verstaerker
    // ohne Signal rauscht hoerbar.
    esp_err_t start();
    void      stop();
    bool      running() const { return running_; }

    bool ready() const { return codec_ != nullptr; }

  private:
    static const uint8_t kChannels = 2;

    i2s_chan_handle_t      tx_    = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    uint32_t               sample_rate_ = 0;
    int                    volume_  = 100;
    bool                   running_ = false;
    int16_t               *scratch_ = nullptr;   // kMaxFrames * kChannels
};
