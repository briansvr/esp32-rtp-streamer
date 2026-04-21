# ESP32 RTP Audio Streamer

Streams line-in audio from an ESP32 AudioKit (ES8388 codec) over Wi-Fi using
RTP/UDP. Any RTP-capable player (VLC, ffplay) can receive the stream with no
format parameters — the codec is self-describing via RTP payload type 11
(L16 mono 44100 Hz, RFC 3551).

## Features

- Low-latency UDP streaming (no TCP buffering)
- RTP encapsulation — VLC auto-detects format, no parameters needed
- Configurable via `menuconfig` (SSID, port, sample rate, etc.)
- Targets the DHCP gateway automatically (ideal for mobile hotspot use)
- Optional fixed destination IP via menuconfig

## Hardware

Tested on **AI Thinker ESP32 AudioKit v2.2** with ES8388 codec.  
Board selected in ADF: **LyraT v4.3** (closest compatible profile).

### AudioKit v2.2 pin workaround

The AudioKit v2.2 uses different I2C and I2S pins than the stock LyraT v4.3
profile. Edit the following file in ESP-ADF **before building**:

```
$ADF_PATH/components/audio_board/lyrat_v4_3/board_pins_config.c
```

Apply these changes:

| Function | LyraT default | AudioKit v2.2 |
|----------|---------------|---------------|
| I2C SDA  | GPIO 18       | **GPIO 33**   |
| I2C SCL  | GPIO 23       | **GPIO 32**   |
| I2S BCK  | GPIO 5        | **GPIO 27**   |

```c
/* get_i2c_pins() */
i2c_config->sda_io_num = GPIO_NUM_33;   // was 18
i2c_config->scl_io_num = GPIO_NUM_32;   // was 23

/* get_i2s_pins() */
i2s_config->bck_io_num = GPIO_NUM_27;   // was 5
```

> **Keep a backup** of the original file before editing — this change affects
> all projects that use the LyraT v4.3 board profile.

### Line-in wiring

The AudioKit v2.2 exposes two line-in jacks wired to **LINPUT2 / RINPUT2** on
the ES8388. The onboard MEMS microphone shares the same analog path; setting
PGA gain to 0 dB makes it nearly inaudible in practice.

## Requirements

- [ESP-IDF](https://github.com/espressif/esp-idf) (tested with v4.4+)
- [ESP-ADF](https://github.com/espressif/esp-adf)
- Environment variables `IDF_PATH` and `ADF_PATH` set correctly

## Build & Flash

```bash
idf.py menuconfig   # set Wi-Fi credentials and optional parameters
idf.py build
idf.py flash monitor
```

## Configuration (`menuconfig`)

| Menu | Key | Default | Description |
|------|-----|---------|-------------|
| Network Configuration | `WIFI_SSID` | `myssid` | Wi-Fi network name |
| Network Configuration | `WIFI_PASSWORD` | `myssid` | Wi-Fi password |
| Network Configuration | `CLIENT_IP` | *(empty)* | Fixed destination IP. If empty, the DHCP gateway is used (= phone on hotspot) |
| Audio Settings | `AUDIO_SAMPLE_RATE` | `44100` | Sample rate in Hz. Must be 44100 for RTP PT11 auto-detection |
| Audio Settings | `AUDIO_CHANNELS` | `1` | 1 = mono, 2 = stereo |
| RTP / Network Settings | `UDP_AUDIO_PORT` | `8889` | Destination UDP port |
| RTP / Network Settings | `UDP_PACKET_SIZE` | `1024` | PCM bytes per UDP packet (~11.6 ms at 44100 Hz mono) |

## Receiving the stream

### VLC (mobile or desktop) — recommended

Open a network stream and enter:

```
rtp://@:8889
```

VLC auto-detects the format from the RTP header. No additional parameters needed.

### ffplay

```bash
ffplay -f s16be -ar 44100 -ac 1 udp://0.0.0.0:8889
```

> Note: RTP L16 is big-endian (`s16be`), not little-endian.

## How it works

```
ES8388 line-in
     │
     ▼
I2S stream (ESP-ADF)
     │
     ▼
raw_stream (ring buffer, 2 KB)
     │
     ▼
RTP sender task
  ├─ byteswap PCM (little-endian → big-endian, required by L16/RFC 3551)
  ├─ prepend 12-byte RTP header (PT=11, seq++, timestamp += samples)
  └─ sendto() → UDP → receiver
```

The 2 KB ring buffer and the blocking `raw_stream_read()` act as a natural
rate limiter: the sender can never outpace the I2S capture clock.

## Project structure

```
main/
├── main.c            — Wi-Fi init, codec init, pipeline init, app_main
├── rtp_sender.c      — RTP header, byteswap, UDP sender task
├── rtp_sender.h      — public API (rtp_sender_start)
├── CMakeLists.txt
└── Kconfig.projbuild — all configurable parameters
```

## Known limitations

- Only one receiver at a time (unicast UDP).
- Changing `AUDIO_SAMPLE_RATE` away from 44100 Hz breaks RTP PT11
  auto-detection; the receiver will need manual format parameters.
- The onboard microphone cannot be fully disabled in software; PGA gain 0 dB
  reduces its contribution to an acceptable level.
