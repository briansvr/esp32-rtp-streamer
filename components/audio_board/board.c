#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"
#include "periph_button.h"

static const char *TAG = "AUDIOKIT_ES8388";

static audio_board_handle_t board_handle = NULL;

audio_board_handle_t audio_board_init(void)
{
    if (board_handle) {
        ESP_LOGW(TAG, "Board already initialized");
        return board_handle;
    }
    board_handle = audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
    board_handle->audio_hal = audio_board_codec_init();
    return board_handle;
}

audio_hal_handle_t audio_board_codec_init(void)
{
    audio_hal_codec_config_t cfg = AUDIO_CODEC_DEFAULT_CONFIG();
    audio_hal_handle_t hal = audio_hal_init(&cfg, &AUDIO_CODEC_ES8388_DEFAULT_HANDLE);
    AUDIO_NULL_CHECK(TAG, hal, return NULL);
    return hal;
}

esp_err_t audio_board_key_init(esp_periph_set_handle_t set)
{
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = (1ULL << BUTTON_REC_ID)     |
                     (1ULL << BUTTON_MODE_ID)    |
                     (1ULL << BUTTON_SET_ID)     |
                     (1ULL << BUTTON_PLAY_ID)    |
                     (1ULL << BUTTON_VOLUP_ID)   |
                     (1ULL << BUTTON_VOLDOWN_ID),
    };
    esp_periph_handle_t btn = periph_button_init(&btn_cfg);
    AUDIO_NULL_CHECK(TAG, btn, return ESP_ERR_NO_MEM);
    return esp_periph_start(set, btn);
}

audio_board_handle_t audio_board_get_handle(void)
{
    return board_handle;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    esp_err_t ret = audio_hal_deinit(audio_board->audio_hal);
    audio_free(audio_board);
    board_handle = NULL;
    return ret;
}
