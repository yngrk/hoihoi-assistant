#pragma once

// ---------------------------------------------------------------------------
// Mikrofoneingang ueber den ES7210 (I2C 0x40) und I2S.
//
// Der ES7210 ist ein reiner ADC: er digitalisiert die Onboard-Mikrofone und
// schiebt sie als I2S-Stream heraus. Takt und Wortsynchronisation kommen vom
// ESP32-S3 (I2S-Master), der ES7210 laeuft als Slave — deshalb muss MCLK
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
//
// ---------------------------------------------------------------------------
// Vier Kanaele statt zwei: die Referenz fuer die Echounterdrueckung
//
// Die Platine fuehrt den Ausgang des ES8311 auf den dritten Eingang des
// ES7210 zurueck. Das ist genau das Signal, das der Lautsprecher bekommt, im
// selben Abtasttakt wie die Mikrofone — die Referenz, die eine
// Echounterdrueckung braucht, und ohne jede Verzoegerung zwischen beiden.
// Mit mehr als zwei Eingaengen spricht der ES7210 TDM, vier Schlitze je
// Abtastwert, und zwar in der Reihenfolge MIC1, MIC3, MIC2, MIC4. Die
// Verstaerkungsregister zaehlen dagegen nach der Bestueckung: MIC3 ist dort
// Kanal 2 und nicht 1. Beides steht so auch in xiaozhi-esp32, das auf dieser
// Platine laeuft.
//
// Das Mikrofon bleibt damit dauerhaft offen, auch waehrend einer Antwort.
// Frueher war es dann zu, weil jedes Oeffnen des Lautsprechers beide Kanaele
// neu einrichtete; jetzt bleibt auch der Lautsprecher offen und nur sein
// Verstaerker wird geschaltet.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_err.h>

#include "esp_codec_dev.h"

class MicInput {
  public:
    // Groesster Block, den read() auf einmal liefern kann. Begrenzt den
    // intern gehaltenen Zwischenpuffer fuer die verschraenkten Rohdaten.
    static const size_t kMaxFrames = 1024;

    // Schlitze im TDM-Rahmen, siehe oben.
    enum Schlitz { kMic1 = 0, kRef = 1, kMic2 = 2, kMic4 = 3, kSchlitze = 4 };

    // bus muss ein bereits angelegter I2C-Master-Bus sein — der ES7210 haengt
    // am selben Bus wie die uebrigen Bausteine, ein zweiter waere ein Konflikt.
    // Richtet Bus und Wandler ein, schaltet das Mikrofon aber noch nicht
    // scharf — dafuer ist start() da.
    //
    // gain_db geht an den Verstaerker im ES7210. Dessen Stufen sind 0 bis 33 dB
    // in Dreierschritten, danach 34.5, 36 und 37.5 dB — mehr gibt der Baustein
    // nicht her. Der Standardwert der Komponente waeren 30 dB; das war hier
    // hoerbar zu leise, Sprache landete bei etwa -19 dBFS effektiv.
    //
    // ref_db ist die Verstaerkung des Referenzeingangs. Uebersteuert er, ist
    // die Referenz nicht mehr das, was der Lautsprecher spielt, und die
    // Echounterdrueckung rechnet mit dem falschen Signal.
    esp_err_t begin(i2c_master_bus_handle_t bus,
                    uint32_t sample_rate = 24000,
                    float    gain_db     = 37.5f,
                    float    ref_db      = 15.0f);

    esp_err_t start();
    void      stop();
    bool      running() const { return running_; }

    // Liest frames Frames. mono ist der Mittelwert der beiden Mikrofone, ref
    // die Referenz vom Lautsprecher (darf nullptr sein). Blockiert, bis so
    // viele Frames vorliegen — bei 24 kHz sind 480 Frames also 20 ms.
    esp_err_t read(int16_t *mono, int16_t *ref, size_t frames);

    // Verstaerkung der beiden Mikrofone, ohne die Referenz. Waehrend einer
    // Antwort wird sie gesenkt: ein uebersteuertes Echo ist nicht mehr linear
    // und laesst sich nicht mehr abziehen.
    esp_err_t verstaerkung(float gain_db);
    float     verstaerkung() const { return gain_db_; }

    uint32_t sample_rate() const { return sample_rate_; }

    // Spitzenwerte und Zahl der Werte am Anschlag je Schlitz, seit dem
    // letzten Aufruf von messung_leeren(). Aus ihnen ist abzulesen, ob die
    // Schlitzreihenfolge stimmt (MIC4 ist nicht bestueckt und bleibt still)
    // und ob Mikrofon oder Referenz uebersteuern.
    int32_t spitze(int schlitz) const { return peak_[schlitz]; }
    int32_t anschlag(int schlitz) const { return clip_[schlitz]; }
    void    messung_leeren();

  private:
    esp_codec_dev_handle_t codec_ = nullptr;
    uint32_t               sample_rate_ = 0;
    float                  gain_db_ = 0.0f;
    float                  ref_db_  = 0.0f;
    bool                   running_ = false;
    int32_t                peak_[kSchlitze] = {0};
    int32_t                clip_[kSchlitze] = {0};
    int16_t               *scratch_ = nullptr;   // kMaxFrames * kSchlitze
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
    //
    // Der Wandler wird hier schon geoeffnet und bleibt offen, siehe oben.
    esp_err_t begin(i2c_master_bus_handle_t bus,
                    uint32_t sample_rate = 24000,
                    int      volume      = 100);

    // Spielt ein Monosignal ab. Blockiert, bis die Frames im DMA sind, und
    // verdoppelt sie dabei auf beide Kanaele — der Schlitz auf dem Bus ist
    // stereo, ein Monosignal darin waere halb so schnell und eine Oktave
    // zu tief.
    esp_err_t write_mono(const int16_t *pcm, size_t frames);

    // Verstaerker an und aus. Ein eingeschalteter Verstaerker ohne Signal
    // rauscht hoerbar, deshalb bleibt er nur waehrend einer Antwort an.
    esp_err_t start();
    void      stop();
    bool      running() const { return running_; }

    bool ready() const { return codec_ != nullptr; }

  private:
    static const uint8_t kChannels = 2;

    esp_codec_dev_handle_t codec_ = nullptr;
    uint32_t               sample_rate_ = 0;
    int                    volume_  = 100;
    bool                   running_ = false;
    int16_t               *scratch_ = nullptr;   // kMaxFrames * kChannels
};
