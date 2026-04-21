#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
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

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "UDP_AUDIO";

#define AUDIO_SAMPLE_RATE  (48000)
#define AUDIO_BITS         (16)
#define AUDIO_CHANNELS     (1)
#define UDP_AUDIO_PORT     (8889)   /* porta su cui il telefono ascolta l'audio */
#define UDP_PACKET_SIZE    (1024)
#define HTTP_PORT          (8080)   /* serve solo la playlist XSPF */

static audio_element_handle_t raw_writer;
static audio_element_handle_t i2s_stream_reader;

/* indirizzo del telefono — impostato in app_main dal gateway DHCP */
static struct sockaddr_in s_target_addr;

/* ── Task UDP sender ──────────────────────────────────────── */

static void udp_sender_task(void *arg)
{
    int tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    char buf[UDP_PACKET_SIZE];

    /* ── statistiche ────────────────────────────────────── */
    uint32_t stat_sent = 0, stat_fail = 0, stat_short = 0;
    int64_t stat_gap_max = 0, stat_gap_min = INT64_MAX;
    int64_t last_send_us = 0, stat_window = esp_timer_get_time();

    ESP_LOGI(TAG, "Streaming → " IPSTR ":%d",
             IP2STR((ip4_addr_t *)&s_target_addr.sin_addr), UDP_AUDIO_PORT);

    while (1)
    {
        /* leggi audio dal pipeline */
        int len = raw_stream_read(raw_writer, buf, sizeof(buf));
        if (len <= 0)
            continue;
        if (len < UDP_PACKET_SIZE)
            stat_short++;

        int64_t now = esp_timer_get_time();
        if (last_send_us > 0)
        {
            int64_t gap = now - last_send_us;
            if (gap > stat_gap_max) stat_gap_max = gap;
            if (gap < stat_gap_min) stat_gap_min = gap;
        }
        last_send_us = now;

        int ret = sendto(tx_sock, buf, len, 0,
                         (struct sockaddr *)&s_target_addr, sizeof(s_target_addr));
        if (ret < 0) stat_fail++;
        else         stat_sent++;

        /* log ogni secondo */
        if ((now - stat_window) >= 1000000LL)
        {
            ESP_LOGI(TAG,
                     "1s: sent=%" PRIu32 " fail=%" PRIu32 " short=%" PRIu32
                     " | gap min=%" PRId64 "µs max=%" PRId64 "µs",
                     stat_sent, stat_fail, stat_short,
                     stat_gap_min == INT64_MAX ? 0 : stat_gap_min, stat_gap_max);
            stat_sent = stat_fail = stat_short = 0;
            stat_gap_max = 0;
            stat_gap_min = INT64_MAX;
            stat_window = now;
        }
    }

    close(tx_sock);
    vTaskDelete(NULL);
}

/* ── Playlist XSPF ───────────────────────────────────────────
 * VLC mobile apre http://ip:8080/ → scarica questo file →
 * si connette da solo a udp://@:8889 con i parametri corretti.
 * ────────────────────────────────────────────────────────── */

static esp_err_t playlist_handler(httpd_req_t *req)
{
    char body[640];
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<playlist version=\"1\""
        " xmlns=\"http://xspf.org/ns/0/\""
        " xmlns:vlc=\"http://www.videolan.org/vlc/playlist/ns/0/\">\n"
        "  <trackList><track>\n"
        "    <location>udp://@:%d</location>\n"
        "    <extension application=\"http://www.videolan.org/vlc/playlist/0\">\n"
        "      <vlc:option>demux=rawaud</vlc:option>\n"
        "      <vlc:option>rawaud-channels=%d</vlc:option>\n"
        "      <vlc:option>rawaud-samplerate=%d</vlc:option>\n"
        "      <vlc:option>rawaud-fourcc=s16l</vlc:option>\n"
        "    </extension>\n"
        "  </track></trackList>\n"
        "</playlist>\n",
        UDP_AUDIO_PORT, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);

    httpd_resp_set_type(req, "application/xspf+xml");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static void start_playlist_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = playlist_handler,
    };
    httpd_register_uri_handler(server, &uri);
}

/* ── app_main ─────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    /* WiFi */
    ESP_LOGI(TAG, "[ 1 ] Connecting to WiFi...");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .wifi_config.sta.ssid     = CONFIG_WIFI_SSID,
        .wifi_config.sta.password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Ricava IP proprio e gateway (= telefono con hotspot) */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    /* Destinazione: CLIENT_IP da menuconfig, oppure gateway DHCP */
    memset(&s_target_addr, 0, sizeof(s_target_addr));
    s_target_addr.sin_family = AF_INET;
    s_target_addr.sin_port   = htons(UDP_AUDIO_PORT);

    if (strlen(CONFIG_CLIENT_IP) > 0) {
        s_target_addr.sin_addr.s_addr = inet_addr(CONFIG_CLIENT_IP);
        ESP_LOGI(TAG, "Destinazione: %s (da menuconfig)", CONFIG_CLIENT_IP);
    } else {
        s_target_addr.sin_addr.s_addr = ip_info.gw.addr;
        ESP_LOGI(TAG, "Destinazione: " IPSTR " (gateway)", IP2STR(&ip_info.gw));
    }

    ESP_LOGI(TAG, "IP ESP32:  " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "VLC mobile:  apri http://" IPSTR ":%d/  (playlist automatica)",
             IP2STR(&ip_info.ip), HTTP_PORT);
    ESP_LOGI(TAG, "ffplay:      ffplay -f s16le -ar 48000 -ac 1 udp://0.0.0.0:%d", UDP_AUDIO_PORT);
    ESP_LOGI(TAG, "VLC desktop: vlc --demux=rawaud --rawaud-channels=1 --rawaud-samplerate=48000 --rawaud-fourcc=s16l udp://@:%d", UDP_AUDIO_PORT);

    /* Codec */
    ESP_LOGI(TAG, "[ 2 ] Init codec");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal,
                         AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    es8388_set_mic_gain(MIC_GAIN_0DB);
    es8388_config_adc_input(ADC_INPUT_LINPUT2_RINPUT2);
    es8388_write_reg(ES8388_ADCPOWER, 0x09);

    /* Pipeline */
    ESP_LOGI(TAG, "[ 3 ] Create pipeline  i2s → raw_stream");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_TYLE_AND_CH(
        CODEC_ADC_I2S_PORT, AUDIO_SAMPLE_RATE, AUDIO_BITS,
        AUDIO_STREAM_READER, AUDIO_CHANNELS);
    i2s_cfg.out_rb_size = 2 * 1024;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_writer = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, raw_writer, "raw");

    const char *link_tag[] = {"i2s", "raw"};
    audio_pipeline_link(pipeline, link_tag, 2);

    i2s_stream_set_clk(i2s_stream_reader, AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS);

    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "[ 4 ] Pipeline avviata");

    start_playlist_server();
    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Ready — streaming UDP verso il telefono");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
