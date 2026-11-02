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

  bool startSpeaker();
  void stopSpeaker();
  void releasePlayBuffer(const void* data);
  static void onBufferReleased(void* args, const void* data, uint8_t channel);

  SemaphoreHandle_t _mutex = nullptr;
  int16_t _playBuffers[kPlayBufferCount][kPlayBufferSamples] = {};
  std::atomic<bool> _playBusy[kPlayBufferCount] = {};
  uint8_t _volume = kDefaultVolume;

  std::atomic<bool> _captureActive{false};
  std::atomic<bool> _playbackEnabled{true};
  std::atomic<bool> _micRunning{false};
  std::atomic<bool> _speakerRunning{false};
  uint32_t _lastSpeakerTryMs = 0;
  std::atomic<float> _captureLevel{0.0f};
  std::atomic<float> _playbackLevel{0.0f};
};

}  // namespace intercom
