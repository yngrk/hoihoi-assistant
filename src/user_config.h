#pragma once

// ---------------------------------------------------------------------------
// Waveshare ESP32-S3-RLCD-4.2 — Pinbelegung
// Quelle: Waveshare ESP-IDF-Beispiele (02_Example/ESP-IDF/*/main/user_config.h)
// und die offizielle ESPHome-Konfiguration von Waveshare.
// ---------------------------------------------------------------------------

// Display: 400x300 monochrom, reflektiv, ST7305 (per SPI)
#define LCD_WIDTH       400
#define LCD_HEIGHT      300

#define RLCD_DC_PIN     GPIO_NUM_5
#define RLCD_CS_PIN     GPIO_NUM_40
#define RLCD_SCK_PIN    GPIO_NUM_11
#define RLCD_MOSI_PIN   GPIO_NUM_12
#define RLCD_RST_PIN    GPIO_NUM_41
#define RLCD_TE_PIN     GPIO_NUM_6    // Tearing-Effect, ausgewertet in Canvas (gfx.h)

// I2C: SHTC3 (0x70), PCF85063 RTC (0x51), ES8311 Codec (0x18), ES7210 ADC (0x40)
#define I2C_SDA_PIN     GPIO_NUM_13
#define I2C_SCL_PIN     GPIO_NUM_14

// I2S-Audio (in dieser Bring-up-Stufe noch ungenutzt)
#define I2S_MCLK_PIN    GPIO_NUM_16
#define I2S_BCLK_PIN    GPIO_NUM_9
#define I2S_LRCLK_PIN   GPIO_NUM_45
#define I2S_DIN_PIN     GPIO_NUM_10   // Mikrofon-Array -> ESP
#define I2S_DOUT_PIN    GPIO_NUM_8    // ESP -> Lautsprecher
#define AMP_ENABLE_PIN  GPIO_NUM_46   // high = Verstaerker an

// Bedienelemente und Versorgung
#define BOOT_BUTTON_PIN GPIO_NUM_0    // active low
#define KEY_BUTTON_PIN  GPIO_NUM_18   // active low
#define BATTERY_ADC_PIN GPIO_NUM_4    // ADC1 Kanal 3
