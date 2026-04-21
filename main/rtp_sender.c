#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "raw_stream.h"
#include "rtp_sender.h"
#include "sdkconfig.h"

static const char *TAG = "RTP";

#define RTP_HEADER_SIZE  12
#define RTP_PT_L16_MONO  11   /* RFC 3551 §6: L16/44100/1 */

/* ── RTP header (RFC 3550) ────────────────────────────────────────────
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                           timestamp                           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           synchronization source (SSRC) identifier           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ─────────────────────────────────────────────────────────────── */

static inline void rtp_header_fill(uint8_t *h, uint16_t seq, uint32_t ts)
{
    h[0]  = 0x80;             /* V=2, P=0, X=0, CC=0           */
    h[1]  = RTP_PT_L16_MONO; /* M=0, PT=11                     */
    h[2]  = seq >> 8;         /* sequence number (big-endian)   */
    h[3]  = seq & 0xFF;
    h[4]  = ts >> 24;         /* timestamp in samples (BE)      */
    h[5]  = (ts >> 16) & 0xFF;
    h[6]  = (ts >>  8) & 0xFF;
    h[7]  = ts & 0xFF;
    h[8]  = 0x12;             /* SSRC — fixed arbitrary value   */
    h[9]  = 0x34;
    h[10] = 0x56;
    h[11] = 0x78;
}

/* ── Task context ─────────────────────────────────────────────────── */

typedef struct {
    struct sockaddr_in    target;
    audio_element_handle_t raw_writer;
} sender_ctx_t;

static void sender_task(void *arg)
{
    sender_ctx_t *ctx = (sender_ctx_t *)arg;

    int tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    /* Full packet: RTP header + PCM payload */
    uint8_t pkt[RTP_HEADER_SIZE + CONFIG_UDP_PACKET_SIZE];
    char   *pcm = (char *)(pkt + RTP_HEADER_SIZE);

    uint16_t rtp_seq = 0;
    uint32_t rtp_ts  = 0;

    /* Statistics */
    uint32_t stat_sent = 0, stat_fail = 0, stat_short = 0;
    int64_t  stat_gap_max = 0, stat_gap_min = INT64_MAX;
    int64_t  last_send_us = 0, stat_window = esp_timer_get_time();

    ESP_LOGI(TAG, "Sender task started, port %d", CONFIG_UDP_AUDIO_PORT);

    while (1) {
        int len = raw_stream_read(ctx->raw_writer, pcm, CONFIG_UDP_PACKET_SIZE);
        if (len <= 0)
            continue;
        if (len < CONFIG_UDP_PACKET_SIZE)
            stat_short++;

        /* L16 (RTP) is big-endian; ES8388 outputs little-endian → swap */
        for (int i = 0; i < len; i += 2) {
            uint8_t tmp = pcm[i];
            pcm[i]      = pcm[i + 1];
            pcm[i + 1]  = tmp;
        }

        rtp_header_fill(pkt, rtp_seq, rtp_ts);
        rtp_seq++;
        rtp_ts += len / 2; /* bytes → samples (2 bytes/sample, mono) */

        int64_t now = esp_timer_get_time();
        if (last_send_us > 0) {
            int64_t gap = now - last_send_us;
            if (gap > stat_gap_max) stat_gap_max = gap;
            if (gap < stat_gap_min) stat_gap_min = gap;
        }
        last_send_us = now;

        int ret = sendto(tx_sock, pkt, RTP_HEADER_SIZE + len, 0,
                         (struct sockaddr *)&ctx->target, sizeof(ctx->target));
        if (ret < 0) stat_fail++;
        else         stat_sent++;

        /* Log once per second */
        if ((now - stat_window) >= 1000000LL) {
            ESP_LOGI(TAG,
                     "1s — sent=%" PRIu32 " fail=%" PRIu32 " short=%" PRIu32
                     " | gap min=%" PRId64 "µs max=%" PRId64 "µs",
                     stat_sent, stat_fail, stat_short,
                     stat_gap_min == INT64_MAX ? 0 : stat_gap_min, stat_gap_max);
            stat_sent = stat_fail = stat_short = 0;
            stat_gap_max = 0;
            stat_gap_min = INT64_MAX;
            stat_window  = now;
        }
    }

    close(tx_sock);
    free(ctx);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────── */

void rtp_sender_start(const struct sockaddr_in *target,
                      audio_element_handle_t    raw_writer)
{
    sender_ctx_t *ctx = malloc(sizeof(sender_ctx_t));
    ctx->target     = *target;
    ctx->raw_writer = raw_writer;
    xTaskCreate(sender_task, "rtp_sender", 4096, ctx, 5, NULL);
}
