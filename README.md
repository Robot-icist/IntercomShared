# IntercomShared

Shared firmware library for the **AtomS3R Intercom** (base station) and
**StickS3 Intercom** (portable unit). Both projects compile this library from
the sibling `IntercomShared` directory via `lib_extra_dirs`.

## Modules

| File | Purpose |
| --- | --- |
| `IntercomConfig.h` | Board selection, audio/protocol constants, per-board flags. |
| `IntercomTypes.h` | Wire packet structs, `Mode`/`State` enums, UI state, helpers. |
| `AdpcmCodec.*` | IMA ADPCM 4-bit encoder/decoder with predictor/index state. |
| `SampleRing.*` | Thread-safe int16 ring buffer, PSRAM-first allocation, drop-oldest. |
| `AudioEngine.*` | M5Unified mic/speaker wrapper, half-duplex switching, play buffers. |
| `EspNowLink.*` | ESP-NOW transport, peer discovery, RSSI, receive queue. |
| `IntercomUi.*` | Canvas/sprite UI for 128x128 and 135x240 displays. |
| `IntercomApp.*` | Application state machine and the TX/RX/playback tasks. |

## Audio format

- 16 kHz, mono, signed 16-bit PCM.
- 20 ms frames: 320 samples -> 160 bytes of IMA ADPCM.
- One ESP-NOW packet per frame (10-byte header + 160-byte payload).

The packet header carries the ADPCM predictor and step index **at the start of
the frame**, so every packet decodes independently even if earlier packets are
lost.

## Protocol

| Type | Direction | Payload |
| --- | --- | --- |
| `Hello` | Broadcast every 1 s | Battery, mode, device name (16 bytes). |
| `Audio` | Unicast to known peer, broadcast fallback | ADPCM frame. |

- Fixed Wi-Fi channel `1`.
- A peer is considered online for 5 s after the last valid packet.
- Audio uses a PSRAM jitter buffer (~6 s) with a 100 ms prebuffer.
- Clip mode records up to 15 s to PSRAM and sends it at 1x real time.

## Thread model

| Task | Core | Priority | Responsibility |
| --- | --- | --- | --- |
| `txTask` | 0 | 4 | Mic capture, live encode/send, clip record, paced clip send. |
| `rxTask` | 0 | 3 | ESP-NOW receive, ADPCM decode, jitter-buffer fill. |
| `playTask` | 0 | 3 | Jitter-buffer read, speaker playback. |

`loop()` runs on the Arduino task and handles buttons, peer/battery polling and
the UI refresh (100 ms).

## Build integration

Each project sets:

```ini
lib_extra_dirs = ../IntercomShared
lib_deps = m5stack/M5Unified@0.2.28
```

and defines one board flag:

- `-DINTERCOM_BOARD_ATOMS3R`
- `-DINTERCOM_BOARD_STICKS3`

`IntercomConfig.h` uses those flags to select the audio port sharing policy and
the UI canvas memory type.
