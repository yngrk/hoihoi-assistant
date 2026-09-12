#pragma once

// ---------------------------------------------------------------------------
// Die Antwort sprechen.
//
// Dritter von drei Schritten: Sprache zu Text (stt.h), Text zu Antwort
// (chat.h), Antwort zu Sprache (hier).
//
// Protokoll, knapp:
//   POST https://api.openai.com/v1/audio/speech
//   Authorization: Bearer <key>,  Content-Type: application/json
//   -> {"model":..., "voice":..., "input":"...", "response_format":"pcm"}
//   <- roher Datenstrom, 24 kHz, 16 Bit vorzeichenbehaftet, mono, Little
//      Endian, ohne Kopf
//
// response_format "pcm" und nicht mp3 oder opus, weil das Format dann genau
// dem entspricht, was der ES8311 ohnehin bekommt: dieselbe Abtastrate,
// dieselbe Wortbreite, dieselbe Bytereihenfolge. Ein Decoder auf dem Geraet
// entfaellt damit vollstaendig — und mit ihm die Frage, ob er schnell genug
// ist. Der Preis sind 48 KB je Sekunde statt 4, aber die Verbindung steht
// ohnehin und der Ton wird gespielt, waehrend er hereinkommt.
//
// ---------------------------------------------------------------------------
// Zwei Tasks, und das ist der Kern der Sache
//
// Der erste Entwurf schrieb den Ton direkt aus dem HTTP-Lesevorgang in den
// Wandler. Das knackte hoerbar und unregelmaessig, und der Grund ist eine
// Zahl: der I2S-Treiber haelt mit seinen Standardwerten 6 mal 240 Frames
// vor, bei 24 kHz also 60 Millisekunden. Jede Stockung im Netz, die laenger
// dauert als das — und ueber WLAN mit TLS sind hundert Millisekunden nichts
// Besonderes —, laeuft der DMA leer, und ein leerer DMA klingt wie ein
// Knacken.
//
// Deshalb liegt zwischen Netz und Wandler jetzt ein Ringpuffer im PSRAM:
// der eine Task fuellt ihn aus dem Netz, der andere leert ihn in den
// Lautsprecher. Gespielt wird erst, wenn kVorlaufMs darin stehen. Diese
// Vorlaufzeit ist der ganze Handel — sie verzoegert den ersten Ton um genau
// so viel und kauft dafuer denselben Betrag an Stockungstoleranz.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "audio.h"
#include "chat.h"
#include "listen.h"

class Tts {
  public:
    enum class Phase {
        Aus,       // kein Schluessel, kein Netz oder kein Lautsprecher
        Bereit,    // wartet auf die naechste fertige Antwort
        Holt,      // Anfrage laeuft, der Vorlauf ist noch nicht voll
        Spricht,   // Ton laeuft
        Fehler
    };

    esp_err_t begin(Chat *quelle, SpeakerOutput *aus, Listener *taste,
                    const char *key, const char *model, const char *voice);

    Phase       phase() const { return (Phase)phase_; }
    const char *phase_text() const;

    // Wie lange es vom Absenden bis zum ersten Ton gedauert hat, und wie viel
    // davon inzwischen gesprochen ist. Beides fuer die Anzeige: die Wartezeit
    // bis zum ersten Ton ist die Zahl, an der sich diese Kette messen laesst.
    int32_t first_ms() const { return first_ms_; }
    int32_t spoken_ms() const { return spoken_ms_; }

  private:
    static void hol_trampolin(void *self);
    static void spiel_trampolin(void *self);

    void holen();      // Task 1: Netz in den Ring
    void spielen();    // Task 2: Ring in den Lautsprecher
    void sprechen(const char *text);

    void   ring_schreiben(const int16_t *pcm, size_t frames);
    size_t ring_belegt() const { return (size_t)(kopf_ - schwanz_); }

    Chat          *quelle_ = nullptr;
    SpeakerOutput *aus_    = nullptr;
    Listener      *taste_  = nullptr;

    const char *key_   = nullptr;
    const char *model_ = nullptr;
    const char *voice_ = nullptr;

    volatile int32_t phase_     = 0;   // Phase
    volatile int32_t first_ms_  = 0;
    volatile int32_t spoken_ms_ = 0;

    // Beginn der Anfrage. Der Spieler braucht ihn, um die Zeit bis zum
    // ersten Ton zu bestimmen — gemessen wird dort, wo der Ton entsteht,
    // nicht dort, wo das erste Byte ankommt.
    volatile int64_t start_us_ = 0;

    uint32_t gesehen_ = 0;   // zuletzt gesprochene Chat::antwort_seq()

    static const uint32_t kRate = 24000;

    // Acht Sekunden Ring. Mehr waere keine bessere Toleranz, sondern nur
    // mehr Speicher: laenger als der Vorlauf wird der Puffer nur, wenn die
    // Gegenseite schneller liefert, als gesprochen wird — und dann bremst
    // der volle Ring den Lesevorgang aus, was genau richtig ist.
    static const uint32_t kRingFrames = kRate * 8;

    // Zwei Sekunden Vorlauf. Das ist die Stockung, die folgenlos bleibt.
    static const uint32_t kVorlaufFrames = kRate * 2;

    int16_t *ring_ = nullptr;

    // Ein Schreiber, ein Leser, zwei fortlaufende Zaehler: solange nur der
    // Schreiber kopf_ und nur der Leser schwanz_ erhoeht, braucht es keine
    // Sperre. Der Ueberlauf der 32 Bit stoert nicht, weil immer nur die
    // Differenz gebildet wird.
    volatile uint32_t kopf_    = 0;
    volatile uint32_t schwanz_ = 0;

    volatile int32_t auftrag_ = 0;   // es ist etwas zu spielen
    volatile int32_t fertig_  = 0;   // nichts kommt mehr nach
    volatile int32_t abbruch_ = 0;   // Taste gedrueckt, Rest verwerfen

    // 2048 Byte sind 1024 Frames und damit genau ein write_mono().
    static const size_t kLeseBytes = 2048;
    uint8_t *lese_ = nullptr;

    // Ein Abtastwert sind zwei Byte, ein TCP-Paket endet aber irgendwo. Das
    // halbe Wort am Ende eines Blocks gehoert an den Anfang des naechsten,
    // sonst verschiebt sich ab dort jedes Byte und aus Sprache wird Rauschen.
    uint8_t rest_     = 0;
    bool    hat_rest_ = false;
};
