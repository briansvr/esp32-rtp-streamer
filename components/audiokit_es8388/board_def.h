#pragma once

/**
 * AudioKit ES8388 v1 — board hardware definitions
 *
 * Pin mapping derived from PinsAudioKitEs8388v1Class:
 *   I2C:  SCL=32, SDA=33
 *   I2S:  MCLK=0, BCK=27, WS=25, DATA_OUT=26, DATA_IN=35
 *   PA:   GPIO 21 (active-high)
 *   LED:  GPIO 22
 *   Keys: 36, 13, 19, 23, 18, 5 (active-low)
 *   AUXIN_DETECT:      GPIO 12 (active-low)
 *   HEADPHONE_DETECT:  GPIO 39 (active-low)
 */

#include "audio_hal.h"

/* ── Codec I2S port ──────────────────────────────────────── */
#define CODEC_ADC_I2S_PORT        ((i2s_port_t)0)
#define CODEC_ADC_BITS_PER_SAMPLE ((i2s_data_bit_width_t)16)
#define CODEC_ADC_SAMPLE_RATE     (44100)
#define RECORD_HARDWARE_AEC       (false)
#define BOARD_PA_GAIN             (0)

/* ── PA / headphone / aux-in ─────────────────────────────── */
#define FUNC_AUDIO_CODEC_EN       (1)
#define PA_ENABLE_GPIO            GPIO_NUM_21
#define HEADPHONE_DETECT          GPIO_NUM_39
#define AUXIN_DETECT_GPIO         GPIO_NUM_12

/* ── LED ─────────────────────────────────────────────────── */
#define FUNC_SYS_LEN_EN           (1)
#define GREEN_LED_GPIO            GPIO_NUM_22

/* ── Buttons (6 physical keys, active-low) ───────────────── */
#define FUNC_BUTTON_EN            (1)
#define INPUT_KEY_NUM             6
#define BUTTON_REC_ID             GPIO_NUM_36
#define BUTTON_MODE_ID            GPIO_NUM_13
#define BUTTON_SET_ID             GPIO_NUM_19
#define BUTTON_PLAY_ID            GPIO_NUM_23
#define BUTTON_VOLUP_ID           GPIO_NUM_18
#define BUTTON_VOLDOWN_ID         GPIO_NUM_5

#define INPUT_KEY_DEFAULT_INFO() {                          \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_REC,                   \
        .act_id  = BUTTON_REC_ID,                           \
    },                                                      \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_MODE,                  \
        .act_id  = BUTTON_MODE_ID,                          \
    },                                                      \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_SET,                   \
        .act_id  = BUTTON_SET_ID,                           \
    },                                                      \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_PLAY,                  \
        .act_id  = BUTTON_PLAY_ID,                          \
    },                                                      \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_VOLUP,                 \
        .act_id  = BUTTON_VOLUP_ID,                         \
    },                                                      \
    {                                                       \
        .type    = PERIPH_ID_BUTTON,                        \
        .user_id = INPUT_KEY_USER_ID_VOLDOWN,               \
        .act_id  = BUTTON_VOLDOWN_ID,                       \
    },                                                      \
}

/* ── Codec default configuration ─────────────────────────── */
extern audio_hal_func_t AUDIO_CODEC_ES8388_DEFAULT_HANDLE;

/* adc_input = LINE1  →  routes line-in jack to ADC          */
#define AUDIO_CODEC_DEFAULT_CONFIG() {                      \
    .adc_input  = AUDIO_HAL_ADC_INPUT_LINE1,                \
    .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,                 \
    .codec_mode = AUDIO_HAL_CODEC_MODE_BOTH,                \
    .i2s_iface  = {                                         \
        .mode    = AUDIO_HAL_MODE_SLAVE,                    \
        .fmt     = AUDIO_HAL_I2S_NORMAL,                    \
        .samples = AUDIO_HAL_44K_SAMPLES,                   \
        .bits    = AUDIO_HAL_BIT_LENGTH_16BITS,             \
    },                                                      \
}
