#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include <string.h>
#include "board.h"
#include "audio_error.h"

static const char *TAG = "AUDIOKIT_ES8388_PINS";

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    AUDIO_NULL_CHECK(TAG, i2c_config, return ESP_FAIL);
    if (port == I2C_NUM_0) {
        i2c_config->scl_io_num = GPIO_NUM_32;
        i2c_config->sda_io_num = GPIO_NUM_33;
    } else {
        ESP_LOGE(TAG, "I2C port %d not supported", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    AUDIO_NULL_CHECK(TAG, i2s_config, return ESP_FAIL);
    if (port == 0) {
        i2s_config->mck_io_num   = GPIO_NUM_0;
        i2s_config->bck_io_num   = GPIO_NUM_27;
        i2s_config->ws_io_num    = GPIO_NUM_25;
        i2s_config->data_out_num = GPIO_NUM_26;
        i2s_config->data_in_num  = GPIO_NUM_35;
    } else {
        memset(i2s_config, -1, sizeof(board_i2s_pin_t));
        ESP_LOGE(TAG, "I2S port %d not supported", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_spi_pins(spi_bus_config_t *spi_config, spi_device_interface_config_t *spi_dev_cfg)
{
    /* SD card not used in this project */
    if (spi_config) {
        spi_config->mosi_io_num   = -1;
        spi_config->miso_io_num   = -1;
        spi_config->sclk_io_num   = -1;
        spi_config->quadwp_io_num = -1;
        spi_config->quadhd_io_num = -1;
    }
    if (spi_dev_cfg) {
        spi_dev_cfg->spics_io_num = -1;
    }
    return ESP_OK;
}

int8_t get_pa_enable_gpio(void)        { return PA_ENABLE_GPIO; }
int8_t get_auxin_detect_gpio(void)     { return AUXIN_DETECT_GPIO; }
int8_t get_headphone_detect_gpio(void) { return HEADPHONE_DETECT; }
int8_t get_green_led_gpio(void)        { return GREEN_LED_GPIO; }

int8_t get_input_rec_id(void)          { return BUTTON_REC_ID; }
int8_t get_input_mode_id(void)         { return BUTTON_MODE_ID; }
int8_t get_input_set_id(void)          { return BUTTON_SET_ID; }
int8_t get_input_play_id(void)         { return BUTTON_PLAY_ID; }
int8_t get_input_volup_id(void)        { return BUTTON_VOLUP_ID; }
int8_t get_input_voldown_id(void)      { return BUTTON_VOLDOWN_ID; }

/* SD not used — return sentinel values */
int8_t get_sdcard_intr_gpio(void)      { return -1; }
int8_t get_sdcard_open_file_num_max(void) { return 0; }
