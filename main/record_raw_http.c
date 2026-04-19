#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
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
#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "HTTP_AUDIO_SERVER";

#define AUDIO_SAMPLE_RATE   (44100)
#define AUDIO_BITS          (16)
#define AUDIO_CHANNELS      (2)
#define HTTP_PORT           (8080)
#define STREAM_BUF_SIZE     (4096)

static audio_pipeline_handle_t pipeline;
static audio_element_handle_t  i2s_stream_reader;
static audio_element_handle_t  raw_writer;

static SemaphoreHandle_t stream_mutex;   /* garantisce un solo client alla volta */

/* ── WAV header per streaming (size = 0x7FFFFFFF) ────────── */

static void send_wav_header(httpd_req_t *req)
{
    const uint32_t sr   = AUDIO_SAMPLE_RATE;
    const uint16_t ch   = AUDIO_CHANNELS;
    const uint16_t bps  = AUDIO_BITS;
    const uint32_t br   = sr * ch * (bps / 8);
    const uint16_t ba   = ch * (bps / 8);
    const uint32_t INF  = 0x7FFFFFFF;

    uint8_t hdr[44] = {
        'R','I','F','F',
        INF & 0xFF, (INF>>8)&0xFF, (INF>>16)&0xFF, (INF>>24)&0xFF,
        'W','A','V','E',
        'f','m','t',' ',
        16,0,0,0,
        1,0,
        ch  & 0xFF, (ch >>8)&0xFF,
        sr  & 0xFF, (sr >>8)&0xFF, (sr >>16)&0xFF, (sr >>24)&0xFF,
        br  & 0xFF, (br >>8)&0xFF, (br >>16)&0xFF, (br >>24)&0xFF,
        ba  & 0xFF, (ba >>8)&0xFF,
        bps & 0xFF, (bps>>8)&0xFF,
        'd','a','t','a',
        INF & 0xFF, (INF>>8)&0xFF, (INF>>16)&0xFF, (INF>>24)&0xFF,
    };
    httpd_resp_send_chunk(req, (char *)hdr, sizeof(hdr));
}

/* ── HTTP handler ─────────────────────────────────────────── */

static esp_err_t audio_stream_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(stream_mutex, 0) == pdFALSE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Already streaming to another client");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client connected — starting stream");

    httpd_resp_set_type(req, "audio/wav");

    audio_pipeline_run(pipeline);
    send_wav_header(req);

    char *buf = malloc(STREAM_BUF_SIZE);
    if (!buf) {
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_terminate(pipeline);
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_NO_MEM;
    }

    bool client_ok = true;
    while (1) {
        int len = raw_stream_read(raw_writer, buf, STREAM_BUF_SIZE);
        if (len <= 0) break;   /* pipeline fermata o errore */

        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
            ESP_LOGI(TAG, "Client disconnected");
            client_ok = false;
            break;
        }
    }

    free(buf);
    if (client_ok) {
        httpd_resp_send_chunk(req, NULL, 0);   /* chiude chunked transfer */
    }

    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_reset_ringbuffer(pipeline);
    audio_pipeline_reset_elements(pipeline);
    audio_pipeline_terminate(pipeline);

    ESP_LOGI(TAG, "Stream terminated");
    xSemaphoreGive(stream_mutex);
    return ESP_OK;
}

/* ── HTTP server ──────────────────────────────────────────── */

static void start_http_server(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = HTTP_PORT;
    config.stack_size = 8192;   /* raw_stream_read + HTTP send */

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t audio_uri = {
        .uri     = "/audio",
        .method  = HTTP_GET,
        .handler = audio_stream_handler,
    };
    httpd_register_uri_handler(server, &audio_uri);

    ESP_LOGI(TAG, "HTTP server on port %d  →  GET /audio", HTTP_PORT);
}

/* ── app_main ─────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);  /* nasconde warning di disconnect normali */

    stream_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(stream_mutex);

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
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

    /* Stampa IP */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Comando: ffplay -f s16le -ar 44100 -ac 2 http://" IPSTR ":%d/audio",
             IP2STR(&ip_info.ip), HTTP_PORT);

    /* Codec */
    ESP_LOGI(TAG, "[ 2 ] Init codec");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal,
                         AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    /* Pipeline */
    ESP_LOGI(TAG, "[ 3 ] Create pipeline  i2s → raw_stream");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_TYLE_AND_CH(
        CODEC_ADC_I2S_PORT, AUDIO_SAMPLE_RATE, AUDIO_BITS,
        AUDIO_STREAM_READER, AUDIO_CHANNELS);
    i2s_cfg.out_rb_size = 16 * 1024;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_writer = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, raw_writer,         "raw");

    const char *link_tag[] = {"i2s", "raw"};
    audio_pipeline_link(pipeline, link_tag, 2);

    i2s_stream_set_clk(i2s_stream_reader, AUDIO_SAMPLE_RATE, AUDIO_BITS, AUDIO_CHANNELS);

    /* HTTP server */
    ESP_LOGI(TAG, "[ 4 ] Start HTTP server");
    start_http_server();

    ESP_LOGI(TAG, "Ready — in attesa di client");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
