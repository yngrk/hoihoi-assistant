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
src/net.h, .cpp           WLAN im Stationsbetrieb
src/stt.h, .cpp           Sprache zu Text über die Realtime-API von OpenAI
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
digitalisiert er nichts. Ein Gerät mit Mikrofon soll nicht dauerhaft zuhören,
und ob es das tut, darf man nicht glauben müssen — es ist derselbe Baustein,
der sonst läuft. Das Öffnen kostet gemessen 26 bis 53 ms und fällt beim
Tastendruck nicht auf; im Log steht `MIK=AN` beziehungsweise `MIK=aus`.

Die Verstärkung steht auf 37,5 dB, dem Maximum des ES7210 (0 bis 33 dB in
Dreierschritten, dann 34,5, 36, 37,5). Sie anzuheben hat den Störabstand nicht
verbessert — Rauschteppich und Signal steigen gemeinsam —, aber sie kostet auch
nichts.

### Wiedergabe als Diagnosemittel

Nach dem Loslassen spielt das Gerät die Aufnahme über den ES8311 zurück. Das
ist kein Bestandteil des Endprodukts, sondern das einzige Mittel, mit dem sich
beurteilen lässt, was das Mikrofon tatsächlich aufgenommen hat.

`volume` bei `esp_codec_dev` ist dabei kein Leistungsanteil, sondern ein Punkt
auf einer Kurve, die 0 bis 100 linear auf −50 bis 0 dB abbildet. Der
zwischenzeitliche Wert 70 waren also nicht „etwas leiser", sondern −15 dB, und
genau so klang es.

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
  Authorization: Bearer <key>,  OpenAI-Beta: realtime=v1
  -> session.update             Format, Modell, Sprache, turn_detection: null
  -> input_audio_buffer.append  Base64-PCM16
  -> input_audio_buffer.commit  beim Loslassen der Taste
  <- ...input_audio_transcription.delta      Teiltext
  <- ...input_audio_transcription.completed  Endtext
```

`turn_detection` bleibt aus: Anfang und Ende bestimmt die Taste, nicht eine
Stimmerkennung auf der Gegenseite.

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
- **microSD und RTC**: noch nicht angebunden.
