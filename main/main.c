#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include "board.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "es8388.h"
#include "audio_idf_version.h"

#include "rtp_sender.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "APP";

/* ── Wi-Fi ────────────────────────────────────────────────────────── */

static void wifi_init(void)
{
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .wifi_config.sta.ssid     = CONFIG_WIFI_SSID,
        .wifi_config.sta.password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    esp_wifi_set_ps(WIFI_PS_NONE); /* disable power save — prevents TX gaps */
}

/* ── Codec ────────────────────────────────────────────────────────── */

static void codec_init(void)
{
    audio_board_handle_t board = audio_board_init();
    audio_hal_ctrl_codec(board->audio_hal,
                         AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    /* Line-in on LINPUT2/RINPUT2 (AudioKit jack).
     * PGA gain = 0 dB: onboard mic is wired in parallel on the same input;
     * keeping gain at 0 makes it nearly inaudible. */
    es8388_set_mic_gain(MIC_GAIN_0DB);
    es8388_config_adc_input(ADC_INPUT_LINPUT2_RINPUT2);
    es8388_write_reg(ES8388_ADCPOWER, 0x09); /* MICBIAS off, ADC on */
}

/* ── Audio pipeline ───────────────────────────────────────────────── */

static audio_element_handle_t pipeline_init(void)
{
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline  = audio_pipeline_init(&pipeline_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_TYLE_AND_CH(
        CODEC_ADC_I2S_PORT,
        CONFIG_AUDIO_SAMPLE_RATE,
        16,                     /* bits */
        AUDIO_STREAM_READER,
        CONFIG_AUDIO_CHANNELS);
    i2s_cfg.out_rb_size = 2 * 1024; /* small ring buffer = low latency */
    audio_element_handle_t i2s_reader = i2s_stream_init(&i2s_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t raw_writer = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, i2s_reader, "i2s");
    audio_pipeline_register(pipeline, raw_writer, "raw");

    const char *link[] = {"i2s", "raw"};
    audio_pipeline_link(pipeline, link, 2);

    i2s_stream_set_clk(i2s_reader,
                       CONFIG_AUDIO_SAMPLE_RATE,
                       16,
                       CONFIG_AUDIO_CHANNELS);

    audio_pipeline_run(pipeline);
    return raw_writer;
}

/* ── Entry point ──────────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*",  ESP_LOG_WARN);
    esp_log_level_set(TAG,  ESP_LOG_INFO);
    esp_log_level_set("RTP", ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    wifi_init();

    /* Resolve destination: menuconfig IP or DHCP gateway (hotspot phone) */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port   = htons(CONFIG_UDP_AUDIO_PORT),
    };
    if (strlen(CONFIG_CLIENT_IP) > 0) {
        target.sin_addr.s_addr = inet_addr(CONFIG_CLIENT_IP);
        ESP_LOGI(TAG, "Target: %s (menuconfig)", CONFIG_CLIENT_IP);
    } else {
        target.sin_addr.s_addr = ip_info.gw.addr;
        ESP_LOGI(TAG, "Target: " IPSTR " (DHCP gateway)", IP2STR(&ip_info.gw));
    }
    ESP_LOGI(TAG, "ESP32 IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Open in VLC: rtp://@:%d", CONFIG_UDP_AUDIO_PORT);

    codec_init();
    audio_element_handle_t raw_writer = pipeline_init();
    rtp_sender_start(&target, raw_writer);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
