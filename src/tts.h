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
//
// ---------------------------------------------------------------------------
// Satzweise, nicht am Stueck
//
// Frueher wurde gewartet, bis die Antwort vollstaendig dastand. Das kostete
// rund anderthalb Sekunden, in denen der erste Satz laengst fertig war.
// Jetzt wird geholt, was an ganzen Saetzen feststeht, und der Rest waechst
// waehrend des Sprechens nach — in denselben Ring, ohne Naht dazwischen.
//
// Die zweite Portion wird geholt, waehrend die erste noch laeuft. Geht das
// einmal nicht schnell genug, stockt es mitten in der Antwort; deshalb
// zaehlt der Spieler seine Stockungen und schreibt sie ins Log. Ohne diese
// Zahl waere jede Aenderung am Vorlauf geraten.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "audio.h"
#include "chat.h"
#include "listen.h"
#include "verbindung.h"

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

    // Wer die Sprechtaste drueckt, will sprechen und nicht zuhoeren. Die
    // laufende Ausgabe faellt dann weg — und zwar *bevor* das Mikrofon den
    // I2S-Port anfasst, denn beide haengen an derselben Datenschnittstelle.
    //
    // Die laufende Runde des Chats gilt danach als erledigt, auch wenn sie
    // hier noch gar nicht angefangen hat: sonst finge die Stimme mit dem
    // ersten Satz an, der kurz vor dem Abbruch fertig wurde.
    void abbrechen()
    {
        if (quelle_ != nullptr) verworfen_ = (int32_t)quelle_->runde_seq();
        if (auftrag_) abbruch_ = 1;
    }

    // True, solange der Lautsprecher noch beschaeftigt ist. Der Aufnahmetask
    // wartet darauf, bevor er das Mikrofon oeffnet.
    bool spricht() const { return auftrag_ != 0; }

    // Leiser, solange eine Pruefaufnahme laeuft: wer hineinspricht, hoert
    // sofort, dass er gehoert wird, und das Echo wird kleiner — abgebrochen
    // wird erst, wenn die Erkennung Worte gefunden hat.
    void leiser(bool an) { leiser_ = an ? 1 : 0; }

  private:
    static void hol_trampolin(void *self);
    static void spiel_trampolin(void *self);

    void   holen();          // Task 1: Netz in den Ring
    void   spielen();        // Task 2: Ring in den Lautsprecher
    void   runde_spielen();  // eine Antwort, in Portionen
    size_t stueck_holen(const char *text);   // eine Portion, gelieferte Frames

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
    volatile int32_t anlauf_ms_ = 0;   // Verstaerker an bis erster Sprachblock
    volatile int32_t spoken_ms_ = 0;

    // Beginn der Anfrage. Der Spieler braucht ihn, um die Zeit bis zum
    // ersten Ton zu bestimmen — gemessen wird dort, wo der Ton entsteht,
    // nicht dort, wo das erste Byte ankommt.
    volatile int64_t start_us_ = 0;

    uint32_t gesehen_ = 0;   // zuletzt gesprochene Chat::runde_seq()
    volatile int32_t verworfen_ = -1;   // per abbrechen() erledigte Chat::runde_seq()

    // Vorwaerm-Adresse und die stehende Verbindung, siehe verbindung.h.
    char       waerm_[128] = {0};
    Verbindung weg_;

    static const uint32_t kRate = 24000;

    // Acht Sekunden Ring. Mehr waere keine bessere Toleranz, sondern nur
    // mehr Speicher: laenger als der Vorlauf wird der Puffer nur, wenn die
    // Gegenseite schneller liefert, als gesprochen wird — und dann bremst
    // der volle Ring den Lesevorgang aus, was genau richtig ist.
    static const uint32_t kRingFrames = kRate * 8;

    // Vorlauf, und damit die Stockung, die folgenlos bleibt. Zwei Sekunden
    // waren die sichere Wahl, als der Knacks gerade weg war; sie standen
    // aber auch mit zwei Sekunden auf dem Weg zum ersten Ton. Dann 1,2, und
    // bei vier Antworten mit 1,2 s blieb der Zaehler jedes Mal auf null.
    // Jetzt 0,6 — steigen die Stockungen, war das zu viel.
    static const uint32_t kVorlaufFrames = kRate * 3 / 5;

    int16_t *ring_ = nullptr;

    // Ein Schreiber, ein Leser, zwei fortlaufende Zaehler: solange nur der
    // Schreiber kopf_ und nur der Leser schwanz_ erhoeht, braucht es keine
    // Sperre. Der Ueberlauf der 32 Bit stoert nicht, weil immer nur die
    // Differenz gebildet wird.
    volatile uint32_t kopf_    = 0;
    volatile uint32_t schwanz_ = 0;

    volatile int32_t auftrag_    = 0;   // es ist etwas zu spielen
    volatile int32_t fertig_     = 0;   // nichts kommt mehr nach
    volatile int32_t abbruch_    = 0;   // Taste gedrueckt, Rest verwerfen
    volatile int32_t stockungen_ = 0;   // Ring lief mitten im Sprechen leer
    volatile int32_t leiser_     = 0;

    // 2048 Byte sind 1024 Frames und damit genau ein write_mono().
    static const size_t kLeseBytes = 2048;
    uint8_t *lese_ = nullptr;

    // Ein Abtastwert sind zwei Byte, ein TCP-Paket endet aber irgendwo. Das
    // halbe Wort am Ende eines Blocks gehoert an den Anfang des naechsten,
    // sonst verschiebt sich ab dort jedes Byte und aus Sprache wird Rauschen.
    uint8_t rest_     = 0;
    bool    hat_rest_ = false;
};
