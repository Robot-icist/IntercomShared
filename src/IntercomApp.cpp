#include "IntercomApp.h"

#include <Preferences.h>

#include <algorithm>
#include <cstring>

namespace intercom {

bool IntercomApp::begin() {
  Serial.begin(115200);
  delay(50);

  auto cfg = M5.config();
#if defined(INTERCOM_BOARD_ATOMS3R)
  cfg.internal_mic = true;
  cfg.internal_spk = false;
  cfg.external_speaker.atomic_echo = true;
#else
  cfg.internal_mic = true;
  cfg.internal_spk = true;
#endif

  M5.begin(cfg);
  if (M5.Display.width() <= 0) {
    Serial.println("[app] M5.begin failed");
    return false;
  }

  Serial.printf("\n[app] %s intercom, channel %u\n", INTERCOM_DEVICE_NAME,
                (unsigned)kChannel);

  if (!_audio.begin()) {
    Serial.println("[app] audio init failed");
    return false;
  }

  Preferences prefs;
  if (prefs.begin("intercom", true)) {
    uint8_t stored = prefs.getUChar("volume", kDefaultVolumePercent);
    prefs.end();
    if (stored <= 100) {
      _volume = stored;
    }
  }
  _audio.setVolume((uint8_t)((uint16_t)_volume * 255 / 100));
  Serial.printf("[app] volume %u%%\n", (unsigned)_volume);

  if (!_link.begin()) {
    Serial.println("[app] link init failed");
    return false;
  }
  if (!_ui.begin()) {
    Serial.println("[app] display init failed");
    return false;
  }
  if (!_jitter.begin(kJitterCapacity)) {
    Serial.println("[app] jitter buffer allocation failed");
    return false;
  }
  if (!_clip.begin(kClipCapacity)) {
    Serial.println("[app] clip buffer allocation failed");
    return false;
  }

  _battery.store(readBattery());
  _lastBatteryMs = millis();
  _lastHelloMs = millis();
  sendHello();

  xTaskCreatePinnedToCore(&IntercomApp::txTask, "intercom_tx", 6144, this, 4,
                          &_txTask, 0);
  xTaskCreatePinnedToCore(&IntercomApp::rxTask, "intercom_rx", 6144, this, 3,
                          &_rxTask, 0);
  xTaskCreatePinnedToCore(&IntercomApp::playTask, "intercom_play", 6144, this,
                          3, &_playTask, 0);

  Serial.printf("[app] ready: %u Hz, %u ms frames, %u byte packets\n",
                (unsigned)kSampleRate, (unsigned)kFrameMs,
                (unsigned)(sizeof(PacketHeader) + kFrameBytes));
  return true;
}

void IntercomApp::loop() {
  // Start the speaker only when there is audio to play, and always from this
  // task. M5Unified's codec enable callbacks temporarily reconfigure the
  // shared M5GFX I2C bus; doing that from the playback task races the display,
  // battery and PMIC accesses on the main task and can blank the panel.
  if (_playbackEnabled.load() && _jitter.available() > 0) {
    _audio.ensureSpeaker();
  }

  M5.update();
  handleButtons();

  // Register newly discovered peers outside the Wi-Fi receive callback.
  _link.poll();

  uint32_t now = millis();

  if (now - _lastHelloMs >= kHelloIntervalMs) {
    _lastHelloMs = now;
    sendHello();
  }

  if (now - _lastBatteryMs >= 5000) {
    _lastBatteryMs = now;
    _battery.store(readBattery());
  }

  if (_recording.load()) {
    uint32_t elapsed = now - _recordingStartMs;
    _recordingMs.store(elapsed);
    if (elapsed >= kClipMaxMs) {
      stopClipRecordAndSend();
    }
  }

  updateState();

  if (now - _lastUiMs >= 100) {
    _lastUiMs = now;
    updateUi();
  }

  delay(5);
}

void IntercomApp::handleButtons() {
  if (M5.BtnA.wasDoubleClicked()) {
    if (_recording.load()) {
      cancelClipRecord();
    } else {
      cycleMode();
    }
  }

#if defined(INTERCOM_BOARD_STICKS3)
  if (M5.BtnB.wasClicked()) {
    if (_mode.load() == Mode::Volume) {
      changeVolume(-(int)kVolumeStepPercent);
    } else {
      cycleMode();
    }
  }
#endif

  if (_mode.load() == Mode::Volume) {
    // One-button AtomS3R: click up, hold down. The StickS3 also has BtnB.
    if (M5.BtnA.wasSingleClicked()) {
      changeVolume((int)kVolumeStepPercent);
    }
    if (M5.BtnA.wasHold()) {
      changeVolume(-(int)kVolumeStepPercent);
    }
    return;
  }

  if (_mode.load() == Mode::Live) {
    if (M5.BtnA.wasPressed()) {
      if (!_txActive.load() && !_recording.load() && !_sendingClip.load()) {
        startLiveTx();
      }
    }
    if (M5.BtnA.wasReleased()) {
      if (_txActive.load()) {
        stopLiveTx();
      }
    }
  } else {
    if (M5.BtnA.wasSingleClicked()) {
      if (_recording.load()) {
        stopClipRecordAndSend();
      } else if (!_sendingClip.load()) {
        startClipRecord();
      }
    }
  }
}

void IntercomApp::startLiveTx() {
  if (_txActive.load() || _recording.load() || _sendingClip.load()) {
    return;
  }

  _encoder.reset();
  _txSeq = 0;
  setPlayback(false);
  if (!_audio.startCapture()) {
    setPlayback(true);
    setError();
    return;
  }

  _txActive.store(true);
  Serial.println("[app] live tx start");
}

void IntercomApp::stopLiveTx() {
  if (!_txActive.load()) {
    return;
  }

  _txActive.store(false);
  _audio.stopCapture();
  _micLevel.store(0.0f);
  setPlayback(true);
  Serial.println("[app] live tx stop");
}

void IntercomApp::startClipRecord() {
  if (_recording.load() || _sendingClip.load() || _txActive.load()) {
    return;
  }

  _clip.clear();
  _recordingStartMs = millis();
  _recordingMs.store(0);
  _encoder.reset();

  setPlayback(false);
  if (!_audio.startCapture()) {
    setPlayback(true);
    setError();
    return;
  }

  _recording.store(true);
  Serial.println("[app] clip record start");
}

void IntercomApp::stopClipRecordAndSend() {
  if (!_recording.load()) {
    return;
  }

  _recording.store(false);
  _audio.stopCapture();
  setPlayback(false);
  _micLevel.store(0.0f);

  size_t samples = _clip.available();
  if (samples == 0) {
    setPlayback(true);
    _state.store(State::Idle);
    return;
  }

  _clipSendTotal = samples;
  _clipSendPos = 0;
  _clipNextMs = millis();
  _sendProgress.store(0);
  _encoder.reset();
  _sendingClip.store(true);

  Serial.printf("[app] clip send start, %u samples\n", (unsigned)samples);
}

void IntercomApp::cancelClipRecord() {
  if (!_recording.load()) {
    return;
  }

  _recording.store(false);
  _audio.stopCapture();
  _clip.clear();
  _recordingMs.store(0);
  _micLevel.store(0.0f);
  setPlayback(true);
  _state.store(State::Idle);
  Serial.println("[app] clip record cancelled");
}

void IntercomApp::sendClipStep() {
  size_t total = _clipSendTotal;
  if (_clipSendPos >= total) {
    _sendingClip.store(false);
    _clipSendPos = 0;
    _sendProgress.store(0);
    _encoder.reset();
    setPlayback(true);
    Serial.println("[app] clip send done");
    return;
  }

  size_t want = std::min<size_t>(kFrameSamples, total - _clipSendPos);
  size_t got = _clip.read(_captureFrame, want);
  if (got < kFrameSamples) {
    memset(_captureFrame + got, 0, (kFrameSamples - got) * sizeof(int16_t));
  }

  // The header carries the ADPCM state at the START of this frame so the
  // receiver can decode each packet independently.
  int16_t predictor = _encoder.predictor();
  uint8_t index = _encoder.index();
  size_t bytes = _encoder.encode(_captureFrame, kFrameSamples, _adpcmFrame);

  PacketHeader* header = (PacketHeader*)_txPacket;
  header->magic = kMagic;
  header->version = kVersion;
  header->type = kPacketAudio;
  header->flags = 0;
  header->seq = _txSeq++;
  header->predictor = predictor;
  header->adpcmIndex = index;
  header->payloadLen = (uint8_t)bytes;
  memcpy(_txPacket + sizeof(PacketHeader), _adpcmFrame, bytes);

  _link.send(_txPacket, sizeof(PacketHeader) + bytes);

  _clipSendPos += got;
  _sendProgress.store((uint32_t)((_clipSendPos * 100) / total));
}

void IntercomApp::cycleMode() {
  if (_recording.load() || _sendingClip.load() || _txActive.load()) {
    return;
  }

  Mode next = _mode.load();
  switch (next) {
    case Mode::Live:
      next = Mode::Clip;
      break;
    case Mode::Clip:
      next = Mode::Volume;
      break;
    default:
      next = Mode::Live;
      break;
  }
  _mode.store(next);
  _state.store(State::Idle);
  Serial.printf("[app] mode %s\n", modeName(next));
}

void IntercomApp::changeVolume(int delta) {
  int next = (int)_volume + delta;
  if (next > 100) {
    next = 0;
  } else if (next < 0) {
    next = 100;
  }

  _volume = (uint8_t)next;
  _audio.setVolume((uint8_t)((uint16_t)_volume * 255 / 100));
  saveVolume();
  Serial.printf("[app] volume %u%%\n", (unsigned)_volume);
}

void IntercomApp::saveVolume() {
  Preferences prefs;
  if (prefs.begin("intercom", false)) {
    prefs.putUChar("volume", _volume);
    prefs.end();
  }
}

void IntercomApp::setPlayback(bool enabled) {
  _playbackEnabled.store(enabled);
  _audio.setPlaybackEnabled(enabled);
}

void IntercomApp::setError() {
  _state.store(State::Error);
  _errorUntilMs = millis() + 2000;
}

void IntercomApp::updateState() {
  if ((int32_t)(millis() - _errorUntilMs) < 0 && _errorUntilMs != 0) {
    _state.store(State::Error);
    return;
  }
  if (_recording.load()) {
    _state.store(State::Rec);
    return;
  }
  if (_sendingClip.load()) {
    _state.store(State::Send);
    return;
  }
  if (_txActive.load()) {
    _state.store(State::Tx);
    return;
  }
  if (_playbackEnabled.load() && _jitter.available() > 0) {
    _state.store(State::Rx);
    return;
  }
  _state.store(State::Idle);
}

void IntercomApp::updateUi() {
  UiState s;
  s.state = _state.load();
  s.mode = _mode.load();
  s.peerOnline = _link.hasPeer();
  _link.peerName(s.peerName, sizeof(s.peerName));
  s.peerRssi = _link.peerRssi();
  s.battery = _battery.load();
  s.micLevel = _micLevel.load();
  s.rxLevel = _rxLevel.load();
  s.recordingMs = _recordingMs.load();
  s.sendProgress = _sendProgress.load();
  s.volume = _volume;
  s.sampleRate = kSampleRate;
  s.channel = kChannel;
  _ui.render(s);
}

void IntercomApp::sendHello() {
  int battery = _battery.load();
  uint8_t value = (battery < 0) ? 0xFF : (uint8_t)battery;
  _link.sendHello(INTERCOM_DEVICE_NAME, value, _mode.load());
}

int IntercomApp::readBattery() const {
  int level = (int)M5.Power.getBatteryLevel();
  if (level < 0 || level > 100) {
    return -1;
  }
  return level;
}

void IntercomApp::runTx() {
  while (true) {
    if (_txActive.load()) {
      size_t got = _audio.readFrame(_captureFrame, kFrameSamples);
      if (got == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      if (got < kFrameSamples) {
        memset(_captureFrame + got, 0, (kFrameSamples - got) * sizeof(int16_t));
      }

      int16_t predictor = _encoder.predictor();
      uint8_t index = _encoder.index();
      size_t bytes = _encoder.encode(_captureFrame, kFrameSamples, _adpcmFrame);

      PacketHeader* header = (PacketHeader*)_txPacket;
      header->magic = kMagic;
      header->version = kVersion;
      header->type = kPacketAudio;
      header->flags = 0;
      header->seq = _txSeq++;
      header->predictor = predictor;
      header->adpcmIndex = index;
      header->payloadLen = (uint8_t)bytes;
      memcpy(_txPacket + sizeof(PacketHeader), _adpcmFrame, bytes);

      _link.send(_txPacket, sizeof(PacketHeader) + bytes);
      continue;
    }

    if (_recording.load()) {
      size_t got = _audio.readFrame(_captureFrame, kFrameSamples);
      if (got == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      if (got < kFrameSamples) {
        memset(_captureFrame + got, 0, (kFrameSamples - got) * sizeof(int16_t));
      }

      _clip.write(_captureFrame, kFrameSamples);
      continue;
    }

    if (_sendingClip.load()) {
      sendClipStep();
      _clipNextMs += kFrameMs;
      int32_t wait = (int32_t)(_clipNextMs - millis());
      if (wait > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait));
      }
      continue;
    }

    _micLevel.store(0.0f);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void IntercomApp::runRx() {
  RxPacket packet;
  while (true) {
    if (!_link.popPacket(packet, 50)) {
      continue;
    }
    if (packet.len < sizeof(PacketHeader)) {
      continue;
    }

    const PacketHeader* header = (const PacketHeader*)packet.data;
    if (header->magic != kMagic || header->version != kVersion ||
        header->type != kPacketAudio) {
      continue;
    }
    if (header->payloadLen == 0 ||
        header->payloadLen > packet.len - sizeof(PacketHeader)) {
      continue;
    }

    if (_haveLastSeq) {
      uint16_t gap = (uint16_t)(header->seq - _lastSeq);
      if (gap > 1 && gap <= kMaxGapFrames) {
        static const int16_t silence[kFrameSamples] = {0};
        for (uint16_t i = 1; i < gap; ++i) {
          _jitter.write(silence, kFrameSamples);
        }
      }
    }
    _lastSeq = header->seq;
    _haveLastSeq = true;

    _decoder.setState(header->predictor, header->adpcmIndex);
    size_t samples = _decoder.decode(packet.data + sizeof(PacketHeader),
                                     header->payloadLen, _rxFrame, kFrameSamples);
    if (samples > 0) {
      _jitter.write(_rxFrame, samples);
      _rxLevel.store(pcmLevel(_rxFrame, samples));
    }
  }
}

void IntercomApp::runPlay() {
  bool primed = false;
  uint32_t nextMs = millis();

  while (true) {
    if (!_playbackEnabled.load()) {
      primed = false;
      nextMs = millis();
      _rxLevel.store(0.0f);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!primed) {
      if (_jitter.available() < kPrebufferSamples) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      primed = true;
      nextMs = millis();
    }

    size_t got = _jitter.read(_playFrame, kFrameSamples);
    if (got == 0) {
      primed = false;
      _rxLevel.store(0.0f);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (got < kFrameSamples) {
      memset(_playFrame + got, 0, (kFrameSamples - got) * sizeof(int16_t));
    }

    if (!_audio.playFrame(_playFrame, kFrameSamples)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      _audio.playFrame(_playFrame, kFrameSamples);
    }
    _rxLevel.store(pcmLevel(_playFrame, kFrameSamples));

    nextMs += kFrameMs;
    int32_t wait = (int32_t)(nextMs - millis());
    if (wait > 0) {
      vTaskDelay(pdMS_TO_TICKS(wait));
    }
  }
}

void IntercomApp::txTask(void* arg) {
  static_cast<IntercomApp*>(arg)->runTx();
}

void IntercomApp::rxTask(void* arg) {
  static_cast<IntercomApp*>(arg)->runRx();
}

void IntercomApp::playTask(void* arg) {
  static_cast<IntercomApp*>(arg)->runPlay();
}

}  // namespace intercom
