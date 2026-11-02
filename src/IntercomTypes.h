#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>

namespace intercom {

enum class Mode : uint8_t {
  Live = 0,    // push-to-talk while the button is held
  Clip = 1,    // click to record, click again to send the clip
  Volume = 2,  // local speaker volume
};

enum class State : uint8_t {
  Idle,
  Tx,
  Rec,
  Send,
  Rx,
  Error,
};

enum PacketType : uint8_t {
  kPacketHello = 1,
  kPacketAudio = 2,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t flags;
  uint16_t seq;
  int16_t predictor;   // ADPCM state after the previous frame
  uint8_t adpcmIndex;  // ADPCM step index after the previous frame
  uint8_t payloadLen;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct HelloPayload {
  uint8_t battery;  // 0..100, 0xFF when unknown
  uint8_t mode;
  char name[16];
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 10, "PacketHeader must stay 10 bytes");
static_assert(sizeof(HelloPayload) == 18, "HelloPayload must stay 18 bytes");

struct RxPacket {
  uint8_t src[6];
  int8_t rssi;
  uint16_t len;
  alignas(4) uint8_t data[250];
};

struct UiState {
  State state = State::Idle;
  Mode mode = Mode::Live;
  bool peerOnline = false;
  char peerName[16] = {0};
  int8_t peerRssi = 0;
  int battery = -1;
  float micLevel = 0.0f;
  float rxLevel = 0.0f;
  uint32_t recordingMs = 0;
  uint32_t sendProgress = 0;  // 0..100
  uint8_t volume = 50;         // local speaker volume, 0..100
  uint32_t sampleRate = 16000;
  uint8_t channel = 1;
};

inline const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Clip:
      return "CLIP";
    case Mode::Volume:
      return "VOL";
    default:
      return "LIVE";
  }
}

inline const char* stateName(State state) {
  switch (state) {
    case State::Tx:
      return "TALK";
    case State::Rec:
      return "REC";
    case State::Send:
      return "SEND";
    case State::Rx:
      return "RX";
    case State::Error:
      return "ERROR";
    default:
      return "READY";
  }
}

// Normalised 0..1 level for the small VU bars. The 4x gain makes quiet speech
// visible without clipping normal speech.
inline float pcmLevel(const int16_t* samples, size_t count) {
  if (!samples || count == 0) {
    return 0.0f;
  }
  uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    int32_t v = samples[i];
    sum += (uint32_t)(v * v);
  }
  float rms = sqrtf((float)sum / (float)count) / 32768.0f;
  return std::min(1.0f, rms * 4.0f);
}

}  // namespace intercom
