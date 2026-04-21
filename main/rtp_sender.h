#pragma once

#include "lwip/sockets.h"
#include "audio_element.h"

/**
 * @brief Start the RTP/UDP sender task.
 *
 * Reads raw PCM from @p raw_writer, wraps each buffer in an RTP header
 * (payload type 11 — L16 mono 44100 Hz, RFC 3551) and sends it via UDP
 * to @p target.  The task runs indefinitely at priority 5.
 *
 * @param target     Destination address (IP + port), filled by the caller.
 * @param raw_writer Raw-stream element handle to read PCM from.
 */
void rtp_sender_start(const struct sockaddr_in *target,
                      audio_element_handle_t    raw_writer);
