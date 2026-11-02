#pragma once

#include <Arduino.h>

#include <atomic>

#include "AdpcmCodec.h"
#include "AudioEngine.h"
#include "EspNowLink.h"
#include "IntercomConfig.h"
#include "IntercomTypes.h"
#include "IntercomUi.h"
#include "SampleRing.h"

namespace intercom {

class IntercomApp {
 public:
  bool begin();
  void loop();

 private:
  static void txTask(void* arg);
  static void rxTask(void* arg);
  static void playTask(void* arg);

  void runTx();
  void runRx();
  void runPlay();

  void handleButtons();
  void startLiveTx();
  void stopLiveTx();
  void startClipRecord();
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
  int readBattery() const;

  AudioEngine _audio;
  EspNowLink _link;
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

  uint16_t _txSeq = 0;
  uint16_t _lastSeq = 0;
  bool _haveLastSeq = false;

  uint32_t _lastHelloMs = 0;
  uint32_t _lastBatteryMs = 0;
  uint32_t _lastUiMs = 0;
  uint32_t _recordingStartMs = 0;
  uint32_t _errorUntilMs = 0;

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
