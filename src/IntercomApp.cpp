#include "IntercomApp.h"

#include <Preferences.h>

#include <algorithm>
#include <cstdio>
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

  // Give every unit a unique short name so identical boards can be told apart.
  uint64_t mac = ESP.getEfuseMac();
  snprintf(_deviceName, sizeof(_deviceName), "%.10s-%04X", INTERCOM_DEVICE_NAME,
           (unsigned)(mac & 0xFFFF));

  Serial.printf("\n[app] %s intercom, channel %u\n", _deviceName,
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

  _peerMutex = xSemaphoreCreateMutex();
  if (!_peerMutex) {
    Serial.println("[app] peer mutex allocation failed");
    return false;
  }
  // Initialize transport manager (ESP-NOW + WiFi)
  if (!_transport.begin(_deviceName)) {
    Serial.println("[app] transport init failed");
    return false;
  }
  WiFi.macAddress(_selfMac);

  // Register transport change callback for UI updates
  _transport.onTransportChange([this](const char* type) {
    Serial.printf("[app] Transport changed to: %s\n", type);
    // UI will pick up peer status automatically via hasPeer()
  });

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

  // Poll transport for peer management, discovery, etc.
  _transport.loop();
  handlePendingSpy();

  uint32_t now = millis();

  if (now - _lastPeerExpireMs >= 1000) {
    _lastPeerExpireMs = now;
    expirePeers();
  }

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

  if (_spyServeLive.load() && now - _spyServeStartMs >= kSpyLiveMaxMs) {
    stopLiveTx();
  }
  if ((_spyRequestLive.load() || _spyRequestClip.load() ||
       _spyAwaitClip.load()) &&
      (int32_t)(now - _spyRequestUntilMs) >= 0) {
    _spyRequestLive.store(false);
    _spyRequestClip.store(false);
    _spyAwaitClip.store(false);
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

  const Mode mode = _mode.load();

  if (mode == Mode::Volume) {
    // One-button AtomS3R: click up, hold down. The StickS3 also has BtnB.
    if (M5.BtnA.wasSingleClicked()) {
      changeVolume((int)kVolumeStepPercent);
    }
    if (M5.BtnA.wasHold()) {
      changeVolume(-(int)kVolumeStepPercent);
    }
    return;
  }

  if (mode == Mode::Live) {
    if (M5.BtnA.wasPressed()) {
      if (!_txActive.load() && !_recording.load() && !_sendingClip.load()) {
        startLiveTx();
      }
    }
    if (M5.BtnA.wasReleased()) {
      // Do not let a local tap stop a remote spy stream.
      if (_txActive.load() && !_spyServeLive.load()) {
        stopLiveTx();
      }
    }
    return;
  }

  if (mode == Mode::Clip) {
    if (M5.BtnA.wasSingleClicked()) {
      if (_recording.load()) {
        stopClipRecordAndSend();
      } else if (!_sendingClip.load()) {
        startClipRecord();
      }
    }
    return;
  }

  if (mode == Mode::Peer) {
    if (M5.BtnA.wasSingleClicked()) {
      peerToggleCurrent();
    }
    if (M5.BtnA.wasHold()) {
      peerCycleNext();
    }
    return;
  }

  // Spy mode: hold to listen live, click to record a clip on the peer.
  if (M5.BtnA.wasHold()) {
    if (!_spyRequestLive.load() && !_spyRequestClip.load() &&
        !_spyAwaitClip.load()) {
      int index = spyTargetIndex();
      Peer peer;
      if (index < 0 || !peerAt(index, peer)) {
        setError();
        return;
      }
      memcpy(_spyTargetAddr, peer.addr, sizeof(_spyTargetAddr));
      _spyTargetPort = peer.port;
      _jitter.clear();
      sendSpyCommand(kSpyLiveStart, peer.addr, peer.port);
      _spyRequestLive.store(true);
      _spyRequestUntilMs = millis() + kSpyLiveMaxMs;
    }
  }

  if (M5.BtnA.wasReleased()) {
    if (_spyRequestLive.load()) {
      sendSpyCommand(kSpyLiveStop, _spyTargetAddr, _spyTargetPort);
      _spyRequestLive.store(false);
    }
  }

  if (M5.BtnA.wasSingleClicked()) {
    if (_spyRequestClip.load()) {
      sendSpyCommand(kSpyClipStop, _spyTargetAddr, _spyTargetPort);
      _spyRequestClip.store(false);
      _spyAwaitClip.store(true);
      _spyRequestUntilMs = millis() + kSpyClipTimeoutMs;
    } else if (!_spyRequestLive.load() && !_spyAwaitClip.load()) {
      int index = spyTargetIndex();
      Peer peer;
      if (index < 0 || !peerAt(index, peer)) {
        setError();
        return;
      }
      memcpy(_spyTargetAddr, peer.addr, sizeof(_spyTargetAddr));
      _spyTargetPort = peer.port;
      _jitter.clear();
      sendSpyCommand(kSpyClipStart, peer.addr, peer.port);
      _spyRequestClip.store(true);
      _spyRequestUntilMs = millis() + kSpyClipTimeoutMs;
    }
  }
}

bool IntercomApp::startLiveTx(bool spy) {
  if (_txActive.load() || _recording.load() || _sendingClip.load()) {
    return false;
  }

  _encoder.reset();
  _txSeq = 0;
  setPlayback(false);
  if (!_audio.startCapture()) {
    setPlayback(true);
    setError();
    return false;
  }

  _spyServeLive.store(spy);
  if (spy) {
    _spyServeStartMs = millis();
  }
  _txActive.store(true);
  Serial.println(spy ? "[app] spy live start" : "[app] live tx start");
  return true;
}

void IntercomApp::stopLiveTx() {
  if (!_txActive.load()) {
    return;
  }

  _txActive.store(false);
  _spyServeLive.store(false);
  _spyRequesterValid = false;
  _audio.stopCapture();
  _micLevel.store(0.0f);
  setPlayback(true);
  Serial.println("[app] live tx stop");
}

bool IntercomApp::startClipRecord(bool spy) {
  if (_recording.load() || _sendingClip.load() || _txActive.load()) {
    return false;
  }

  _clip.clear();
  _recordingStartMs = millis();
  _recordingMs.store(0);
  _encoder.reset();
  _txSeq = 0;

  setPlayback(false);
  if (!_audio.startCapture()) {
    setPlayback(true);
    setError();
    return false;
  }

  _spyRecording.store(spy);
  _recording.store(true);
  Serial.println(spy ? "[app] spy clip record start" : "[app] clip record start");
  return true;
}

void IntercomApp::stopClipRecordAndSend() {
  if (!_recording.load()) {
    return;
  }

  _recording.store(false);
  _audio.stopCapture();
  setPlayback(false);
  _micLevel.store(0.0f);

  _clipSpyReply = _spyRecording.load();
  _spyRecording.store(false);
  if (_clipSpyReply) {
    _clipBroadcast = false;
    _clipTargetCount = 1;
    memcpy(_clipTargets[0].addr, _spyRequesterAddr,
           sizeof(_clipTargets[0].addr));
    _clipTargets[0].port = _spyRequesterPort;
    _clipTargets[0].viaWifi = _spyRequesterPort != 0;
  } else {
    bool all = false;
    _clipTargetCount = collectTargets(_clipTargets, kMaxPeers, all);
    _clipBroadcast = all;
  }

  size_t samples = _clip.available();
  if (samples == 0) {
    if (_clipSpyReply) {
      sendSpyCommand(kSpyClipDone, _spyRequesterAddr, _spyRequesterPort);
      _clipSpyReply = false;
      _spyRequesterValid = false;
    }
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
  if (_spyRecording.load()) {
    _spyRecording.store(false);
    sendSpyCommand(kSpyClipDone, _spyRequesterAddr, _spyRequesterPort);
    _spyRequesterValid = false;
  }
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
    if (_clipSpyReply) {
      sendSpyCommand(kSpyClipDone, _spyRequesterAddr, _spyRequesterPort);
      _clipSpyReply = false;
      _spyRequesterValid = false;
    }
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

  const size_t len = sizeof(PacketHeader) + bytes;
  if (_clipBroadcast) {
    broadcastAll(_txPacket, len);
  } else {
    for (size_t i = 0; i < _clipTargetCount; ++i) {
      sendToPeer(_clipTargets[i].addr, _clipTargets[i].port, _txPacket, len);
    }
  }

  _clipSendPos += got;
  _sendProgress.store((uint32_t)((_clipSendPos * 100) / total));
}

void IntercomApp::cycleMode() {
  if (_recording.load() || _sendingClip.load() || _txActive.load() ||
      _spyRequestLive.load() || _spyRequestClip.load() ||
      _spyAwaitClip.load()) {
    return;
  }

  Mode next = _mode.load();
  switch (next) {
    case Mode::Live:
      next = Mode::Clip;
      break;
    case Mode::Clip:
      next = Mode::Spy;
      break;
    case Mode::Spy:
      next = Mode::Peer;
      break;
    case Mode::Peer:
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
  if (_spyRequestLive.load() || _spyRequestClip.load() ||
      _spyAwaitClip.load() || _spyServeLive.load()) {
    _state.store(State::Spy);
    return;
  }
  _state.store(State::Idle);
}

void IntercomApp::updateUi() {
  Transport* t = _transport.activeTransport();
  UiState s;
  s.state = _state.load();
  s.mode = _mode.load();
  strncpy(s.selfName, _deviceName, sizeof(s.selfName) - 1);
  s.transportType = t ? t->typeName() : "none";
  fillPeerUi(s);
  s.battery = _battery.load();
  s.micLevel = _micLevel.load();
  s.rxLevel = _rxLevel.load();
  s.recordingMs = _recordingMs.load();
  s.sendProgress = _sendProgress.load();
  s.volume = _volume;
  s.sampleRate = kSampleRate;
  s.channel = kChannel;
  s.spyLive = _spyRequestLive.load() || _spyServeLive.load();
  s.spyClip = _spyRequestClip.load() || _spyRecording.load();
  s.spyAwait = _spyAwaitClip.load();
  s.spy = s.spyLive || s.spyClip || s.spyAwait;
  _ui.render(s);
}

void IntercomApp::sendHello() {
  int battery = _battery.load();
  uint8_t value = (battery < 0) ? 0xFF : (uint8_t)battery;
  Transport* t = _transport.activeTransport();
  if (t) {
    t->sendHello(_deviceName, value, _mode.load());
  }
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

      const size_t len = sizeof(PacketHeader) + bytes;
      if (_spyServeLive.load()) {
        sendToPeer(_spyRequesterAddr, _spyRequesterPort, _txPacket, len);
      } else {
        sendToTargets(_txPacket, len);
      }
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
    Transport* t = _transport.activeTransport();
    if (!t || !t->popPacket(packet, 50)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (packet.len < sizeof(PacketHeader)) {
      continue;
    }

    const PacketHeader* header = (const PacketHeader*)packet.data;
    if (header->magic != kMagic || header->version != kVersion ||
        header->payloadLen > packet.len - sizeof(PacketHeader)) {
      continue;
    }

    if (header->type == kPacketHello) {
      updatePeer(packet);
      continue;
    }
    if (header->type == kPacketSpy) {
      handleSpyPacket(packet);
      continue;
    }
    if (header->type != kPacketAudio || header->payloadLen == 0) {
      continue;
    }
    if (!acceptAudio(packet.src, packet.port)) {
      continue;
    }

    if (!_haveLastAudioSrc ||
        memcmp(_lastAudioSrc, packet.src, sizeof(_lastAudioSrc)) != 0 ||
        _lastAudioPort != packet.port) {
      memcpy(_lastAudioSrc, packet.src, sizeof(_lastAudioSrc));
      _lastAudioPort = packet.port;
      _haveLastAudioSrc = true;
      _haveLastSeq = false;
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

void IntercomApp::updatePeer(const RxPacket& packet) {
  const PacketHeader* header = (const PacketHeader*)packet.data;
  if (header->payloadLen < sizeof(HelloPayload)) {
    return;
  }
  const HelloPayload* hello =
      (const HelloPayload*)(packet.data + sizeof(PacketHeader));

  if (strncmp(hello->name, _deviceName, sizeof(_deviceName)) == 0) {
    return;
  }
  if (packet.port == 0) {
    if (memcmp(packet.src, _selfMac, sizeof(_selfMac)) == 0) {
      return;
    }
  } else {
    IPAddress local = WiFi.localIP();
    if (packet.src[0] == local[0] && packet.src[1] == local[1] &&
        packet.src[2] == local[2] && packet.src[3] == local[3]) {
      return;
    }
  }

  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  int slot = -1;
  bool existing = false;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used && _peers[i].port == packet.port &&
        memcmp(_peers[i].addr, packet.src, sizeof(_peers[i].addr)) == 0) {
      slot = (int)i;
      existing = true;
      break;
    }
  }
  if (slot < 0) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (!_peers[i].used) {
        slot = (int)i;
        break;
      }
    }
  }
  if (slot < 0) {
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (_peers[i].lastSeenMs < oldest) {
        oldest = _peers[i].lastSeenMs;
        slot = (int)i;
      }
    }
  }
  if (slot < 0) {
    xSemaphoreGive(_peerMutex);
    return;
  }

  Peer& peer = _peers[slot];
  if (!existing) {
    peer = Peer();
    peer.used = true;
  }
  memcpy(peer.addr, packet.src, sizeof(peer.addr));
  peer.port = packet.port;
  peer.viaWifi = packet.port != 0;
  peer.battery = hello->battery;
  peer.mode = hello->mode;
  peer.rssi = packet.rssi;
  peer.lastSeenMs = millis();
  peer.online = true;
  peer.receiveOnly = (header->flags & kHelloFlagReceiveOnly) != 0;
  strncpy(peer.name, hello->name, sizeof(peer.name) - 1);
  peer.name[sizeof(peer.name) - 1] = 0;

  xSemaphoreGive(_peerMutex);
}

void IntercomApp::expirePeers() {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  const uint32_t now = millis();
  bool removed = false;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used && (uint32_t)(now - _peers[i].lastSeenMs) > kPeerTimeoutMs) {
      _peers[i].used = false;
      removed = true;
    }
  }

  if (removed) {
    size_t write = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (_peers[i].used) {
        if (write != i) {
          _peers[write] = _peers[i];
        }
        ++write;
      }
    }
    for (size_t i = write; i < kMaxPeers; ++i) {
      _peers[i] = Peer();
    }
    _peerIndex = -1;
    if (write == 0) {
      _targetAll = true;
    }
  }

  xSemaphoreGive(_peerMutex);
}

bool IntercomApp::peerAt(int index, Peer& out) const {
  if (index < 0 || !_peerMutex ||
      xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }
  int count = 0;
  bool found = false;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (!_peers[i].used) {
      continue;
    }
    if (count == index) {
      out = _peers[i];
      found = true;
      break;
    }
    ++count;
  }
  xSemaphoreGive(_peerMutex);
  return found;
}

void IntercomApp::peerCycleNext() {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }
  int count = 0;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used) {
      ++count;
    }
  }
  if (count == 0) {
    _peerIndex = -1;
  } else {
    int next = _peerIndex + 1;
    _peerIndex = (next >= count) ? -1 : (int8_t)next;
  }
  xSemaphoreGive(_peerMutex);
}

void IntercomApp::peerToggleCurrent() {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }
  int count = 0;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used) {
      ++count;
    }
  }
  if (count == 0) {
    _peerIndex = -1;
    _targetAll = true;
    xSemaphoreGive(_peerMutex);
    return;
  }
  if (_peerIndex < 0) {
    _targetAll = true;
    for (size_t i = 0; i < kMaxPeers; ++i) {
      _peers[i].selected = false;
    }
  } else {
    int index = 0;
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (!_peers[i].used) {
        continue;
      }
      if (index == _peerIndex) {
        if (_targetAll) {
          _targetAll = false;
          for (size_t j = 0; j < kMaxPeers; ++j) {
            _peers[j].selected = false;
          }
          _peers[i].selected = true;
        } else {
          _peers[i].selected = !_peers[i].selected;
          bool any = false;
          for (size_t j = 0; j < kMaxPeers; ++j) {
            if (_peers[j].used && _peers[j].selected) {
              any = true;
              break;
            }
          }
          if (!any) {
            _targetAll = true;
          }
        }
        break;
      }
      ++index;
    }
  }
  xSemaphoreGive(_peerMutex);
}

size_t IntercomApp::collectTargets(Target* out, size_t max, bool& all) const {
  all = true;
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return 0;
  }
  if (_targetAll) {
    xSemaphoreGive(_peerMutex);
    return 0;
  }
  all = false;
  size_t count = 0;
  for (size_t i = 0; i < kMaxPeers && count < max; ++i) {
    if (!_peers[i].used || !_peers[i].selected || !_peers[i].online) {
      continue;
    }
    memcpy(out[count].addr, _peers[i].addr, sizeof(out[count].addr));
    out[count].port = _peers[i].port;
    out[count].viaWifi = _peers[i].viaWifi;
    ++count;
  }
  if (count == 0) {
    all = true;
  }
  xSemaphoreGive(_peerMutex);
  return count;
}

bool IntercomApp::sendToPeer(const uint8_t* addr, uint16_t port,
                             const uint8_t* data, size_t len) {
  if (!addr || !data || len == 0) {
    return false;
  }
  Transport* t = (port != 0) ? (Transport*)_transport.wifi()
                             : (Transport*)_transport.espNow();
  return t ? t->sendTo(addr, port, data, len) : false;
}

bool IntercomApp::broadcastAll(const uint8_t* data, size_t len) {
  // Send on every available transport so ESP-NOW-only receivers such as the
  // ATOM Lite speaker still hear ALL traffic when the manager prefers WiFi.
  bool ok = _transport.espNow()->broadcast(data, len);
  ok = _transport.wifi()->broadcast(data, len) || ok;
  return ok;
}

bool IntercomApp::sendToTargets(const uint8_t* data, size_t len) {
  Target targets[kMaxPeers];
  bool all = false;
  size_t count = collectTargets(targets, kMaxPeers, all);
  if (all) {
    return broadcastAll(data, len);
  }
  bool ok = false;
  for (size_t i = 0; i < count; ++i) {
    ok = sendToPeer(targets[i].addr, targets[i].port, data, len) || ok;
  }
  return ok;
}

void IntercomApp::fillPeerUi(UiState& s) const {
  s.peerOnline = false;
  s.peerName[0] = 0;
  s.peerRssi = 0;
  s.peerCount = 0;
  s.peerIndex = -1;
  s.targetAll = true;
  s.peerSelected = false;
  s.selectedCount = 0;

  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  int count = 0;
  int selected = 0;
  int first = -1;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (!_peers[i].used) {
      continue;
    }
    ++count;
    if (_targetAll || _peers[i].selected) {
      ++selected;
      if (first < 0) {
        first = (int)i;
      }
      if (_peers[i].online) {
        s.peerOnline = true;
      }
    }
  }

  s.peerCount = (uint8_t)count;
  s.peerIndex = _peerIndex;
  s.targetAll = _targetAll;
  s.selectedCount = (uint8_t)selected;

  if (_mode.load() == Mode::Peer) {
    if (_peerIndex < 0) {
      strncpy(s.peerName, "ALL", sizeof(s.peerName) - 1);
      s.peerSelected = _targetAll;
      if (count > 0) {
        for (size_t i = 0; i < kMaxPeers; ++i) {
          if (_peers[i].used) {
            s.peerOnline = _peers[i].online;
            s.peerRssi = _peers[i].rssi;
            break;
          }
        }
      }
    } else {
      int index = 0;
      for (size_t i = 0; i < kMaxPeers; ++i) {
        if (!_peers[i].used) {
          continue;
        }
        if (index == _peerIndex) {
          strncpy(s.peerName, _peers[i].name, sizeof(s.peerName) - 1);
          s.peerOnline = _peers[i].online;
          s.peerRssi = _peers[i].rssi;
          s.peerSelected = _targetAll || _peers[i].selected;
          break;
        }
        ++index;
      }
    }
  } else if (count == 0) {
    strncpy(s.peerName, "NO PEER", sizeof(s.peerName) - 1);
  } else if (_targetAll || selected == 0) {
    // No individual selection left: sends fall back to broadcast.
    strncpy(s.peerName, "ALL", sizeof(s.peerName) - 1);
  } else if (selected == 1 && first >= 0) {
    strncpy(s.peerName, _peers[first].name, sizeof(s.peerName) - 1);
    s.peerRssi = _peers[first].rssi;
  } else {
    snprintf(s.peerName, sizeof(s.peerName), "%d PEERS", selected);
  }

  s.peerName[sizeof(s.peerName) - 1] = 0;
  xSemaphoreGive(_peerMutex);
}

void IntercomApp::handleSpyPacket(const RxPacket& packet) {
  const PacketHeader* header = (const PacketHeader*)packet.data;
  if (header->payloadLen < sizeof(SpyPayload)) {
    return;
  }
  const uint8_t command =
      ((const SpyPayload*)(packet.data + sizeof(PacketHeader)))->command;

  const bool fromTarget =
      packet.port == _spyTargetPort &&
      memcmp(packet.src, _spyTargetAddr, sizeof(_spyTargetAddr)) == 0;

  if (command == kSpyClipDone || command == kSpyBusy) {
    if (!fromTarget) {
      return;
    }
    _spyRequestLive.store(false);
    _spyRequestClip.store(false);
    _spyAwaitClip.store(false);
    if (command == kSpyBusy) {
      setError();
    }
    return;
  }

  memcpy(_pendingSpyAddr, packet.src, sizeof(_pendingSpyAddr));
  _pendingSpyPort = packet.port;
  _pendingSpyCmd.store(command);
}

void IntercomApp::handlePendingSpy() {
  const uint8_t command = _pendingSpyCmd.exchange(0);
  if (command == 0) {
    return;
  }

  switch (command) {
    case kSpyLiveStart:
      if (_txActive.load() || _recording.load() || _sendingClip.load()) {
        // Busy: keep the current requester untouched.
        sendSpyCommand(kSpyBusy, _pendingSpyAddr, _pendingSpyPort);
        break;
      }
      memcpy(_spyRequesterAddr, _pendingSpyAddr, sizeof(_spyRequesterAddr));
      _spyRequesterPort = _pendingSpyPort;
      _spyRequesterValid = true;
      if (!startLiveTx(true)) {
        sendSpyCommand(kSpyBusy, _spyRequesterAddr, _spyRequesterPort);
        _spyRequesterValid = false;
      }
      break;
    case kSpyLiveStop:
      if (_spyServeLive.load()) {
        stopLiveTx();
      }
      break;
    case kSpyClipStart:
      if (_txActive.load() || _recording.load() || _sendingClip.load()) {
        // Busy: keep the current requester untouched.
        sendSpyCommand(kSpyBusy, _pendingSpyAddr, _pendingSpyPort);
        break;
      }
      memcpy(_spyRequesterAddr, _pendingSpyAddr, sizeof(_spyRequesterAddr));
      _spyRequesterPort = _pendingSpyPort;
      _spyRequesterValid = true;
      if (!startClipRecord(true)) {
        sendSpyCommand(kSpyBusy, _spyRequesterAddr, _spyRequesterPort);
        _spyRequesterValid = false;
      }
      break;
    case kSpyClipStop:
      if (_spyRecording.load()) {
        stopClipRecordAndSend();
      } else if (!_sendingClip.load() && _spyRequesterValid) {
        sendSpyCommand(kSpyClipDone, _spyRequesterAddr, _spyRequesterPort);
        _spyRequesterValid = false;
      }
      break;
    default:
      break;
  }
}

void IntercomApp::sendSpyCommand(uint8_t cmd, const uint8_t* addr,
                                 uint16_t port) {
  if (!addr) {
    return;
  }
  alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(SpyPayload)] = {};
  PacketHeader* header = (PacketHeader*)buf;
  SpyPayload* payload = (SpyPayload*)(buf + sizeof(PacketHeader));

  header->magic = kMagic;
  header->version = kVersion;
  header->type = kPacketSpy;
  header->flags = 0;
  header->seq = 0;
  header->predictor = 0;
  header->adpcmIndex = 0;
  header->payloadLen = sizeof(SpyPayload);
  payload->command = cmd;

  sendToPeer(addr, port, buf, sizeof(buf));
}

int IntercomApp::spyTargetIndex() const {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return -1;
  }
  int online = 0;
  int lastOnline = -1;
  int selected = 0;
  int selectedIndex = -1;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    // Receive-only units have no microphone, so they are never spy targets.
    if (!_peers[i].used || !_peers[i].online || _peers[i].receiveOnly) {
      continue;
    }
    ++online;
    lastOnline = (int)i;
    if (_peers[i].selected) {
      ++selected;
      selectedIndex = (int)i;
    }
  }
  const bool all = _targetAll;
  xSemaphoreGive(_peerMutex);

  if (all) {
    return online == 1 ? lastOnline : -1;
  }
  return selected == 1 ? selectedIndex : -1;
}

bool IntercomApp::acceptAudio(const uint8_t* src, uint16_t port) const {
  if (_spyRequestLive.load() || _spyRequestClip.load() ||
      _spyAwaitClip.load()) {
    return port == _spyTargetPort &&
           memcmp(src, _spyTargetAddr, sizeof(_spyTargetAddr)) == 0;
  }

  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return true;
  }
  bool allowed = _targetAll;
  if (!allowed) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (_peers[i].used && _peers[i].selected && _peers[i].online &&
          _peers[i].port == port &&
          memcmp(_peers[i].addr, src, sizeof(_peers[i].addr)) == 0) {
        allowed = true;
        break;
      }
    }
  }
  xSemaphoreGive(_peerMutex);
  return allowed;
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