#pragma once

#include <Arduino.h>

#include <atomic>

#include "AdpcmCodec.h"
#include "AudioEngine.h"
#include "IntercomConfig.h"
#include "IntercomTypes.h"
#include "IntercomUi.h"
#include "SampleRing.h"
#include "Transport.h"

namespace intercom {

class IntercomApp {
 public:
  bool begin();
  void loop();

 private:
  struct Peer {
    bool used = false;
    bool online = false;
    bool selected = false;
    bool viaWifi = false;
    bool receiveOnly = false;
    bool webPeer = false;
    bool hasIdentityMac = false;
    bool hasEspNowRoute = false;
    bool hasWifiRoute = false;
    uint8_t addr[6] = {0};
    uint8_t identityMac[6] = {0};
    uint8_t wifiAddr[6] = {0};
    uint16_t port = 0;
    uint16_t wifiPort = 0;
    char name[16] = {0};
    uint8_t battery = 0xFF;
    uint8_t mode = 0;
    int8_t rssi = 0;
    uint32_t lastSeenMs = 0;
    uint32_t lastEspNowMs = 0;
    uint32_t lastWifiMs = 0;
  };

  struct Target {
    uint8_t addr[6] = {0};
    uint8_t identity[6] = {0};
    uint16_t port = 0;
    bool viaWifi = false;
    bool webPeer = false;
  };

  static void txTask(void* arg);
  static void rxTask(void* arg);
  static void playTask(void* arg);

  void runTx();
  void runRx();
  void runPlay();

  void handleButtons();
  bool startLiveTx(bool spy = false);
  void stopLiveTx();
  bool startClipRecord(bool spy = false);
  void stopClipRecordAndSend();
  void cancelClipRecord();
  void sendClipStep();
  void cycleMode();
  void changeVolume(int delta);
  void saveVolume();
  void setPlayback(bool enabled);
  void setError();
  void updateState();
  void updateUi();
  void sendHello();
  void sendWebPeerList();
  void sendWebPeerAnnouncements();
  int readBattery() const;

  // Peer table and selection.
  void updatePeer(const RxPacket& packet);
  void expirePeers();
  bool peerAt(int index, Peer& out) const;
  void peerCycleNext();
  void peerToggleCurrent();
  size_t collectTargets(Target* out, size_t max, bool& all) const;
  bool sendToTargets(const uint8_t* data, size_t len);
  bool broadcastAll(const uint8_t* data, size_t len);
  bool broadcastToWebPeers(const uint8_t* data, size_t len);
  bool sendToPeer(const uint8_t* addr, uint16_t port, const uint8_t* data,
                  size_t len);
  bool isLocalNetworkSource(const RxPacket& packet) const;
  bool isDuplicateAudioPacket(const RxPacket& packet);
  void fillPeerUi(UiState& s) const;

  // Spy mode.
  void handleSpyPacket(const RxPacket& packet);
  void handlePendingSpy();
  void sendSpyCommand(uint8_t cmd, const uint8_t* addr, uint16_t port,
                      bool webPeer = false,
                      const uint8_t* webPeerIdentity = nullptr);
  bool routeSpyRelayPacket(const RxPacket& packet);
  int spyTargetIndex() const;
  bool acceptAudio(const uint8_t* src, uint16_t port) const;

  AudioEngine _audio;
  TransportManager _transport;
  IntercomUi _ui;
  SampleRing _jitter;
  SampleRing _clip;
  AdpcmCodec _encoder;
  AdpcmCodec _decoder;

  TaskHandle_t _txTask = nullptr;
  TaskHandle_t _rxTask = nullptr;
  TaskHandle_t _playTask = nullptr;

  std::atomic<Mode> _mode{Mode::Live};
  std::atomic<State> _state{State::Idle};
  std::atomic<bool> _txActive{false};
  std::atomic<bool> _recording{false};
  std::atomic<bool> _sendingClip{false};
  std::atomic<bool> _playbackEnabled{true};
  std::atomic<float> _micLevel{0.0f};
  std::atomic<float> _rxLevel{0.0f};
  std::atomic<int> _battery{-1};
  uint8_t _volume = kDefaultVolumePercent;
  std::atomic<uint32_t> _recordingMs{0};
  std::atomic<uint32_t> _sendProgress{0};

  char _deviceName[16] = {0};
  uint8_t _selfMac[6] = {0};

  Peer _peers[kMaxPeers];
  SemaphoreHandle_t _peerMutex = nullptr;
  int8_t _peerIndex = -1;  // -1 = ALL
  bool _targetAll = true;

  // Controller side of spy mode.
  std::atomic<bool> _spyRequestLive{false};
  std::atomic<bool> _spyRequestClip{false};
  std::atomic<bool> _spyAwaitClip{false};
  uint8_t _spyTargetAddr[6] = {0};
  uint8_t _spyTargetIdentity[6] = {0};
  uint16_t _spyTargetPort = 0;
  bool _spyTargetWebPeer = false;
  uint32_t _spyRequestUntilMs = 0;

  // Target side of spy mode.
  std::atomic<bool> _spyServeLive{false};
  std::atomic<bool> _spyRecording{false};
  uint8_t _spyRequesterAddr[6] = {0};
  uint16_t _spyRequesterPort = 0;
  bool _spyRequesterValid = false;
  uint32_t _spyServeStartMs = 0;
  std::atomic<uint8_t> _pendingSpyCmd{0};
  uint8_t _pendingSpyAddr[6] = {0};
  uint16_t _pendingSpyPort = 0;
  bool _pendingSpyFromWeb = false;
  uint8_t _pendingSpyTargetAddr[6] = {0};
  uint8_t _pendingSpyTargetIdentity[6] = {0};
  uint16_t _pendingSpyTargetPort = 0;
  uint8_t _pendingSpyTargetFlags = 0;
  std::atomic<bool> _spyRelayActive{false};
  bool _spyRelayFromWeb = false;
  uint8_t _spyRelayRequesterAddr[6] = {0};
  uint16_t _spyRelayRequesterPort = 0;
  uint8_t _spyRelayTargetAddr[6] = {0};
  uint8_t _spyRelayTargetIdentity[6] = {0};
  uint16_t _spyRelayTargetPort = 0;

  // Clip send destination snapshot.
  Target _clipTargets[kMaxPeers];
  size_t _clipTargetCount = 0;
  bool _clipBroadcast = false;
  bool _clipSpyReply = false;

  uint16_t _txSeq = 0;
  uint16_t _lastSeq = 0;
  bool _haveLastSeq = false;
  uint8_t _lastAudioSrc[6] = {0};
  uint16_t _lastAudioPort = 0;
  bool _haveLastAudioSrc = false;
  struct RecentAudioPacket {
    uint32_t hash = 0;
    uint32_t receivedAtMs = 0;
    uint16_t len = 0;
  };
  RecentAudioPacket _recentAudioPackets[64];
  size_t _recentAudioPacketIndex = 0;

  uint32_t _lastHelloMs = 0;
  uint32_t _lastBatteryMs = 0;
  uint32_t _lastUiMs = 0;
  uint32_t _recordingStartMs = 0;
  uint32_t _errorUntilMs = 0;
  uint32_t _lastPeerExpireMs = 0;

  size_t _clipSendPos = 0;
  size_t _clipSendTotal = 0;
  uint32_t _clipNextMs = 0;

  alignas(4) uint8_t _txPacket[kMaxPacketSize] = {};
  int16_t _captureFrame[kFrameSamples] = {};
  uint8_t _adpcmFrame[kFrameBytes] = {};
  int16_t _rxFrame[kFrameSamples] = {};
  int16_t _playFrame[kFrameSamples] = {};
};

}  // namespace intercom
