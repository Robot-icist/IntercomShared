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
| `Transport.*` | ESP-NOW and WiFi transports, peer discovery, transport selection. |
| `EspNowLink.*` | Legacy ESP-NOW helper, not used by `IntercomApp`. |
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
| `Hello` | Broadcast every 1 s | Battery, mode, unique device name (16 bytes). |
| `Audio` | Selected peer(s) or broadcast | ADPCM frame. |
| `Spy` | Unicast to one peer | Live/clip start, stop, done, busy. |

- Fixed Wi-Fi channel `1`.
- A peer is considered online for 5 s after the last valid packet.
- Audio uses a PSRAM jitter buffer (~6 s) with a 100 ms prebuffer.
- Clip mode records up to 15 s to PSRAM and sends it at 1x real time.
- Device names get a short MAC suffix, so identical boards stay distinguishable.
- Receive-only peers (no microphone, e.g. the ATOM Lite speaker) set the
  `ReceiveOnly` Hello flag; spy mode skips them.

## Modes

| Mode | Controls |
| --- | --- |
| `LIVE` | Hold A to talk to the selected peer(s). |
| `CLIP` | Click A to record, click again to send. |
| `SPY` | Hold A to listen through the selected peer's mic, click A to record a clip on it. Spy requires exactly one peer. |
| `PEER` | Click A to toggle the highlighted peer, hold A to move through `ALL` and the peer list. |
| `VOL` | Click A up, hold A down. |

Double-click A cycles modes. `ALL` broadcasts, so every peer hears Live and Clip
audio, including receive-only speakers. Individual selections can include several
peers. A spy clip is recorded on the remote unit and played back on the controller.

## Thread model

| Task | Core | Priority | Responsibility |
| --- | --- | --- | --- |
| `txTask` | 0 | 4 | Mic capture, live encode/send, clip record, paced clip send. |
| `rxTask` | 0 | 3 | Receive, ADPCM decode, jitter-buffer fill. |
| `playTask` | 0 | 3 | Jitter-buffer read, speaker playback. |

`loop()` runs on the Arduino task and handles buttons, peer/battery polling and
the UI refresh (100 ms).

## Build integration

Each firmware project includes this repository as a Git submodule at
`lib/IntercomShared`. Clone either project with its submodules:

```sh
git clone --recurse-submodules https://github.com/Robot-icist/StickS3-Intercom.git
git clone --recurse-submodules https://github.com/Robot-icist/AtomS3R-Intercom.git
```

If a project was cloned without submodules, initialize them with:

```sh
git submodule update --init --recursive
```

PlatformIO uses the shared library from `lib/IntercomShared` and its M5Unified
patch script. Each project also sets:

```ini
lib_extra_dirs = lib
lib_deps = m5stack/M5Unified@0.2.28
```

and defines one board flag:

- `-DINTERCOM_BOARD_ATOMS3R`
- `-DINTERCOM_BOARD_STICKS3`

`IntercomConfig.h` uses those flags to select the audio port sharing policy and
the UI canvas memory type.
