#include "AudioEngine.h"

#include <algorithm>
#include <cstring>

#include "IntercomTypes.h"

namespace intercom {

bool AudioEngine::begin() {
  _mutex = xSemaphoreCreateMutex();
  if (!_mutex) {
    return false;
  }

  auto mic = M5.Mic.config();
  mic.sample_rate = kSampleRate;
  mic.stereo = false;
  // M5Unified's capture task reads two DMA descriptors per pass. For a mono
  // port that is dma_buf_len * 2 samples. Keep that equal to one intercom
  // frame, otherwise the second half of the read is discarded when the next
  // request is not already queued and the effective rate halves.
  mic.dma_buf_len = kFrameSamples / 2;
  mic.dma_buf_count = 4;
  mic.task_priority = 2;
  mic.task_pinned_core = 1;
  M5.Mic.config(mic);

  auto spk = M5.Speaker.config();
  spk.sample_rate = kSampleRate;
  // Keep the board-specific magnification from M5Unified (1 for the Atomic
  // Echo base and StickS3). Overriding it with a large value clips playback.
  spk.dma_buf_len = kFrameSamples;
  spk.dma_buf_count = 8;
  spk.task_priority = 2;
  spk.task_pinned_core = 1;
  M5.Speaker.config(spk);
  M5.Speaker.setVolume(_volume);

  return true;
}

void AudioEngine::end() {
  stopCapture();
  stopPlayback();
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
}

bool AudioEngine::startCapture() {
  if (_captureActive.load()) {
    return true;
  }
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }

  // The AtomS3R voice base uses one I2S peripheral for both directions. The
  // microphone cannot start until the speaker driver has released it.
  if (kSharedAudioPort && _speakerRunning.load()) {
    stopSpeaker();
  }

  bool ok = M5.Mic.begin();
  if (ok) {
    _micRunning.store(true);
    _captureActive.store(true);
  }

  xSemaphoreGive(_mutex);
  if (!ok) {
    Serial.println("[audio] mic begin failed");
  }
  return ok;
}

void AudioEngine::stopCapture() {
  _captureActive.store(false);
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (_micRunning.load()) {
      M5.Mic.end();
      _micRunning.store(false);
    }
    xSemaphoreGive(_mutex);
  }
  _captureLevel.store(0.0f);
}

size_t AudioEngine::readFrame(int16_t* samples, size_t count) {
  if (!samples || count == 0 || !_captureActive.load() || !_mutex) {
    return 0;
  }
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }

  size_t got = 0;
  if (_captureActive.load() && _micRunning.load()) {
    // M5.Mic.record() only queues the request; the capture task fills the
    // buffer asynchronously. Wait for the request to be released before
    // reading it, otherwise we encode stale samples and race the capture
    // task writing into the same buffer.
    bool ok = M5.Mic.record(samples, count, kSampleRate, false);
    if (ok) {
      const uint32_t start = millis();
      while (M5.Mic.isRecording() != 0 && _captureActive.load() &&
             (uint32_t)(millis() - start) < 100) {
        vTaskDelay(1);
      }
      if (M5.Mic.isRecording() == 0) {
        got = count;
      }
    }
  }

  xSemaphoreGive(_mutex);
  if (got > 0) {
    _captureLevel.store(pcmLevel(samples, got));
  }
  return got;
}

void AudioEngine::setPlaybackEnabled(bool enabled) {
  _playbackEnabled.store(enabled);
  if (!enabled) {
    // On the StickS3 the speaker task keeps running. Let the already queued
    // frames drain; the caller simply stops feeding new ones. On the AtomS3R
    // startCapture() tears the speaker down to free the shared I2S port.
    _playbackLevel.store(0.0f);
  }
}

bool AudioEngine::startSpeaker() {
  if (_speakerRunning.load()) {
    return true;
  }

  M5.Speaker.setBufferReleaseCallback(this, &AudioEngine::onBufferReleased);
  bool ok = M5.Speaker.begin();
  if (ok) {
    M5.Speaker.setVolume(_volume);
    _speakerRunning.store(true);
  } else {
    Serial.println("[audio] speaker begin failed");
  }
  return ok;
}

void AudioEngine::stopSpeaker() {
  if (!_speakerRunning.load()) {
    return;
  }
  M5.Speaker.end();
  _speakerRunning.store(false);
  _playbackLevel.store(0.0f);
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    _playBusy[i].store(false);
  }
}

bool AudioEngine::playFrame(const int16_t* samples, size_t count) {
  if (!samples || count == 0 || !_playbackEnabled.load() || !_mutex) {
    return false;
  }

  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }

  if (!_playbackEnabled.load()) {
    xSemaphoreGive(_mutex);
    return false;
  }

  if (!_speakerRunning.load() && !startSpeaker()) {
    xSemaphoreGive(_mutex);
    return false;
  }

  int index = -1;
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    bool expected = false;
    if (_playBusy[i].compare_exchange_strong(expected, true)) {
      index = (int)i;
      break;
    }
  }
  if (index < 0) {
    xSemaphoreGive(_mutex);
    return false;
  }

  int16_t* slot = _playBuffers[index];
  memcpy(slot, samples, count * sizeof(int16_t));
  if (count < kPlayBufferSamples) {
    memset(slot + count, 0, (kPlayBufferSamples - count) * sizeof(int16_t));
  }

  _playbackLevel.store(pcmLevel(slot, count));
  bool queued = M5.Speaker.playRaw(slot, kPlayBufferSamples, kSampleRate, false, 1, 0, false);
  if (!queued) {
    _playBusy[index].store(false);
  }
  xSemaphoreGive(_mutex);
  return queued;
}

void AudioEngine::stopPlayback() {
  if (_speakerRunning.load()) {
    M5.Speaker.end();
    _speakerRunning.store(false);
  }
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    _playBusy[i].store(false);
  }
  _playbackLevel.store(0.0f);
}

void AudioEngine::setVolume(uint8_t volume) {
  _volume = volume;
  M5.Speaker.setVolume(volume);
}

void AudioEngine::onBufferReleased(void* args, const void* data, uint8_t channel) {
  (void)channel;
  if (args) {
    static_cast<AudioEngine*>(args)->releasePlayBuffer(data);
  }
}

void AudioEngine::releasePlayBuffer(const void* data) {
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    if (data == _playBuffers[i]) {
      _playBusy[i].store(false);
      return;
    }
  }
}

}  // namespace intercom
