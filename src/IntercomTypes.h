#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>

namespace intercom {

enum class Mode : uint8_t {
  Live = 0,    // push-to-talk while the button is held
  Clip = 1,    // click to record, click again to send the clip
  Spy = 2,     // remote mic listen / remote clip record
  Peer = 3,    // choose the destination peer(s)
  Volume = 4,  // local speaker volume
};

enum class State : uint8_t {
  Idle,
  Tx,
  Rec,
  Send,
  Rx,
  Spy,
  Error,
};

enum PacketType : uint8_t {
  kPacketHello = 1,
  kPacketAudio = 2,
  kPacketSpy = 3,
};

// Hello header flags. The WiFi transport reserves 0x80 for discovery.
// Receive-only units (for example the ATOM Lite speaker) set bit 0 so spy
// mode never tries to record from them.
constexpr uint8_t kHelloFlagReceiveOnly = 0x01;
constexpr uint8_t kHelloFlagWebPeer = 0x02;
constexpr uint8_t kHelloFlagHasDeviceMac = 0x04;
constexpr uint8_t kAudioFlagWebPeer = 0x01;
constexpr uint8_t kAudioFlagWebPeerTarget = 0x02;
constexpr uint8_t kSpyFlagWebPeerTarget = 0x01;
constexpr uint8_t kSpyTargetWebPeer = 0x02;
constexpr uint16_t kWebClientPort = 1000;
constexpr size_t kMaxWebClients = 3;

constexpr bool isWebClientPort(uint16_t port) {
  return port >= kWebClientPort &&
         port < kWebClientPort + kMaxWebClients;
}

inline void makeWebPeerIdentity(const char* name, uint8_t identity[6]) {
  uint32_t hash = 2166136261u;
  if (name) {
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(name);
         *p; ++p) {
      hash = (hash ^ *p) * 16777619u;
    }
  }
  identity[0] = 0x02;
  identity[1] = 0x57;
  identity[2] = static_cast<uint8_t>(hash >> 24);
  identity[3] = static_cast<uint8_t>(hash >> 16);
  identity[4] = static_cast<uint8_t>(hash >> 8);
  identity[5] = static_cast<uint8_t>(hash);
}

enum SpyCommand : uint8_t {
  kSpyLiveStart = 1,
  kSpyLiveStop = 2,
  kSpyClipStart = 3,
  kSpyClipStop = 4,
  kSpyClipDone = 5,
  kSpyBusy = 6,
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

struct HelloDeviceIdentity {
  uint8_t mac[6];
};

struct WebPeerAudioTarget {
  uint8_t identity[6];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct SpyPayload {
  uint8_t command;
};

struct SpyTargetPayload {
  uint8_t command;
  uint8_t addr[6];
  uint16_t port;
  uint8_t flags;
  uint8_t identity[6];
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 10, "PacketHeader must stay 10 bytes");
static_assert(sizeof(HelloPayload) == 18, "HelloPayload must stay 18 bytes");
static_assert(sizeof(HelloDeviceIdentity) == 6,
              "HelloDeviceIdentity must stay 6 bytes");
static_assert(sizeof(WebPeerAudioTarget) == 6,
              "WebPeerAudioTarget must stay 6 bytes");
static_assert(sizeof(SpyPayload) == 1, "SpyPayload must stay 1 byte");
static_assert(sizeof(SpyTargetPayload) == 16,
              "SpyTargetPayload must stay 16 bytes");

struct RxPacket {
  uint8_t src[6];
  int8_t rssi;
  uint16_t port;  // 0 for ESP-NOW, UDP port for WiFi
  uint16_t len;
  alignas(4) uint8_t data[250];
};

struct UiState {
  State state = State::Idle;
  Mode mode = Mode::Live;
  bool peerOnline = false;
  char peerName[16] = {0};
  char peerAddress[16] = {0};
  int8_t peerRssi = 0;
  int battery = -1;
  float micLevel = 0.0f;
  float rxLevel = 0.0f;
  uint32_t recordingMs = 0;
  uint32_t sendProgress = 0;  // 0..100
  uint8_t volume = 50;         // local speaker volume, 0..100
  uint32_t sampleRate = 16000;
  uint8_t channel = 1;
  const char* transportType = "ESP-NOW";  // Current transport: "ESP-NOW", "WiFi", "none"
  char selfName[16] = {0};
  uint8_t peerCount = 0;
  int8_t peerIndex = -1;  // -1 = ALL
  bool targetAll = true;
  bool peerSelected = false;
  uint8_t selectedCount = 0;
  char selfAddress[16] = {0};
  char selfMdns[32] = {0};
  char selfOtaPassword[64] = {0};
  bool spy = false;       // spy activity on either side
  bool spyLive = false;   // live spy stream active
  bool spyClip = false;   // remote clip recording in progress
  bool spyAwait = false;  // waiting for the remote clip to come back
};

inline const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Clip:
      return "CLIP";
    case Mode::Spy:
      return "SPY";
    case Mode::Peer:
      return "PEER";
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
    case State::Spy:
      return "SPY";
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
