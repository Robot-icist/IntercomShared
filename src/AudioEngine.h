#pragma once

#include <Arduino.h>
#include <M5Unified.h>

#include <atomic>

#include "IntercomConfig.h"

namespace intercom {

// Small wrapper around M5Unified's Mic and Speaker classes.
//
// It handles the AtomS3R half-duplex turnaround (one I2S peripheral is shared)
// and keeps the StickS3 speaker running while its separate microphone port is
// capturing. Playback buffers are returned to us by M5Unified's release
// callback, so streaming can reuse a fixed pool safely.
class AudioEngine {
 public:
  bool begin();
  void end();

  bool startCapture();
  void stopCapture();
  bool captureActive() const { return _captureActive.load(); }

  size_t readFrame(int16_t* samples, size_t count);

  void setPlaybackEnabled(bool enabled);
  bool playbackEnabled() const { return _playbackEnabled.load(); }
  // Starts the speaker from the caller's task. M5Unified codec/I2S setup must
  // stay on the same task as M5.update()/battery polling, so the application
  // calls this from its main loop. Never start the speaker from the audio task.
  bool ensureSpeaker();
  bool playFrame(const int16_t* samples, size_t count);
  void stopPlayback();
  void setVolume(uint8_t volume);

  float captureLevel() const { return _captureLevel.load(); }
  float playbackLevel() const { return _playbackLevel.load(); }

 private:
  static constexpr size_t kPlayBufferCount = 6;
  static constexpr size_t kPlayBufferSamples = kFrameSamples;
  static constexpr uint32_t kSpeakerRetryMs = 500;
  // Soft-start: ramp volume over ~100 ms (5 frames @ 20 ms) to avoid
  // brownout on StickS3 when the first packet arrives at high volume.
  static constexpr uint8_t kSoftStartFrames = 15;
   // First-frame protection: always heavily attenuate first N frames to
   // suppress transients (button clicks) even if soft-start was interrupted.
   static constexpr uint8_t kFirstFrameMuteFrames = 2;
   // Minimum time between speaker restarts to prevent rapid cycling.
   static constexpr uint32_t kMinRestartIntervalMs = 500;
   // Battery-aware volume limiting: below this voltage (mV), reduce max volume.
   static constexpr uint16_t kBrownoutVoltageMv = 3300;  // 3.3V - safe margin above brownout
   static constexpr uint8_t kLowBatteryMaxVolume = 180;  // ~70% of 255

  bool startSpeaker();
  void stopSpeaker();
  void releasePlayBuffer(const void* data);
  static void onBufferReleased(void* args, const void* data, uint8_t channel);

  SemaphoreHandle_t _mutex = nullptr;
  int16_t _playBuffers[kPlayBufferCount][kPlayBufferSamples] = {};
  std::atomic<bool> _playBusy[kPlayBufferCount] = {};
  uint8_t _volume = kDefaultVolume;
  uint8_t _targetVolume = kDefaultVolume;
  uint8_t _softStartRemaining = 0;
   uint8_t _firstFrameMuteRemaining = 0;
    uint8_t _maxAllowedVolume = 255;  // Capped by battery voltage
   uint32_t _lastSpeakerStopMs = 0;  // For minimum restart interval

   std::atomic<bool> _captureActive{false};
  std::atomic<bool> _playbackEnabled{true};
  std::atomic<bool> _micRunning{false};
  std::atomic<bool> _speakerRunning{false};
  uint32_t _lastSpeakerTryMs = 0;
  std::atomic<float> _captureLevel{0.0f};
  std::atomic<float> _playbackLevel{0.0f};
};

}  // namespace intercom
