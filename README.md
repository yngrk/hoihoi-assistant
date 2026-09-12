# hoihoi-screen-assistant

Firmware für das **Waveshare ESP32-S3-RLCD-4.2**. Aktueller Stand: Hardware-Bring-up.

## Board

| | |
|---|---|
| Modul | ESP32-S3-WROOM-1-N16R8 |
| Flash | 16 MB, QIO @ 80 MHz |
| PSRAM | 8 MB, **Octal**-Modus @ 80 MHz |
| Display | 400×300 monochrom, reflektiv (kein Backlight), ST7305 über SPI |
| Audio | ES8311 Codec (Ausgabe), ES7210 ADC (Dual-Mikrofon), Lautsprecher onboard |
| Sensorik | SHTC3 (Temperatur/Feuchte), PCF85063 (RTC) |
| Sonstiges | microSD, RGB-LED (GPIO38), 18650-Ladeschaltung |

Der Octal-PSRAM ist nicht optional: ohne `CONFIG_SPIRAM_MODE_OCT` bleiben rund
320 KB internes RAM, was für einen 400×300-Framebuffer nicht reicht.

## Pinbelegung

| Funktion | GPIO |
|---|---|
| LCD SCLK / MOSI | 11 / 12 |
| LCD CS / DC / RST / TE | 40 / 5 / 41 / 6 |
| I²C SDA / SCL | 13 / 14 |
| I²S MCLK / BCLK / LRCLK | 16 / 9 / 45 |
| I²S DIN (Mic) / DOUT (Speaker) | 10 / 8 |
| Verstärker-Enable | 46 |
| BOOT / KEY (beide active low) | 0 / 18 |
| Batterie-ADC | 4 (ADC1_CH3) |

Gepflegt in [src/user_config.h](src/user_config.h).

I²C-Adressen: `0x18` ES8311, `0x40` ES7210, `0x51` PCF85063, `0x70` SHTC3.

## Toolchain

PlatformIO mit ESP-IDF. Die Plattform ist in [platformio.ini](platformio.ini)
bewusst auf `espressif32@6.13.0` gepinnt — das entspricht ESP-IDF 5.5.3 und
damit derselben Minor-Serie, gegen die Waveshare seine Beispiele baut (5.5.1).
`espressif32@7.x` läge bei IDF 6.1, wo etliche APIs entfernt wurden, die der
übernommene Display-Treiber noch verwendet.

Der CPU-Takt steht über `sdkconfig.defaults` auf 240 MHz statt der IDF-Vorgabe
von 160 MHz. Achtung beim Ändern von `sdkconfig.defaults`: eine bereits erzeugte
`sdkconfig.rlcd42` gewinnt gegen die Defaults, neue Werte greifen dann nicht.
Dafür die Datei löschen — sie wird beim nächsten Build neu erzeugt und ist
ohnehin nicht versioniert.

```bash
pio run                  # bauen
pio run -t upload        # flashen
pio device monitor       # Log ansehen
pio run -t upload -t monitor
```

Auf Windows stürzt der Compiler gelegentlich ab, immer mit
`internal compiler error: Segmentation fault` in `during RTL pass: ira`, meist
in `esp_lcd/rgb/esp_lcd_panel_rgb.c`. Das ist kein Fehler im Projekt: derselbe
Aufruf mit denselben Flags läuft einzeln zuverlässig durch, bei sechzehn
gleichzeitigen Aufrufen scheitert im Mittel einer. Auslöser ist die
Parallelität, nicht die Übersetzungseinheit. Ein voller Neubau läuft deshalb
zuverlässiger mit gedrosselter Parallelität — nötig wird er, sobald sich
`src/CMakeLists.txt` oder die Abhängigkeiten ändern:

```bash
pio run -j 6
```

Geflasht wird über den nativen USB-Serial/JTAG-Port des ESP32-S3
(VID `303A` / PID `1001`). Der Port ist in `platformio.ini` fest auf `COM6`
gesetzt; bei anderem Port dort anpassen oder die Zeilen entfernen, dann sucht
PlatformIO selbst.

## Aufbau

```
src/main.cpp              Bring-up-Ablauf
src/user_config.h         Pinbelegung
src/gfx.h, src/gfx.cpp    Clippende Zeichenschicht über dem Treiber
src/display_sync.h, .cpp  DisplayPort mit Rückmeldung über das DMA-Ende
src/font5x7.h, .cpp       5×7-Bitmapfont, ASCII 0x20–0x7F plus ä ö ü ß
src/audio.h, src/audio.cpp  ES7210 und ES8311 im Vollduplex an einem I²S-Port
src/listen.h, .cpp        Zuhören auf Tastendruck, Mitschnitt im PSRAM
src/logview.h, .cpp       ESP-Log zusätzlich auf das Display
src/net.h, .cpp           WLAN im Stationsbetrieb
src/cfg.h, .cpp           Zugangsdaten im NVS
src/prov.h, .cpp          Einrichtung über BLE
src/verbindung.h, .cpp    Stehende HTTPS-Verbindung zu api.openai.com
src/stt.h, .cpp           Sprache zu Text über die Realtime-API von OpenAI
src/chat.h, .cpp          Text zu Antwort, als Strom
src/tts.h, .cpp           Antwort zu Sprache, satzweise
src/secrets.h.example     Vorlage für WLAN-Zugang und API-Schlüssel
src/idf_component.yml     esp_codec_dev und esp_websocket_client
components/port_bsp/      ST7305-Treiber von Waveshare, eine Zeile geändert
sdkconfig.defaults        Flash-, PSRAM- und Konsolenkonfiguration
partitions.csv            8 MB App-Partition
```

Der Display-Treiber (`components/port_bsp/`) stammt aus Waveshares
ESP-IDF-Beispiel `09_LVGL_V9_Test` und bleibt so nah am Original wie möglich,
damit Updates von dort einfach nachgezogen werden können. Die Kommentare darin
sind chinesisch.

Genau eine Zeile weicht ab: `private:` in `display_bsp.h` ist zu `protected:`
geworden. Damit kommt `SyncDisplay` an `io_handle` heran und kann sich das Ende
der DMA-Übertragung melden lassen — siehe unten. Der Alternativweg wäre gewesen,
den Treiber selbst umzubauen; eine Ableitung lässt sich bei einem Update
dagegen einfach wieder daraufsetzen.

`DisplayPort::RLCD_SetPixel()` prüft seine Koordinaten nicht, und die
Lookup-Tabelle hat eine fest auf 300 gesetzte Zeilenlänge: ein `x >= 400` liest
hinter die Allokation, ein `y >= 300` still in die Zeile des nächsten `x`. Der
Treiber ruft die Methode nirgends selbst auf, deshalb liegt die Prüfung in
[src/gfx.h](src/gfx.h) statt im Treiber — so bleibt `port_bsp` byte-identisch
zum Upstream und die Absicherung ist trotzdem lückenlos. **Nicht direkt
`RLCD_SetPixel()` aufrufen, immer über `Canvas`.**

## Was der Bring-up prüft

1. **Chip und Speicher** — Kerne, Revision, Flash-Größe, PSRAM-Initialisierung
   und freier SPIRAM. Meldet ausdrücklich, wenn weniger als 8 MB erkannt werden.
2. **I²C-Scan** — tastet `0x08`–`0x77` ab und benennt die vier erwarteten
   Bausteine namentlich.
3. **SHTC3-Messung** — Wakeup, Messbefehl, Auslesen, Sleep. Beweist, dass der
   Bus nicht nur ACKt, sondern plausible Werte liefert.
4. **Display** — Testbild aus Rahmen, zwei Diagonalen, einem asymmetrischen
   Marker links oben und einem Schachbrett. Der Rahmen prüft die Ränder (er
   liegt exakt auf Zeile 0/299 und Spalte 0/399 — auf Hardware verifiziert, dort
   verdeckt keine Blende etwas), die Diagonalen die Adressierung, das
   Schachbrett die Bit-Packung innerhalb eines Bytes, der Marker die
   Orientierung: die anderen Elemente sind symmetrisch und würden eine
   Spiegelung nicht verraten. Danach elf Zeichenaufrufe mit Koordinaten
   außerhalb der Fläche als Selbsttest der Bereichsprüfung.
5. **Tasten und Batterie** — Zustand beider Tasten und Batterie-Rohwert.
6. **Mikrofon** — läuft anschließend dauerhaft, siehe unten.

## Bildaufbau

Die Fläche ist in drei Bänder über die volle Breite geteilt, nach einem
Figma-Entwurf mit den Anteilen 76 / 19 / 4 Prozent bei 4:3:

| Band | Zeilen | Inhalt |
|---|---|---|
| stats | 0–227 | Kopfzeile, Umweltwerte links, Systemzustand rechts — während einer Aufnahme stattdessen Aufnahmezustand und Transkript |
| audio wave visualizer | 230–286 | Wellenbild, eine Sekunde Signal |
| audio scale | 290–299 | Pegelbalken in dBFS |

Text kommt aus einem eigenen 5×7-Bitmapfont, nicht aus LVGL. Bei einem Bit je
Pixel gibt es nichts zu rastern und nichts zu glätten, und so bleiben die
Bereichsprüfung und die TE-Synchronisation in `Canvas` unverändert gültig —
LVGL brächte sein eigenes Puffer- und Auffrischmodell mit und würde beides
verdrängen. `Canvas::text()` skaliert ganzzahlig; Hierarchie entsteht über die
Zeichengröße, weil Graustufen für diesen Zweck fehlen. `0x7F` ist im Font kein
DEL, sondern ein Gradzeichen.

Die Umweltwerte liest ein eigener `stats_task` alle zwei Sekunden. Eine
SHTC3-Messung wartet 16 ms — im Aufnahmetask kostete das Abtastwerte, im
Anzeigetask ein halbes Bild. Die Werte stehen in ausgerichteten 32-Bit-Worten
ohne Mutex: Schreiben und Lesen sind dort unteilbar, und ob die Anzeige einen
Messwert ein Bild später übernimmt, ist bei zwei Sekunden Messabstand ohne
Belang.

Das Stats-Band wird in jedem Bild neu gezeichnet, obwohl sich sein Inhalt
höchstens sekündlich ändert. Das ist bewusst: von den 37 ms Bildperiode gehen
nur wenige Millisekunden fürs Zeichnen drauf, der Rest ist ohnehin Warten auf
die Austastlücke. Eine Teilaktualisierung würde Zustand einführen, ohne Zeit zu
sparen.

## Mikrofon-Visualisierung

Die beiden Onboard-Mikrofone hängen am ES7210, einem reinen ADC. Der ESP32-S3
ist I²S-Master und gibt Takt und Wortsynchronisation vor, der ES7210 läuft als
Slave — MCLK muss deshalb bespielt werden, sonst arbeitet der Wandler ohne
Referenz. Die Registerprogrammierung übernimmt Espressifs `esp_codec_dev`; das
ist deutlich verlässlicher, als die Werte selbst herzuleiten.

24 kHz, 16 Bit, beide Kanäle zu Mono gemittelt. 60 Frames je Bildspalte mal 400
Spalten ergeben genau eine Sekunde Signal über die volle Breite; das Bild läuft
nach links weg.

24 kHz und nicht 16, seit die Transkription dazugekommen ist: die Realtime-API
ist auf 24 kHz ausgelegt, und so geht der Ton unverändert hinaus. Umrechnen auf
dem Gerät wäre zusätzlicher Code an einer Stelle, an der ein Fehler nur als
schlechtere Erkennung auffiele.

Aufnahme und Anzeige laufen in **getrennten Tasks**, und das ist keine
Stilfrage. Das Panel gibt über die TE-Leitung 27,03 Hz vor (gemessen: 36990 µs,
sehr stabil). Waren beide aneinandergekoppelt, lag die Bildrate auf dem
Audiotakt von 25 Hz — zwei fast gleiche Frequenzen ergeben eine Schwebung von
gut 2 Hz, sichtbar als regelmäßiges Stottern. Entkoppelt taktet sich die
Anzeige über die TE-Leitung selbst auf die Panelfrequenz, die Aufnahme läuft in
ihrem eigenen Takt, und keine zieht die andere.

Die TE-Synchronisation steckt in `Canvas` ([src/gfx.h](src/gfx.h)), nicht im
Treiber. `RLCD_Init()` schaltet die Leitung bereits ein (Befehl `0x35` mit
Parameter `0x00`), ausgewertet hat sie bisher niemand: `RLCD_Sendbuffera()`
schickt den Puffer über `esp_lcd_panel_io_tx_color` **asynchron** per DMA los,
der Transfer von 15000 Byte bei 10 MHz dauert rund 12 ms und landete ohne
Synchronisation quer über dem Bildaufbau des Panels — das wanderte von Bild zu
Bild und war als Tearing sichtbar.

Dieselbe Asynchronität hatte einen zweiten, davon unabhängigen Effekt: nach dem
Absetzen lief der Zeichencode sofort weiter und überschrieb mit
`RLCD_ColorClear()` denselben Puffer, aus dem das DMA noch zwölf Millisekunden
lang las. Das Panel bekam die oberen Zeilen aus dem alten und die unteren aus
dem neuen Bild. Bei 27 Hz und einem Wellenbild, das sich ohnehin bewegt, fällt
das kaum auf — mit einem stehenden Stats-Band und erst recht bei 51 Hz wäre es
deutlich geworden.

`SyncDisplay` ([src/display_sync.h](src/display_sync.h)) hängt dafür den
Rückruf `on_color_trans_done` an den Panel-IO und lässt `flush()` warten, bis
der Puffer gelesen ist. Das kostet nichts: von den 37 ms Bildperiode gehen 12 ms
für die Übertragung und rund 4 ms fürs Zeichnen weg, der Rest war ohnehin Warten
auf die nächste Austastlücke. Auf Hardware gemessen bleibt die Bildrate bei
27/s und die TE-Periode bei 36990 µs, ohne eine einzige ausbleibende
Abschlussmeldung.

Der Vollausschlag skaliert automatisch — sofort auf, langsam wieder zu, nie
unter einen Sockelwert. Die Empfindlichkeit der Mikrofone ist nicht
dokumentiert, ein fester Faktor würde also entweder in Stille das Grundrauschen
aufblasen oder bei Sprache am Anschlag kleben.

Auf der Hardware gemessen: Grundrauschen bei −63 dBFS, Raumgeräusch um
−40 dBFS, laute Sprache −21 dBFS — über 40 dB nutzbarer Dynamikumfang. Das
Zeichnen selbst kostet rund 4 ms, der Rest der 37 ms ist gewolltes Warten auf
die Austastlücke.

## Zuhören auf Tastendruck

Die KEY-Taste (GPIO18) ist eine Sprechtaste: drücken und **halten** nimmt auf,
loslassen beendet. Damit bestimmt der Sprecher Anfang und Ende selbst, und es
kann kein Zustand offen stehen bleiben, den niemand bemerkt hat — der Fall, den
ein Umschalter zwangsläufig mitbringt.

Die Zehn-Sekunden-Schranke bleibt trotzdem, denn der Puffer ist endlich. Sie
ist ein Netz, kein Bedienelement: greift sie, endet die Aufnahme, und die Taste
wird erst nach dem Loslassen wieder scharf. Der Puffer fasst elf Sekunden, also
eine mehr als die Schranke — bei genau zehn liefe er voll, bevor die Uhr
abgelaufen ist, und jede Aufnahme endete mit der falschen Begründung.

Der Mitschnitt liegt im PSRAM (elf Sekunden bei 24 kHz, rund 528 KB) und bleibt
nach dem Ende stehen. Daran hängt die Transkription: sie bekommt einen sauber
abgegrenzten Abschnitt statt eines endlosen Stroms, und weil alles im Puffer
steht, geht auch nichts verloren, während die Verbindung noch aufgebaut wird.

Die Taste hängt an keinem Interrupt. Der Aufnahmetask fragt sie alle 20 ms ab,
wenn ohnehin ein Audioblock vorliegt, und dieser Abtastabstand ist zugleich die
Entprellung. Zwei Abfragen ohne Kontakt gelten als Loslassen — ein einzelner
Prellimpuls während des Haltens schnitte sonst mitten im Wort ab.

### Was vorne und hinten abgeschnitten wird

Von jeder Aufnahme fallen die ersten 120 und die letzten 60 ms weg. Das ist
kein Sicherheitsabstand, sondern die Antwort auf zwei gemessene Störungen:
das Einschalten des ES7210 setzt einen Einschwinger ab, und das Loslassen der
Taste knackt.

Beide waren **lauter als jedes gesprochene Wort**. Vor dem Schnitt lag die
Spitze jeder Aufnahme bei rund 14000 Zählern, danach bei 2700 bis 4200 — die
Artefakte übertrafen das Nutzsignal um 12 bis 14 dB. Sie gingen bis dahin
unbesehen in die Transkription.

Gesprochen wird in diesen Abschnitten ohnehin nicht: vorne ist die Taste gerade
erst heruntergegangen, hinten geht sie gerade hoch. Wer bis zum letzten Moment
durchspricht, verliert die letzte Silbe — dann ist `kTailMs` in
[src/listen.h](src/listen.h) die Stellschraube.

### Was am Ende gemessen wird

Die Kennzahlen entstehen in einem Durchgang über den fertigen Mitschnitt
(`Listener::measure()`), nicht mitlaufend in `feed()`. Anders ginge es nicht:
erst beim Ende steht fest, wo geschnitten wird, und ein mitlaufender Zähler
hätte den Knacks am Schluss längst eingerechnet. `feed()` bleibt damit im
Audiopfad das, was es sein soll — Kopieren.

Drei Zahlen, weil keine davon allein trägt:

| Zahl | wofür |
|------|-------|
| Spitze | Übersteuerung und Artefakte |
| Effektivwert | die tatsächliche Lautheit |
| Grundrauschen | leisestes 100-ms-Fenster, also eine Sprechpause |

Der Abstand zwischen Effektivwert und Grundrauschen ist der Störabstand, und
das ist die einzige Zahl, die für die Erkennung wirklich zählt. Auf der
Hardware gemessen: Sprache bei −34 dBFS, Grundrauschen zwischen −58 und
−64 dBFS, also **22 bis 29 dB Störabstand**.

Das Fenster statt einer eigenen Stille-Aufnahme, weil es robust ist: ein
einzelner Nadelimpuls verdirbt höchstens ein Fenster, und der Wert fällt in
jeder Aufnahme nebenbei mit ab.

Die Spitze liegt damit bei −18 dBFS, es sind also 18 dB Luft nach oben. Die
bleiben ungenutzt, und das ist Absicht: digitale Verstärkung hebt Sprache und
Rauschen gleichermaßen, der Störabstand ändert sich um kein Dezibel, und die
Realtime-API normalisiert eingehendes Audio ohnehin selbst. Der einzige
wirksame Hebel ist der Abstand zum Mikrofon — jede Halbierung bringt 6 dB.

## Vollduplex: beide Wandler an einem I²S-Port

Auf der Platine gibt es nur ein BCLK, ein LRCLK und ein MCLK. ES7210 (Aufnahme)
und ES8311 (Wiedergabe) hängen am selben Taktpaar, zwei I²S-Controller könnten
dieselben Pins nicht gemeinsam treiben. Der Port wird deshalb einmal angelegt
(`port_begin()` in [src/audio.cpp](src/audio.cpp)) und von beiden Klassen
benutzt; wer zuerst `begin()` ruft, legt die Abtastrate fest.

Zwei Eigenheiten des Treibers haben das mehr Mühe gekostet, als es aussieht:

**Die Konfiguration muss bytegleich sein.** `i2s_new_channel()` liefert TX und
RX auf einmal, aber der Treiber erkennt Vollduplex erst, wenn beide Kanäle eine
`memcmp`-identische `i2s_std_config_t` bekommen — die `gpio_cfg` eingeschlossen.
Also stehen in **beiden** Konfigurationen `dout` und `din`, obwohl je eine
Richtung sie nicht braucht. Der zuerst eingerichtete Kanal bleibt Master, der
zweite wird automatisch zum `full_duplex_slave` herabgestuft.

**Es darf nur ein Datenobjekt geben.** Getrennte `audio_codec_new_i2s_data()`
je Richtung sehen sauberer aus und sind falsch. `i2s_ll_share_bck_ws()` lässt
RX am Takt von TX hängen: wird TX abgeschaltet, verliert das Mikrofon seinen
Takt. `esp_codec_dev` verhindert genau das — aber die Buchführung dazu
(`in_enable`, `out_enable`, „When RX is working TX disable should be blocked")
liegt in **einem** `i2s_data_t`. Mit zwei Objekten weiß keines vom anderen.

Das Fehlerbild war entsprechend: die erste Aufnahme lief, jede weitere lieferte
`0 Frames, Spitze 0` und Lesefehler — das Schließen des Lautsprechers nach der
Wiedergabe hatte dem Mikrofon den Takt abgedreht. Mit einem gemeinsamen
Datenobjekt: sechs Aufnahmen, sechs Wiedergaben, kein Lesefehler.

### Mikrofon nur bei Tastendruck

Der ES7210 wird in `MicInput::start()` geöffnet und in `stop()` wieder
geschlossen, nicht einmalig beim Hochfahren. Zwischen den Aufnahmen
digitalisiert er nichts. Das Öffnen kostet gemessen 26 bis 53 ms und fällt beim
Tastendruck nicht auf; im Log steht `MIK=AN` beziehungsweise `MIK=aus`.

Das ist eine Etappenentscheidung, kein Grundsatz: das fertige Gerät soll auf
ein Weckwort hören und muss dafür dauerhaft digitalisieren. Solange es das noch
nicht tut, ist „das Mikrofon ist zu" die ehrlichere Aussage — und sie ist an
derselben Stelle im Code ablesbar, an der es später aufgeht.

Die Verstärkung steht auf 37,5 dB, dem Maximum des ES7210 (0 bis 33 dB in
Dreierschritten, dann 34,5, 36, 37,5). Sie anzuheben hat den Störabstand nicht
verbessert — Rauschteppich und Signal steigen gemeinsam —, aber sie kostet auch
nichts.

### Der Lautsprecher

Die Wiedergabe der eigenen Aufnahme war eine Zeitlang das einzige Mittel, um zu
beurteilen, was das Mikrofon tatsächlich aufnimmt. Sie ist wieder heraus — der
Lautsprecher trägt jetzt die Antwort, und ein Gerät, das erst die Frage
wiederholt und dann antwortet, ist eine Zumutung.

`volume` bei `esp_codec_dev` ist kein Leistungsanteil, sondern ein Punkt auf
einer Kurve, die 0 bis 100 linear auf −50 bis 0 dB abbildet. Der
zwischenzeitliche Wert 70 waren also nicht „etwas leiser", sondern −15 dB, und
genau so klang es.

### Die Übergabe des Ports zwischen Stimme und Mikrofon

Beide Wandler hängen an *einer* Datenschnittstelle, und `esp_codec_dev` führt
darin Buch, welche Richtung läuft. Wer während einer Antwort die Sprechtaste
drückt, lässt beide im selben Augenblick daran drehen: das Mikrofon öffnet den
Empfangskanal, während die Stimme den Sendekanal zurückgibt. Im Mitschnitt
stand das als

```
I (77249) listen: Zuhoeren gestartet.
E (77249) i2s_common: i2s_channel_disable: the channel has not been enabled yet
I (77454) listen: Zuhoeren beendet: 205 ms, 0 Frames, Spitze 0
```

— und eine Aufnahme mit null Frames heißt: die Nachfrage ist weg. Zwei Dinge
halten das jetzt auseinander. Ein Mutex um jedes `esp_codec_dev_open()` und
`_close()`, damit sich die beiden Vorgänge nicht überlappen. Und eine
Reihenfolge: der Aufnahmetask bricht die Ausgabe ab und **wartet**, bis der
Lautsprecher den Port abgegeben hat, bevor er das Mikrofon öffnet. Das kostet
rund 60 ms und ist der Unterschied zwischen „nachfragen geht" und „nachfragen
geht nicht".

## Sprache zu Text

Freie Transkription auf dem Gerät selbst gibt es nicht: Whisper tiny sind rund
39 MB in int8, der Chip hat 8 MB PSRAM, und Espressifs esp-sr kann hier nur
feste Kommandolisten. Wer freien Text will, braucht einen Dienst — und wer ihn
*während* des Sprechens sehen will, einen, der Teilergebnisse schickt.

Deshalb die **Realtime-API** von OpenAI und nicht `/v1/audio/transcriptions`:
letztere nimmt die fertige Datei und antwortet einmal, hier kommt der Text
stückweise zurück, während noch gesprochen wird. Modell ist
`gpt-4o-mini-transcribe` — mit 0,003 $/min zugleich das günstigste und das
einzige, das Teiltexte liefert; `whisper-1` kostet das Doppelte und kann es
nicht. Fünf Sekunden Sprechen kosten damit rund 0,00025 $.

```
wss://api.openai.com/v1/realtime?intent=transcription
  Authorization: Bearer <key>
  -> session.update             Format, Modell, Sprache, turn_detection: null
  -> input_audio_buffer.append  Base64-PCM16
  -> input_audio_buffer.commit  beim Loslassen der Taste
  <- ...input_audio_transcription.delta      Teiltext
  <- ...input_audio_transcription.completed  Endtext
```

`turn_detection` bleibt aus: Anfang und Ende bestimmt die Taste, nicht eine
Stimmerkennung auf der Gegenseite.

**Kein `OpenAI-Beta: realtime=v1`.** Der Header stand hier, solange die API in
der Beta war, und blieb danach stehen. Er ist nicht bloss ueberfluessig, sondern
der Grund fuer die Ablehnung: *"The Realtime Beta API is no longer supported.
Please use /v1/realtime for the GA API."* Die Erkennung war damit tot, waehrend
alles andere unveraendert aussah.

[src/stt.h](src/stt.h) hängt sich an den `Listener` und macht den Rest allein —
Tastendruck bemerken, Verbindung aufbauen, nachschicken, was während des
Verbindungsaufbaus schon aufgenommen wurde, und nach dem Endtext wieder
schließen. Das läuft in einem eigenen Task auf Kern 0, damit weder Aufnahme noch
Anzeige auf das Netz warten. Der TLS-Handschlag dauert ein bis zwei Sekunden;
weil der Mitschnitt vollständig im PSRAM steht, kostet das trotzdem keine Silbe:
ein Sendezeiger holt den Rückstand auf, sobald die Sitzung steht.

### Zugangsdaten

`src/secrets.h` ist bewusst **nicht** im Repository. Vorlage kopieren und
ausfüllen:

```bash
cp src/secrets.h.example src/secrets.h
```

Ohne `WIFI_SSID` bleibt das Gerät offline, ohne `OPENAI_API_KEY` wird
aufgenommen, aber nicht erkannt — die übrige Firmware läuft in beiden Fällen
unverändert weiter, und der Bring-up sagt im Log, was fehlt.

### Anzeige während der Aufnahme

Das obere Band wechselt für die Dauer der Aufnahme die Einteilung: statt der
Kennzahlen stehen dort ein blinkender Aufnahmepunkt, ein Zeitbalken mit
Sekundenmarken bis zur Zehn-Sekunden-Schranke, eine Zeile über den Zustand der
Erkennung und darunter der Text, so wie er hereinkommt. Wer spricht, schaut auf
den Text und nicht auf die Batteriespannung.

Der Text wird mit Wortumbruch gesetzt und zeigt bei Überlänge die **letzten**
acht Zeilen — bei einem laufenden Transkript ist das Ende das Interessante.
Acht Sekunden nach dem Abschluss kehrt die Anzeige zu den Kennzahlen zurück;
kürzer wäre das Ergebnis weg, bevor es gelesen ist.

Der Font kennt dafür vier zusätzliche Zeichen: ä, ö, ü und ß. Die großen
Umlaute passen nicht in sieben Zeilen und werden zu Ae, Oe, Ue umgeschrieben,
alles andere außerhalb von ASCII wird ein Fragezeichen.

## Von der Frage zur Antwort

Drei Aufrufe, nicht einer. Es gäbe eine Sprache-zu-Sprache-Sitzung, die alles
in einem macht und unter einer Sekunde antwortet — sie kostet aber das
Fünfzig- bis Hundertfache je Runde, und vor allem fiele der Text unterwegs
weg. Auf einem Gerät, dessen Hauptausgabe ein Display ist, ist der Text nicht
das Nebenprodukt, sondern der Zweck.

```
Taste  --> stt.cpp --> chat.cpp --> tts.cpp --> Lautsprecher
           wss:.../realtime  POST /v1/chat/completions  POST /v1/audio/speech
           Teiltext          Strom aus Satzstuecken     PCM 24 kHz roh
```

Jeder Schritt hängt am vorigen über einen Zähler, nicht über einen Aufruf:
`Stt::final_seq()` wechselt, wenn ein Endtext dasteht, `Chat::runde_seq()`,
wenn eine neue Antwort beginnt. Damit läuft jeder Schritt in seinem eigenen
Task, und keiner wartet auf den anderen, solange es nichts zu tun gibt.

**Der Systemhinweis ist Gerätekunde.** Der Font kennt ASCII plus ä ö ü ß,
sonst nichts; eine Antwort mit Aufzählungszeichen, typografischen
Anführungsstrichen oder einem Emoji wäre auf diesem Display eine Reihe
Fragezeichen. Der Hinweis verbietet sie deshalb ausdrücklich und bittet um
höchstens drei Sätze — mehr passt nicht ins Band.

**`response_format: "pcm"`** und nicht mp3 oder opus: das Format ist dann
genau das, was der ES8311 ohnehin bekommt — 24 kHz, 16 Bit, mono, Little
Endian, ohne Kopf. Ein Decoder auf dem Gerät entfällt vollständig, und mit ihm
die Frage, ob er schnell genug ist. Der Preis sind 48 KB je Sekunde statt 4.

### Warum der Ton geknackt hat

Der erste Entwurf schrieb den Ton direkt aus dem HTTP-Lesevorgang in den
Wandler. Das knackte hörbar und unregelmäßig, und der Grund ist eine Zahl: der
I2S-Treiber hält mit `I2S_CHANNEL_DEFAULT_CONFIG` sechs mal 240 Frames vor,
bei 24 kHz also **60 Millisekunden**. Jede Stockung im Netz, die länger dauert
als das — und über WLAN mit TLS sind hundert Millisekunden nichts Besonderes
—, läuft der DMA leer, und ein leerer DMA klingt wie ein Knacken.

Dazwischen liegt jetzt ein Ringpuffer im PSRAM, acht Sekunden groß: ein Task
füllt ihn aus dem Netz, ein zweiter leert ihn in den Lautsprecher. Ein
Schreiber, ein Leser, zwei frei laufende 32-Bit-Zähler — damit braucht es
keine Sperre, und der Überlauf stört nicht, weil immer nur die Differenz
gebildet wird. Am Schluss gehen 20 ms Stille hinterher: ein Wandler, der
mitten im Signal stehenbleibt, tut das mit einem Knacks.

## Antwortzeit

Gemessen vom Loslassen der Taste bis zum ersten Ton, dieselbe Frage vorher und
nachher:

| Abschnitt | vorher ms | jetzt ms |
|---|---:|---:|
| Loslassen → Endtext | 536 | 699 |
| Chat: TLS-Handschlag | 793 | — |
| Chat: Anfrage → Antwort | 2320 | 1240 |
| Stimme: TLS-Handschlag | 802 | — |
| Stimme: erstes Byte + Vorlauf | 1745 | 964 |
| **Summe** | **6196** | **2903** |

Die interessante Zahl stand zweimal da. **1,6 Sekunden für zwei
TLS-Handschläge**, beide zum selben Host, beide mitten auf dem Weg. Beide sind
jetzt weg, und zwar durch zwei Dinge, die in
[src/verbindung.h](src/verbindung.h) stehen:

- **Stehen lassen.** `esp_http_client` baut nur dann neu auf, wenn der Zustand
  unter `HTTP_STATE_CONNECTED` liegt. Wer die Antwort zu Ende liest und danach
  *nicht* schließt, bekommt beim nächsten `open()` denselben Socket. Das setzt
  voraus, dass wirklich alles gelesen wurde — beim Ereignisstrom des Chats
  also auch der Schlusschunk hinter `[DONE]`, den der alte Code liegen ließ.
- **Vorwärmen, sobald das WLAN steht.** Der Aufbau wird in eine Zeit
  vorgezogen, in der ohnehin nichts zu tun ist: direkt nach dem Hochfahren.
  Erst während der Aufnahme vorzuwärmen war zu knapp — ein Handschlag kostet
  hier 0,8 bis 3 Sekunden, und wenn Chat und Stimme gleichzeitig aufbauen,
  über vier; beim ersten Tastendruck nach dem Einschalten war er dann noch
  nicht fertig und verzögerte genau die Frage, die er beschleunigen sollte.
  Vorgewärmt wird mit einem `GET /v1/models/<modell>`; was es antwortet, ist
  gleichgültig, auch eine 404 hält den Socket offen.

Eine stehende Verbindung kann die Gegenseite jederzeit zumachen, ohne dass man
es merkt. Deshalb gilt der erste fehlgeschlagene Versuch auf einer
wiederverwendeten Verbindung nicht als Fehler, sondern als Anlass, einmal neu
aufzubauen.

**Satzweise sprechen.** Die Stimme wartete früher auf die vollständige Antwort
und verschenkte damit rund anderthalb Sekunden, in denen der erste Satz längst
fertig war. Jetzt meldet der Chat, wie viele Zeichen als *ganze Sätze*
feststehen, und die Stimme holt, was da ist — die zweite Portion, während die
erste noch läuft, in denselben Ring, ohne Naht dazwischen. Ein Satzzeichen
gilt dabei nur als Ende, wenn ein Leerzeichen folgt und seit der letzten
Grenze mindestens 16 Zeichen vergangen sind; sonst zerfiele "z. B." in zwei
Portionen und jede kostete eine eigene Anfrage.

**Der Vorlauf** ist der verbleibende Handel: er verzögert den ersten Ton um
genau seine Länge und kauft dafür denselben Betrag an Stockungstoleranz. Von
2 s auf 1,2 s heruntergesetzt — und damit das keine Glaubensfrage bleibt,
zählt der Spieler mit, wie oft der Ring mitten im Sprechen leer lief:

```
I (...) tts: Gesprochen: 3100 ms Ton, erster Ton nach 964 ms, 0 Stockungen, 103 KB intern frei.
```

### Wer wem den Vortritt lässt

Der Aufnahmetakt lief anfangs im Haupttask, und der hat in der IDF **Priorität
1 auf Kern 0** — unter allem, was das Netz anfasst. Chat, Erkennung und Stimme
liegen dort auf 3. Beim ersten Tastendruck nach dem Einschalten laufen die
Handschläge aller drei gleichzeitig, jeder über eine Sekunde reine
Rechenarbeit, und der Aufnahmetakt kam in dieser Zeit nicht mehr dran. Der
I2S-Ring fasst 60 ms; alles darüber hinaus verfällt. Das Ergebnis:

```
I (45512) listen: Zuhoeren gestartet.
I (46333) bringup: Pegel rms=  207  peak= 7737   |  Bild 36/44 ms, 38/s
I (47520) bringup: Pegel rms=   20  peak=   52   |  Bild 0/0 ms, 0/s
I (47989) listen: Zuhoeren beendet: 2477 ms, 0 Frames, Spitze 0
E (48175) stt: Dienstfehler: ... buffer only has 0.00ms of audio.
```

Zweieinhalb Sekunden Aufnahme, null Frames — und der Pegelmesser zeigte im
selben Augenblick Sprache an. Die paar Blöcke, die durchkamen, reichten für
den Messwert, nicht für den Mitschnitt.

Der Aufnahmetakt hat deshalb einen eigenen Task, **Priorität 6 auf Kern 1**,
über der Anzeige. Kern 1 hat außer dem Zeichnen nichts zu tun, und die
Reihenfolge dort stimmt: ein ausgelassenes Bild fällt nicht auf, eine
verlorene Silbe schon. Der Haupttask kehrt danach zurück; die IDF räumt ihn
samt seinen acht Kilobyte Stack ab.

Die Anzeige stockt unter drei gleichzeitigen Handschlägen weiterhin sichtbar
(`Bild 113/1177 ms, 15/s` für eine Sekunde). Das kostet nichts als Glätte und
steht unter den offenen Punkten.

### Was das Gespräch zusammenhält

Sechs Frage-Antwort-Wechsel gehen bei jeder Anfrage wieder mit hinaus. Das
Gerät hat eine Sprechtaste und keinen sichtbaren Faden — wer nachfragt,
bezieht sich fast immer auf das Vorige. Im Log steht mit jeder Antwort, wie
viele Wechsel mitgingen; ohne diese Zahl wäre nicht zu unterscheiden, ob der
Verlauf fehlt oder das Modell ihn ignoriert.

```
I (63172) stt: Endtext: Kannst du sie benennen?
I (65485) chat: Antwort nach 2311 ms (erster Satz nach 0 ms, 2 Wechsel Verlauf):
                Ja, sie heissen Merkur, Venus, Erde, Mars, ...
```

Wer während einer laufenden Antwort die Taste drückt, bricht sie ab — wer
spricht, will nicht zuhören. Die Erkennung schließt dann auch eine Sitzung,
die noch auf ihren Endtext wartet; ohne das bliebe eine offene Verbindung
stehen, deren Ereignisse weiterhin hereinkämen.

**Die Sitzung darf langsamer sein als der Tastendruck.** Eine Erkennungssitzung
aufzubauen dauert rund 1,9 Sekunden — 0,8 s TLS, der Rest WebSocket-Aufstieg
und die Antwort auf `transcription_session.update`. Wer kurz nachfragt, lässt
vorher los. Früher endete das an dieser Stelle mit `Sitzung beendet (ohne
Sitzung)`, und die anderthalb Sekunden Aufnahme, die sauber im PSRAM lagen,
waren weg — ohne Antwort und ohne ein Zeichen, dass überhaupt etwas angekommen
war. Das Loslassen setzt den Abschluss jetzt nur auf *offen*; das `commit`
geht hinaus, sobald die Sitzung steht und der letzte Frame drüben ist.
Aufgegeben wird erst nach fünf Sekunden.

## Log auf dem Display

`esp_log_set_vprintf()` gibt den bisherigen Handler zurück. Damit lässt sich
das Log *abzweigen* statt umzuleiten: der Hook reicht zuerst an den alten
Handler weiter und legt sich danach eine Kopie in einen Ring von 40 Zeilen.
Die serielle Diagnose bleibt dabei vollständig erhalten.

Das Log ist die Standardansicht, nicht die Kennzahlen — das Wellenband ist
leer, solange das Mikrofon zu ist, und das ist die meiste Zeit. BOOT schaltet
um; während der Einrichtung gewinnt immer die Kennzahlenansicht, weil nur dort
Gerätename und Nachweis stehen.

Zwei Dinge waren dabei nicht offensichtlich. Der **`sys_evt`-Task hat 2304
Byte Stack**, und der Hook läuft auch in ihm — deshalb ein Zwischenpuffer von
160 Byte und nichts Größeres. Und der Pegel-Herzschlag schreibt 200 Zeichen je
Sekunde; er füllt 28 Zeilen in einer halben Minute. Statt die Zeile zu
streichen, kann `logview::mute()` einzelne Anfänge unterdrücken — im Log auf
der seriellen Schnittstelle stehen sie weiter.


## Offene Punkte

- **Controller-Bezeichnung**: Waveshare und die ESPHome-Komponente nennen den
  Display-Controller ST7305, die Zephyr-Doku ST7306 (4 Graustufen statt rein
  monochrom). Praktisch irrelevant, solange der Waveshare-Treiber läuft — beim
  Umstieg auf einen generischen Treiber aber zu klären.
- **Batterie-ADC**: Das Teilerverhältnis am ADC ist nicht dokumentiert, deshalb
  wird bisher nur der Rohwert geloggt. Für eine Spannungsangabe muss der Faktor
  am Schaltplan oder empirisch bestimmt werden.
- **Weckwort „HoiHoi"**: zurückgestellt, ausgelöst wird vorerst über die Taste.
  Für „HoiHoi" gibt es kein fertiges Modell: Espressifs WakeNet kennt ab Werk
  nur Hi ESP, Alexa, Jarvis, Computer, Sophia und einige weitere, und ein
  eigenes Weckwort ist dort ein kostenpflichtiger Dienst — mindestens 20 000
  Sprachaufnahmen, zwei bis drei Wochen Training. MultiNet nimmt zwar eigene
  Kommandos als Text an, setzt laut Doku aber zwingend ein WakeNet davor und
  taugt nicht als eigenständiger Wortdetektor. Bleiben zwei Wege: ein auf die
  eigene Stimme eingelernter Erkenner (MFCC plus DTW gegen selbst gesprochene
  Vorlagen, sprecherabhängig, dafür ohne Lizenz und sofort machbar) oder ein
  selbst trainiertes Modell nach Art von microWakeWord, das aus
  TTS-erzeugten Beispielen entsteht und sprecherunabhängig arbeitet, dafür aber
  eine Trainingspipeline außerhalb der Firmware braucht.
- **Stockende Anzeige beim ersten Handschlag**: laufen Erkennung, Chat und
  Stimme gleichzeitig durch ihren TLS-Aufbau, fällt die Bildrate für rund eine
  Sekunde auf 15/s, einzelne Bilder brauchen über eine Sekunde. Die Aufnahme
  ist davon nicht mehr betroffen, die Anzeige schon. Der Anzeigetask läuft auf
  Kern 1, die Handschläge auf Kern 0 — die Kopplung dürfte über die
  Heap-Sperre laufen, die mbedTLS mit `CONFIG_MBEDTLS_DYNAMIC_BUFFER` stark
  belastet. Nicht nachgemessen.
- **Erkennungssitzung vorwärmen**: der WebSocket wird bei jedem Tastendruck
  neu aufgebaut und kostet 1,9 s — die größte verbliebene Einzelzeit. Chat und
  Stimme halten ihre Verbindung stehen; für die Erkennung wäre dasselbe
  möglich, braucht aber eine Antwort darauf, wie lange die Gegenseite eine
  unbenutzte Transkriptionssitzung offen lässt.
- **microSD und RTC**: noch nicht angebunden.
