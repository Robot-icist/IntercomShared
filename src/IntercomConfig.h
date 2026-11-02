#pragma once

#include <stddef.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Board selection
//
// Each PlatformIO project defines exactly one of these in platformio.ini.
// -----------------------------------------------------------------------------
#if !defined(INTERCOM_BOARD_ATOMS3R) && !defined(INTERCOM_BOARD_STICKS3)
#error "Define INTERCOM_BOARD_ATOMS3R or INTERCOM_BOARD_STICKS3"
#endif

#if defined(INTERCOM_BOARD_ATOMS3R) && defined(INTERCOM_BOARD_STICKS3)
#error "Select exactly one intercom board"
#endif

// Short name advertised in the ESP-NOW hello packet and shown in the UI.
#ifndef INTERCOM_DEVICE_NAME
#define INTERCOM_DEVICE_NAME "INTERCOM"
#endif

// Both units must use the same Wi-Fi channel. ESP-NOW does not hop channels.
#ifndef INTERCOM_CHANNEL
#define INTERCOM_CHANNEL 1
#endif

namespace intercom {

// 16 kHz mono is plenty for speech and keeps the ADPCM bitrate at 64 kbit/s.
constexpr uint32_t kSampleRate = 16000;
constexpr uint16_t kFrameSamples = 320;  // 20 ms
constexpr uint16_t kFrameBytes = 160;    // IMA ADPCM, 2 samples per byte
constexpr uint32_t kFrameMs = 20;

constexpr uint8_t kChannel = INTERCOM_CHANNEL;
constexpr uint8_t kDefaultVolume = 65;

constexpr uint32_t kHelloIntervalMs = 1000;
constexpr uint32_t kPeerTimeoutMs = 5000;

// 6 s of jitter buffer absorbs bursts and the half-duplex turnaround.
constexpr size_t kJitterCapacity = kSampleRate * 6;
// 15 s of speech is a comfortable voice clip and fits easily in PSRAM.
constexpr size_t kClipCapacity = kSampleRate * 15;
constexpr uint32_t kClipMaxMs = (uint32_t)((uint64_t)kClipCapacity * 1000 / kSampleRate);
// Start playback once this much audio is queued (100 ms).
constexpr size_t kPrebufferSamples = kSampleRate / 10;
// Insert silence for short packet gaps; larger gaps are treated as loss.
constexpr size_t kMaxGapFrames = 5;

constexpr size_t kMaxPacketSize = 250;
constexpr uint8_t kMagic = 0x49;  // 'I'
constexpr uint8_t kVersion = 1;

// The AtomS3R voice base shares one I2S peripheral between mic and speaker, so
// capture and playback must not run at the same time. The StickS3 uses two
// separate I2S peripherals and can keep the speaker alive while capturing.
#if defined(INTERCOM_BOARD_ATOMS3R)
constexpr bool kSharedAudioPort = true;
constexpr bool kUiUsePsram = false;
#else
constexpr bool kSharedAudioPort = false;
constexpr bool kUiUsePsram = true;
#endif

}  // namespace intercom
