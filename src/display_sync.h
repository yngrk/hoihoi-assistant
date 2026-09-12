#pragma once

// ---------------------------------------------------------------------------
// DisplayPort mit Rueckmeldung ueber das Ende der DMA-Uebertragung.
//
// DisplayPort::RLCD_Display() schickt den Bildpuffer ueber
// esp_lcd_panel_io_tx_color() los. Der Aufruf reiht die Uebertragung nur ein
// und kehrt sofort zurueck; 15000 Byte bei 10 MHz brauchen danach noch rund
// 12 ms, waehrend derer das DMA aus DispBuffer liest. Der Zeichencode lief in
// dieser Zeit bereits weiter und hat mit RLCD_ColorClear() denselben Puffer
// ueberschrieben — das Panel bekam also die obere Bildhaelfte aus dem alten
// und die untere aus dem neuen Bild.
//
// Der esp_lcd-Treiber bietet dafuer on_color_trans_done an, aufgerufen aus der
// SPI-Unterbrechungsroutine, wenn das letzte Teilstueck durch ist. Registriert
// werden kann der Rueckruf nur ueber io_handle, und das liegt im
// Waveshare-Treiber hinter einem Sichtbarkeitsmodifikator — deshalb die
// Ableitung hier statt einer Aenderung am Treiber selbst.
//
// Das Warten kostet nichts: von den 37 ms Bildperiode gehen 12 ms fuer die
// Uebertragung und rund 4 ms fuers Zeichnen drauf, der Rest war ohnehin
// Warten auf die naechste Austastluecke.
// ---------------------------------------------------------------------------

#include <stdint.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "display_bsp.h"

class SyncDisplay : public DisplayPort {
  public:
    using DisplayPort::DisplayPort;

    // Haengt den Abschlussrueckruf an den Panel-IO. Danach wartet
    // send_and_wait() tatsaechlich; vorher verhaelt es sich wie
    // RLCD_Display().
    esp_err_t enable_transfer_wait();

    // Legt den Bildpuffer in DMA-faehigen internen Speicher um.
    //
    // Der Treiber legt ihn im PSRAM an, und daraus kann das SPI-DMA nicht
    // lesen: spi_master besorgt sich deshalb bei *jedem* Bild einen eigenen
    // internen Zwischenpuffer von 15 KB — siebenundzwanzigmal je Sekunde
    // anlegen und wieder freigeben. Geht das einmal nicht, endet es nicht
    // etwa mit einem ausgelassenen Bild, sondern mit
    //
    //   E spi_master: setup_dma_priv_buffer: Failed to allocate priv TX buffer
    //   ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM ... abort()
    //
    // im Treiber, und damit mit einem Neustart. Einmalig 15 KB intern beim
    // Hochfahren kosten weniger als diese Unruhe — und die Anzeige haengt
    // danach an keiner Speicherlage mehr.
    esp_err_t pin_buffer_to_dma();

    // Schickt den Bildpuffer los und kehrt erst zurueck, wenn das DMA ihn
    // vollstaendig gelesen hat. Ab dann darf wieder hineingezeichnet werden.
    void send_and_wait();

    // Anzahl der Uebertragungen, deren Ende nicht gemeldet wurde.
    uint32_t dma_timeouts() const { return dma_timeouts_; }

  private:
    static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *ctx);

    SemaphoreHandle_t dma_sem_     = nullptr;
    volatile uint32_t dma_timeouts_ = 0;
};
