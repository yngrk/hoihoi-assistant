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
components/port_bsp/      ST7305-Treiber, unverändert von Waveshare übernommen
sdkconfig.defaults        Flash-, PSRAM- und Konsolenkonfiguration
partitions.csv            8 MB App-Partition
```

Der Display-Treiber (`components/port_bsp/`) stammt aus Waveshares
ESP-IDF-Beispiel `09_LVGL_V9_Test` und ist absichtlich unverändert, damit
Updates von dort einfach nachgezogen werden können. Die Kommentare darin sind
chinesisch.

## Was der Bring-up prüft

1. **Chip und Speicher** — Kerne, Revision, Flash-Größe, PSRAM-Initialisierung
   und freier SPIRAM. Meldet ausdrücklich, wenn weniger als 8 MB erkannt werden.
2. **I²C-Scan** — tastet `0x08`–`0x77` ab und benennt die vier erwarteten
   Bausteine namentlich.
3. **SHTC3-Messung** — Wakeup, Messbefehl, Auslesen, Sleep. Beweist, dass der
   Bus nicht nur ACKt, sondern plausible Werte liefert.
4. **Display** — Testbild aus Rahmen, zwei Diagonalen und einem Schachbrett.
   Der Rahmen prüft die Ränder, die Diagonalen die Adressierung, das Schachbrett
   die Bit-Packung innerhalb eines Bytes.
5. **Tasten und Batterie** — Dauerschleife, alle zwei Sekunden ein Log.

## Offene Punkte

- **Controller-Bezeichnung**: Waveshare und die ESPHome-Komponente nennen den
  Display-Controller ST7305, die Zephyr-Doku ST7306 (4 Graustufen statt rein
  monochrom). Praktisch irrelevant, solange der Waveshare-Treiber läuft — beim
  Umstieg auf einen generischen Treiber aber zu klären.
- **Batterie-ADC**: Das Teilerverhältnis am ADC ist nicht dokumentiert, deshalb
  wird bisher nur der Rohwert geloggt. Für eine Spannungsangabe muss der Faktor
  am Schaltplan oder empirisch bestimmt werden.
- **Audio**: ES8311/ES7210 sind noch nicht initialisiert. Referenz dafür ist
  Waveshares Beispiel `07_Audio_Test`.
- **microSD und RTC**: noch nicht angebunden.
