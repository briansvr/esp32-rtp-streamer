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

#define AUDIO_SAMPLE_RATE (48000)
#define AUDIO_BITS (16)
#define AUDIO_CHANNELS (2)
#define UDP_REG_PORT   (8888)   /* porta su cui ESP32 ascolta le registrazioni */
#define UDP_AUDIO_PORT (8889)   /* porta su cui il client ascolta l'audio       */
#define UDP_PACKET_SIZE (1024)  /* ~5ms di audio per pacchetto */

static audio_element_handle_t raw_writer;
static audio_element_handle_t i2s_stream_reader;

/* ── Task UDP sender ──────────────────────────────────────── */

/*
 * Il client si registra inviando qualsiasi pacchetto UDP alla porta UDP_PORT.
 * L'ESP32 risponde con audio unicast verso quell'IP:porta_client.
 * Se il client non manda keepalive entro CLIENT_TIMEOUT_S, viene rimosso.
 */
#define CLIENT_TIMEOUT_S 5

static void udp_sender_task(void *arg)
{
    /* socket di ascolto (riceve registrazioni / keepalive) */
    int rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in rx_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_REG_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(rx_sock, (struct sockaddr *)&rx_addr, sizeof(rx_addr));

    /* timeout su recvfrom così non blocca il loop */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 1000};
    setsockopt(rx_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* socket di invio (trasmette audio unicast) */
    int tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    struct sockaddr_in client = {0};
    int64_t client_last_seen = 0;
    bool has_client = false;

    char buf[UDP_PACKET_SIZE];
    char rx_buf[16];

    /* ── statistiche ────────────────────────────────────── */
    uint32_t stat_sent = 0, stat_fail = 0, stat_short = 0;
    int64_t stat_gap_max = 0, stat_gap_min = INT64_MAX;
    int64_t last_send_us = 0, stat_window = esp_timer_get_time();

    ESP_LOGI(TAG, "In ascolto su UDP :%d — invia un pacchetto per registrarti", UDP_REG_PORT);

    while (1)
    {
        /* controlla nuovi client / keepalive */
        struct sockaddr_in sender = {0};
        socklen_t sender_len = sizeof(sender);
        int rx = recvfrom(rx_sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&sender, &sender_len);
        if (rx > 0)
        {
            int64_t now_s = esp_timer_get_time() / 1000000LL;
            if (!has_client || sender.sin_addr.s_addr != client.sin_addr.s_addr)
            {
                ESP_LOGI(TAG, "Client registrato: " IPSTR ":%d",
                         IP2STR((ip4_addr_t *)&sender.sin_addr),
                         ntohs(sender.sin_port));
            }
            client = sender;
            client.sin_port = htons(UDP_AUDIO_PORT);  /* audio sempre su porta fissa */
            client_last_seen = now_s;
            has_client = true;
        }

        /* timeout client */
        if (has_client)
        {
            int64_t now_s = esp_timer_get_time() / 1000000LL;
            if ((now_s - client_last_seen) >= CLIENT_TIMEOUT_S)
            {
                ESP_LOGI(TAG, "Client timeout — in attesa di nuova registrazione");
                has_client = false;
            }
        }

        /* leggi audio dal pipeline */
        int len = raw_stream_read(raw_writer, buf, sizeof(buf));
        if (len <= 0)
            continue;
        if (len < UDP_PACKET_SIZE)
            stat_short++;

        if (!has_client)
            continue; /* nessun client: scarta e tieni il buffer svuotato */

        int64_t now = esp_timer_get_time();
        if (last_send_us > 0)
        {
            int64_t gap = now - last_send_us;
            if (gap > stat_gap_max)
                stat_gap_max = gap;
            if (gap < stat_gap_min)
                stat_gap_min = gap;
        }
        last_send_us = now;

        int ret = sendto(tx_sock, buf, len, 0,
                         (struct sockaddr *)&client, sizeof(client));
        if (ret < 0)
            stat_fail++;
        else
            stat_sent++;

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

    close(rx_sock);
    close(tx_sock);
    vTaskDelete(NULL);
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
        .wifi_config.sta.ssid = CONFIG_WIFI_SSID,
        .wifi_config.sta.password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    esp_wifi_set_ps(WIFI_PS_NONE); /* disabilita power save → niente gap nel TX */

    /* IP e broadcast */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    // uint32_t broadcast_ip = ip_info.ip.addr | ~ip_info.netmask.addr;

    ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "1) Registrati (keepalive ogni ~3s):  echo r | nc -u " IPSTR " %d", IP2STR(&ip_info.ip), UDP_REG_PORT);
    ESP_LOGI(TAG, "2) Ascolta ffplay:  ffplay -f s16le -ar 48000 -ac 2 udp://0.0.0.0:%d", UDP_AUDIO_PORT);
    ESP_LOGI(TAG, "   Ascolta VLC:     vlc --demux=rawaud --rawaud-channels=2 --rawaud-samplerate=48000 --rawaud-fourcc=s16l udp://@:%d", UDP_AUDIO_PORT);

    /* Codec */
    ESP_LOGI(TAG, "[ 2 ] Init codec");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal,
                         AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    es8388_set_mic_gain(MIC_GAIN_24DB);
    es8388_config_adc_input(ADC_INPUT_LINPUT2_RINPUT2);  /* AudioKit: line-in sui jack LIN2/RIN2 */

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

    /* Avvia pipeline subito — sempre in esecuzione */
    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "[ 4 ] Pipeline avviata");

    xTaskCreate(udp_sender_task, "udp_sender", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Ready — streaming UDP in corso");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
