#pragma once

// ---------------------------------------------------------------------------
// Eine Verbindung zu api.openai.com, die stehen bleibt.
//
// Gemessen an einer vollstaendigen Runde gingen von 6,2 Sekunden zwischen dem
// Loslassen der Taste und dem ersten Ton rund 1,6 Sekunden fuer zwei
// TLS-Handschlaege drauf — beide zum selben Host, beide mitten auf dem Weg.
// Das ist keine Rechenzeit und keine Bandbreite, sondern reine Wartezeit, und
// sie laesst sich auf zwei Arten loswerden:
//
//   1. Stehen lassen. esp_http_client baut nur dann neu auf, wenn der Zustand
//      unter HTTP_STATE_CONNECTED liegt. Wer die Antwort zu Ende liest und
//      danach *nicht* schliesst, bekommt beim naechsten open() denselben
//      Socket — ohne Handschlag, ohne Zertifikatspruefung.
//
//   2. Vorwaermen. Steht noch nichts, kann der Aufbau vorgezogen werden in
//      eine Zeit, in der ohnehin gewartet wird: waehrend der Sprecher noch
//      spricht, und waehrend die Antwort noch geschrieben wird. Der
//      Handschlag dauert 0,8 Sekunden, gesprochen wird laenger — er ist
//      vorbei, bevor er gebraucht wird.
//
// Vorgewaermt wird mit einer billigen GET-Anfrage auf denselben Host. Was sie
// antwortet, ist gleichgueltig; auch eine 404 haelt die Verbindung offen.
// Gebraucht wird nur der Socket darunter.
//
// Eine stehende Verbindung kann die Gegenseite jederzeit zumachen, ohne dass
// man es merkt. Deshalb gilt der erste fehlgeschlagene Versuch auf einer
// wiederverwendeten Verbindung nicht als Fehler, sondern als Anlass, einmal
// neu aufzubauen.
//
// Kein eigener Schutz gegen gleichzeitige Benutzung: jede Verbindung gehoert
// genau einem Task, und der waermt sie auch selbst vor.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_http_client.h>

class Verbindung {
  public:
    // url   — wohin die eigentlichen Anfragen gehen
    // waerm — billige GET-Adresse auf demselben Host zum Vorwaermen
    // accept— zusaetzlicher Accept-Header oder nullptr
    esp_err_t begin(const char *url, const char *waerm, const char *key,
                    const char *accept, int wartems);

    // Baut auf, falls noch nichts steht. Tut nichts, wenn die Verbindung
    // schon offen ist — darf also in einer Schleife gerufen werden.
    void vorwaermen();

    bool steht() const { return offen_; }

    // POST mit rumpf. Rueckgabe ist der HTTP-Status, 0 heisst: die
    // Verbindung kam nicht zustande.
    int senden(const char *rumpf, int laenge);

    int  lesen(char *dst, int max);
    bool vollstaendig() const;

    // Den Rest der Antwort wegraeumen, damit die Verbindung stehen bleiben
    // darf. Mit kurzer Frist: kommt nichts mehr, ist die Verbindung eben weg
    // — das ist billiger als auf die volle Zeitschranke zu warten.
    void leerlesen(int frist_ms);

    // Nach der Antwort. sauber == false wirft die Verbindung weg; das ist der
    // Fall, wenn mitten im Strom abgebrochen wurde.
    void abschluss(bool sauber);

  private:
    esp_http_client_handle_t c_ = nullptr;

    const char *url_    = nullptr;
    const char *waerm_  = nullptr;
    const char *accept_ = nullptr;
    int         warte_  = 30000;

    char auth_[256] = {0};

    bool offen_ = false;

    // Bis wann nicht wieder vorgewaermt wird. Das Vorwaermen steht in einer
    // Schleife, die alle paar Millisekunden vorbeikommt; ohne Sperrzeit wuerde
    // aus einem misslungenen Handschlag ein Sturm von hunderten.
    int64_t sperre_bis_ = 0;

    // Ein Handschlag braucht rund 20 KB im internen Speicher, und der
    // Bildpuffer holt sich bei jedem Bild 15 KB davon. Wer vorwaermt, waehrend
    // es knapp ist, nimmt der Anzeige den Speicher weg — und die bricht bei
    // ESP_ERROR_CHECK ab, statt ein Bild auszulassen.
    static const size_t kMindestHeap = 60000;
};
