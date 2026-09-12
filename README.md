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

Geflasht wird über den nativen USB-Serial/JTAG-Port des ESP32-S3
(VID `303A` / PID `1001`). Der Port ist in `platformio.ini` fest auf `COM6`
gesetzt; bei anderem Port dort anpassen oder die Zeilen entfernen, dann sucht
PlatformIO selbst.

## Aufbau

```
src/main.cpp              Bring-up-Ablauf
src/user_config.h         Pinbelegung
src/gfx.h, src/gfx.cpp    Clippende Zeichenschicht über dem Treiber
src/font5x7.h, .cpp       5×7-Bitmapfont, ASCII 0x20–0x7F
src/audio.h, src/audio.cpp  Mikrofoneingang: ES7210 über I²C, Daten über I²S
src/idf_component.yml     Abhängigkeit auf espressif/esp_codec_dev
components/port_bsp/      ST7305-Treiber, unverändert von Waveshare übernommen
sdkconfig.defaults        Flash-, PSRAM- und Konsolenkonfiguration
partitions.csv            8 MB App-Partition
```

Der Display-Treiber (`components/port_bsp/`) stammt aus Waveshares
ESP-IDF-Beispiel `09_LVGL_V9_Test` und ist absichtlich unverändert, damit
Updates von dort einfach nachgezogen werden können. Die Kommentare darin sind
chinesisch.

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
| stats | 0–227 | Kopfzeile, Umweltwerte links, Systemzustand rechts |
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

16 kHz, 16 Bit, beide Kanäle zu Mono gemittelt. 40 Frames je Bildspalte mal 400
Spalten ergeben genau eine Sekunde Signal über die volle Breite; das Bild läuft
nach links weg.

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

Der Vollausschlag skaliert automatisch — sofort auf, langsam wieder zu, nie
unter einen Sockelwert. Die Empfindlichkeit der Mikrofone ist nicht
dokumentiert, ein fester Faktor würde also entweder in Stille das Grundrauschen
aufblasen oder bei Sprache am Anschlag kleben.

Auf der Hardware gemessen: Grundrauschen bei −63 dBFS, Raumgeräusch um
−40 dBFS, laute Sprache −21 dBFS — über 40 dB nutzbarer Dynamikumfang. Das
Zeichnen selbst kostet rund 4 ms, der Rest der 37 ms ist gewolltes Warten auf
die Austastlücke.

## Offene Punkte

- **Controller-Bezeichnung**: Waveshare und die ESPHome-Komponente nennen den
  Display-Controller ST7305, die Zephyr-Doku ST7306 (4 Graustufen statt rein
  monochrom). Praktisch irrelevant, solange der Waveshare-Treiber läuft — beim
  Umstieg auf einen generischen Treiber aber zu klären.
- **Batterie-ADC**: Das Teilerverhältnis am ADC ist nicht dokumentiert, deshalb
  wird bisher nur der Rohwert geloggt. Für eine Spannungsangabe muss der Faktor
  am Schaltplan oder empirisch bestimmt werden.
- **Keyword-Erkennung**: Der Mikrofonpfad steht, die eigentliche
  Schlüsselworterkennung fehlt noch. Die Visualisierung ist der erste Schritt
  dorthin und belegt, dass brauchbares Signal ankommt.
- **Audioausgabe**: Der ES8311 ist noch nicht initialisiert, nur der ES7210 für
  die Aufnahme. Referenz dafür ist Waveshares Beispiel `07_Audio_Test`.
- **microSD und RTC**: noch nicht angebunden.
