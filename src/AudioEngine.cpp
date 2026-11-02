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
  // M5Unified gives StickS3 magnification 1 while its ES8311 DAC runs at 0 dB,
  // which makes it about 18 dB quieter than the other ES8311 boards. In stereo
  // at 16 kHz, magnification 8 is unity digital gain (no extra clipping).
#if defined(INTERCOM_BOARD_STICKS3)
  spk.magnification = 8;
#endif
  spk.dma_buf_len = kFrameSamples;
  spk.dma_buf_count = 8;
  spk.task_priority = 2;
  spk.task_pinned_core = 1;
  M5.Speaker.config(spk);
  M5.Speaker.setVolume(_volume);

  // The speaker is started from ensureSpeaker() on the application task, once
  // the UI is up. Starting it here or, worse, from the playback task, runs the
  // codec/PMIC I2C setup outside the task that owns M5.update() and the
  // battery poll, which can take the panel/backlight down on the ESP32-S3.
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

  // Never run the microphone and the speaker together. The AtomS3R voice base
  // shares one I2S peripheral, and the StickS3 speaker and microphone are two
  // I2S masters wired to the same MCLK/BCK/WS pins. Either way, starting the
  // microphone while the speaker is live corrupts the codec clocking.
  if (_speakerRunning.load()) {
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
    // Let the already queued frames drain; the caller stops feeding new ones.
    // startCapture() tears the speaker down before it starts the microphone.
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
#if defined(INTERCOM_BOARD_STICKS3)
    // M5Unified starts the StickS3 ES8311 at 0xBF (0 dB). Keep at 0 dB (0xBF)
    // instead of +6 dB (0xCB) to minimize peak speaker current and avoid brownout
    // on battery. Digital magnification (8x) provides sufficient headroom for
    // speech; analog gain reduction cuts peak power by ~75% vs +6dB.
    M5.In_I2C.writeRegister8(0x18, 0x32, 0xBF, 100000);

    // Battery-aware volume limiting: read voltage and cap max volume if low.
    // M5.Power.getBatteryVoltage() returns mV. Below 3.3V, limit to ~70% volume.
    uint16_t batMv = M5.Power.getBatteryVoltage();
    if (batMv > 0 && batMv < kBrownoutVoltageMv) {
      _maxAllowedVolume = kLowBatteryMaxVolume;
      Serial.printf("[audio] low battery %u mV, limiting volume to %u\n", batMv, _maxAllowedVolume);
    } else {
      _maxAllowedVolume = 255;
    }
#endif
    // Soft-start: begin at zero volume and ramp up over the first few frames
    // to prevent brownout on StickS3 when the first packet arrives at high volume.
    _softStartRemaining = kSoftStartFrames;
    // First-frame mute: heavily attenuate first N frames to suppress transients
    // (button clicks) even if soft-start was interrupted by rapid restarts.
    _firstFrameMuteRemaining = kFirstFrameMuteFrames;
    _volume = 0;
    M5.Speaker.setVolume(0);
    _speakerRunning.store(true);
    _lastSpeakerTryMs = 0;
  } else {
    _lastSpeakerTryMs = millis();
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
  _lastSpeakerTryMs = 0;
  _lastSpeakerStopMs = millis();  // Record stop time for minimum restart interval
  _playbackLevel.store(0.0f);
  _softStartRemaining = 0;
  _firstFrameMuteRemaining = 0;
  _volume = _targetVolume;
  _maxAllowedVolume = 255;
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    _playBusy[i].store(false);
  }
}

bool AudioEngine::ensureSpeaker() {
  if (!_mutex) {
    return false;
  }
  if (_speakerRunning.load()) {
    return true;
  }
  if (!_playbackEnabled.load() || _captureActive.load()) {
    return false;
  }

  // Minimum restart interval: prevent rapid speaker on/off cycles from
  // AtomS3R rapid clicking which defeats soft-start and causes brownout.
  if (_lastSpeakerStopMs != 0 &&
      (uint32_t)(millis() - _lastSpeakerStopMs) < kMinRestartIntervalMs) {
    return false;
  }

  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  bool ok = _speakerRunning.load();
  if (!ok && _playbackEnabled.load() && !_captureActive.load()) {
    // Do not hammer the codec with a fresh I2C probe every loop iteration if
    // it failed to start (wrong volume, missing base, etc.).
    if (_lastSpeakerTryMs == 0 ||
        (uint32_t)(millis() - _lastSpeakerTryMs) >= kSpeakerRetryMs) {
      ok = startSpeaker();
      if (!ok) {
        _lastSpeakerTryMs = millis();
      }
    }
  }
  xSemaphoreGive(_mutex);
  return ok;
}

bool AudioEngine::playFrame(const int16_t* samples, size_t count) {
  if (!samples || count == 0 || !_playbackEnabled.load() || !_mutex) {
    return false;
  }

  // The speaker is owned by the application task. If it is not running yet
  // (e.g. just after the shared-port microphone was released), drop this frame
  // instead of re-initialising M5Unified from the audio task.
  if (!_speakerRunning.load()) {
    return false;
  }

  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }

  if (!_playbackEnabled.load() || !_speakerRunning.load()) {
    xSemaphoreGive(_mutex);
    return false;
  }

  // playRaw() with an explicit channel waits for a free speaker slot, and the
  // wait is unbounded if the speaker task ever stalls. A real-time producer
  // must drop the frame instead of blocking the audio task while holding the
  // mutex (which would also stall capture start/stop on the main task).
  if (M5.Speaker.isPlaying(0) >= 2) {
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

  // First-frame mute: heavily attenuate first N frames ALWAYS to suppress
  // transients (button clicks on AtomS3R) even if soft-start was interrupted
  // by rapid restarts. This runs independently of soft-start volume ramp.
  if (_firstFrameMuteRemaining > 0) {
    --_firstFrameMuteRemaining;
    // 1/8 amplitude for frame 1, 2/8 for frame 2, etc.
    uint16_t scaleNumerator = kFirstFrameMuteFrames - _firstFrameMuteRemaining;
    uint16_t scaleDenominator = kFirstFrameMuteFrames * 4;  // Extra 4x attenuation
    for (size_t i = 0; i < count; ++i) {
      slot[i] = (int16_t)((int32_t)slot[i] * scaleNumerator / scaleDenominator);
    }
  }

  // Soft-start: also attenuate the first few frames at the sample level to
  // protect against loud transients (e.g. button clicks) even if volume ramp
  // is interrupted by rapid start/stop cycles.
  if (_softStartRemaining > 0) {
    uint8_t framesDone = kSoftStartFrames - _softStartRemaining;
    // Linear ramp: 1/15, 2/15, 3/15... of full amplitude
    uint16_t scaleNumerator = framesDone;
    uint16_t scaleDenominator = kSoftStartFrames;
    for (size_t i = 0; i < count; ++i) {
      slot[i] = (int16_t)((int32_t)slot[i] * scaleNumerator / scaleDenominator);
    }
  }

  _playbackLevel.store(pcmLevel(slot, count));

  // Soft-start: ramp volume up over the first kSoftStartFrames frames to avoid
  // brownout on StickS3 when the first packet arrives at high volume.
  if (_softStartRemaining > 0) {
    --_softStartRemaining;
    // Use the lower of target volume and battery-limited max volume.
    uint8_t effectiveTarget = _targetVolume < _maxAllowedVolume ? _targetVolume : _maxAllowedVolume;
    uint8_t nextVolume = (effectiveTarget * (kSoftStartFrames - _softStartRemaining)) / kSoftStartFrames;
    if (nextVolume != _volume) {
      _volume = nextVolume;
      M5.Speaker.setVolume(_volume);
    }
  }

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
  _lastSpeakerStopMs = millis();  // Record stop time for minimum restart interval
  _softStartRemaining = 0;
  _firstFrameMuteRemaining = 0;
  _volume = _targetVolume;
  _maxAllowedVolume = 255;
  for (size_t i = 0; i < kPlayBufferCount; ++i) {
    _playBusy[i].store(false);
  }
  _playbackLevel.store(0.0f);
}

void AudioEngine::setVolume(uint8_t volume) {
  _targetVolume = volume;
  // If soft-start is not active, apply immediately (capped by battery limit).
  if (_softStartRemaining == 0) {
    uint8_t effectiveVolume = volume < _maxAllowedVolume ? volume : _maxAllowedVolume;
    if (_volume != effectiveVolume) {
      _volume = effectiveVolume;
      M5.Speaker.setVolume(effectiveVolume);
    }
  }
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
