#include "display_sync.h"

#include <string.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

bool IRAM_ATTR SyncDisplay::on_trans_done(esp_lcd_panel_io_handle_t,
                                          esp_lcd_panel_io_event_data_t *,
                                          void *ctx)
{
    SyncDisplay *self = (SyncDisplay *)ctx;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(self->dma_sem_, &woken);
    if (woken == pdTRUE) {
        // Der SPI-Treiber wertet den Rueckgabewert nicht aus, also hier selbst
        // umschalten — sonst wartet der Anzeigetask bis zum naechsten Tick.
        portYIELD_FROM_ISR();
    }
    return woken == pdTRUE;
}

esp_err_t SyncDisplay::enable_transfer_wait()
{
    if (dma_sem_ != nullptr) {
        return ESP_OK;
    }

    dma_sem_ = xSemaphoreCreateBinary();
    if (dma_sem_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done          = on_trans_done;

    const esp_err_t err = esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, this);
    if (err != ESP_OK) {
        vSemaphoreDelete(dma_sem_);
        dma_sem_ = nullptr;
    }
    return err;
}

void SyncDisplay::send_and_wait()
{
    RLCD_Display();

    if (dma_sem_ == nullptr) {
        return;
    }

    // Der Rueckruf kommt genau einmal je Uebertragung, das Semaphor ist damit
    // ausgeglichen und kann keinen Rest aus dem Vorbild enthalten. Trotzdem
    // mit Zeitschranke: bleibt die Meldung aus, soll die Anzeige langsamer
    // werden, nicht stehen bleiben — 12 ms Nutzdauer gegen 100 ms Schranke.
    if (xSemaphoreTake(dma_sem_, pdMS_TO_TICKS(100)) != pdTRUE) {
        dma_timeouts_++;
    }
}

esp_err_t SyncDisplay::pin_buffer_to_dma()
{
    if (DispBuffer == nullptr || DisplayLen <= 0) return ESP_ERR_INVALID_STATE;
    if (esp_ptr_dma_capable(DispBuffer)) return ESP_OK;

    uint8_t *neu = (uint8_t *)heap_caps_malloc((size_t)DisplayLen,
                                               MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (neu == nullptr) return ESP_ERR_NO_MEM;

    memcpy(neu, DispBuffer, (size_t)DisplayLen);
    heap_caps_free(DispBuffer);
    DispBuffer = neu;

    ESP_LOGI("display", "Bildpuffer im internen Speicher (%d Byte).", DisplayLen);
    return ESP_OK;
}
