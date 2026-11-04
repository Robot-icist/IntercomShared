#include "IntercomApp.h"

#include <Preferences.h>
#include <LittleFS.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace intercom {

namespace {

bool wrapWebPeerAudioPacket(const uint8_t* packet, size_t length,
                            const uint8_t identity[6], uint8_t* wrapped,
                            size_t& wrappedLength) {
  if (!packet || !identity || !wrapped ||
      length < sizeof(PacketHeader) ||
      length + sizeof(WebPeerAudioTarget) > kMaxPacketSize) {
    return false;
  }
  const PacketHeader* source =
      reinterpret_cast<const PacketHeader*>(packet);
  if (source->payloadLen + sizeof(WebPeerAudioTarget) > UINT8_MAX ||
      length < sizeof(PacketHeader) + source->payloadLen) {
    return false;
  }

  memcpy(wrapped, packet, sizeof(PacketHeader));
  PacketHeader* header = reinterpret_cast<PacketHeader*>(wrapped);
  header->flags &= ~kAudioFlagWebPeer;
  header->flags |= kAudioFlagWebPeerTarget;
  header->payloadLen += sizeof(WebPeerAudioTarget);
  memcpy(wrapped + sizeof(PacketHeader), identity,
         sizeof(WebPeerAudioTarget));
  memcpy(wrapped + sizeof(PacketHeader) + sizeof(WebPeerAudioTarget),
         packet + sizeof(PacketHeader), length - sizeof(PacketHeader));
  wrappedLength = length + sizeof(WebPeerAudioTarget);
  return true;
}

}  // namespace

bool IntercomApp::begin() {
  Serial.begin(115200);
  delay(50);

  // Mount LittleFS filesystem for web server
  if (!LittleFS.begin()) {
    Serial.println("[app] LittleFS mount failed");
  } else {
    Serial.println("[app] LittleFS mounted successfully");
    // List contents for debugging
    File root = LittleFS.open("/");
    if (root) {
      Serial.println("[app] LittleFS contents:");
      File file = root.openNextFile();
      while (file) {
        Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
      }
      root.close();
    }
  }

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
      memcpy(_spyTargetIdentity, peer.identityMac,
             sizeof(_spyTargetIdentity));
      _spyTargetPort = peer.port;
      _spyTargetWebPeer = peer.webPeer;
      _jitter.clear();
      sendSpyCommand(kSpyLiveStart, peer.addr, peer.port, peer.webPeer,
                     peer.identityMac);
      _spyRequestLive.store(true);
      _spyRequestUntilMs = millis() + kSpyLiveMaxMs;
    }
  }

  if (M5.BtnA.wasReleased()) {
    if (_spyRequestLive.load()) {
      sendSpyCommand(kSpyLiveStop, _spyTargetAddr, _spyTargetPort,
                     _spyTargetWebPeer, _spyTargetIdentity);
      _spyRequestLive.store(false);
    }
  }

  if (M5.BtnA.wasSingleClicked()) {
    if (_spyRequestClip.load()) {
      sendSpyCommand(kSpyClipStop, _spyTargetAddr, _spyTargetPort,
                     _spyTargetWebPeer, _spyTargetIdentity);
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
      memcpy(_spyTargetIdentity, peer.identityMac,
             sizeof(_spyTargetIdentity));
      _spyTargetPort = peer.port;
      _spyTargetWebPeer = peer.webPeer;
      _jitter.clear();
      sendSpyCommand(kSpyClipStart, peer.addr, peer.port, peer.webPeer,
                     peer.identityMac);
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
    _clipTargets[0].webPeer = false;
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
      if (_clipTargets[i].webPeer) {
        uint8_t targetPacket[kMaxPacketSize];
        if (isWebClientPort(_clipTargets[i].port)) {
          memcpy(targetPacket, _txPacket, len);
          ((PacketHeader*)targetPacket)->flags |= kAudioFlagWebPeer;
          sendToPeer(_clipTargets[i].addr, _clipTargets[i].port, targetPacket,
                     len);
        } else {
          size_t targetLength = 0;
          if (wrapWebPeerAudioPacket(_txPacket, len,
                                     _clipTargets[i].identity, targetPacket,
                                     targetLength)) {
            sendToPeer(_clipTargets[i].addr, _clipTargets[i].port,
                       targetPacket, targetLength);
          } else {
            Serial.println("[web] Could not encode targeted web peer audio");
          }
        }
      } else {
        sendToPeer(_clipTargets[i].addr, _clipTargets[i].port, _txPacket, len);
      }
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
  const IPAddress localIp = WiFi.localIP();
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (WiFi.status() == WL_CONNECTED &&
      localIp != IPAddress(0, 0, 0, 0) && wifiMode != WIFI_AP &&
      wifiMode != WIFI_AP_STA) {
    snprintf(s.selfAddress, sizeof(s.selfAddress), "%u.%u.%u.%u",
             localIp[0], localIp[1], localIp[2], localIp[3]);
    snprintf(s.selfMdns, sizeof(s.selfMdns), "%s-web.local", _deviceName);
  }
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
  // Send on all transports so web clients and WiFi peers both see us
  Transport* transports[] = {
    (Transport*)_transport.espNow(),
    (Transport*)_transport.wifi(),
    (Transport*)_transport.web()
  };
  for (int i = 0; i < 3; ++i) {
    if (transports[i]) {
      transports[i]->sendHello(_deviceName, value, _mode.load());
    }
  }
  sendWebPeerAnnouncements();
  sendWebPeerList();
}

void IntercomApp::sendWebPeerAnnouncements() {
  Peer webPeers[kMaxPeers];
  size_t count = 0;
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used && _peers[i].online &&
        isWebClientPort(_peers[i].port)) {
      webPeers[count++] = _peers[i];
    }
  }
  xSemaphoreGive(_peerMutex);

  for (size_t i = 0; i < count; ++i) {
    alignas(4) uint8_t packet[sizeof(PacketHeader) + sizeof(HelloPayload) +
                              sizeof(HelloDeviceIdentity)] = {};
    PacketHeader* header = (PacketHeader*)packet;
    HelloPayload* hello = (HelloPayload*)(packet + sizeof(PacketHeader));
    HelloDeviceIdentity* identity =
        (HelloDeviceIdentity*)(packet + sizeof(PacketHeader) +
                               sizeof(HelloPayload));
    header->magic = kMagic;
    header->version = kVersion;
    header->type = kPacketHello;
    header->flags = kHelloFlagWebPeer | kHelloFlagHasDeviceMac;
    header->payloadLen = sizeof(HelloPayload) + sizeof(HelloDeviceIdentity);
    hello->battery = webPeers[i].battery;
    hello->mode = webPeers[i].mode;
    strncpy(hello->name, webPeers[i].name, sizeof(hello->name) - 1);
    if (webPeers[i].hasIdentityMac) {
      memcpy(identity->mac, webPeers[i].identityMac, sizeof(identity->mac));
    } else {
      makeWebPeerIdentity(webPeers[i].name, identity->mac);
    }
    _transport.espNow()->broadcast(packet, sizeof(packet));
    _transport.wifi()->broadcast(packet, sizeof(packet));
  }
}

void IntercomApp::sendWebPeerList() {
  char message[2000] = "{\"type\":\"peer_list\",\"peers\":[";
  size_t used = strlen(message);
  bool first = true;
  auto appendPeer = [&](const uint8_t* addr, const uint8_t* identityMac,
                        bool hasIdentityMac, uint16_t port, const char* name,
                        int8_t rssi, uint8_t battery, uint8_t mode,
                        bool viaWifi, bool webPeer) {
    char id[24];
    char identityText[18] = {};
    if (hasIdentityMac) {
      snprintf(identityText, sizeof(identityText),
               "%02X:%02X:%02X:%02X:%02X:%02X",
               identityMac[0], identityMac[1], identityMac[2],
               identityMac[3], identityMac[4], identityMac[5]);
    }
    if (webPeer && hasIdentityMac) {
      snprintf(id, sizeof(id), "web:%02X%02X%02X%02X%02X%02X",
               identityMac[0], identityMac[1], identityMac[2],
               identityMac[3], identityMac[4], identityMac[5]);
    } else if (webPeer) {
      uint32_t hash = 2166136261u;
      for (const unsigned char* p =
               reinterpret_cast<const unsigned char*>(name);
           *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
      }
      snprintf(id, sizeof(id), "web:%08lX", (unsigned long)hash);
    } else if (hasIdentityMac) {
      snprintf(id, sizeof(id), "esp:%02X%02X%02X%02X%02X%02X",
               identityMac[0], identityMac[1], identityMac[2],
               identityMac[3], identityMac[4], identityMac[5]);
    } else {
      uint32_t hash = 2166136261u;
      for (const unsigned char* p =
               reinterpret_cast<const unsigned char*>(name);
           *p; ++p) {
        hash = (hash ^ *p) * 16777619u;
      }
      snprintf(id, sizeof(id), "peer:%08lX", (unsigned long)hash);
    }
    int written = snprintf(message + used, sizeof(message) - used,
                           "%s{\"id\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                           "\"identityMac\":\"%s\",\"port\":%u,\"viaWifi\":%s,"
                           "\"webPeer\":%s,\"name\":\"",
                           first ? "" : ",", id,
                           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                           identityText, port, viaWifi ? "true" : "false",
                           webPeer ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(message) - used) return false;
    used += (size_t)written;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(name);
         *p; ++p) {
      if (*p == '"' || *p == '\\') {
        if (used + 2 >= sizeof(message)) return false;
        message[used++] = '\\';
        message[used++] = (char)*p;
      } else if (*p >= 0x20) {
        if (used + 1 >= sizeof(message)) return false;
        message[used++] = (char)*p;
      }
    }
    written = snprintf(message + used, sizeof(message) - used,
                       "\",\"rssi\":%d,\"battery\":%u,\"mode\":%u}",
                       rssi, battery == 0xFF ? 255 : battery, mode);
    if (written < 0 || (size_t)written >= sizeof(message) - used) return false;
    used += (size_t)written;
    first = false;
    return true;
  };

  const int battery = _battery.load();
  if (!appendPeer(_selfMac, _selfMac, true, 0, _deviceName, 0,
                  battery < 0 ? 0xFF : (uint8_t)battery,
                  (uint8_t)_mode.load(), false, false)) {
    return;
  }

  Peer peers[kMaxPeers];
  size_t count = 0;
  if (_peerMutex && xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (size_t i = 0; i < kMaxPeers; ++i) {
      if (_peers[i].used && _peers[i].online &&
          memcmp(_peers[i].addr, _selfMac, sizeof(_selfMac)) != 0) {
        peers[count++] = _peers[i];
      }
    }
    xSemaphoreGive(_peerMutex);
  }
  for (size_t i = 0; i < count; ++i) {
    if (!appendPeer(peers[i].addr,
                    peers[i].identityMac,
                    peers[i].hasIdentityMac,
                    peers[i].port, peers[i].name,
                    peers[i].rssi, peers[i].battery, peers[i].mode,
                    peers[i].viaWifi, peers[i].webPeer)) {
      return;
    }
  }
  if (used + 3 >= sizeof(message)) return;
  memcpy(message + used, "]}", 3);
  _transport.web()->broadcastToWeb(
      reinterpret_cast<const uint8_t*>(message), used + 2);
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
  size_t nextTransport = 0;
  while (true) {
    bool gotPacket = false;
    Transport* transports[] = {
      (Transport*)_transport.espNow(),
      (Transport*)_transport.wifi(),
      (Transport*)_transport.web()
    };
    for (size_t i = 0; i < 3 && !gotPacket; ++i) {
      const size_t index = (nextTransport + i) % 3;
      if (transports[index] && transports[index]->popPacket(packet, 0)) {
        gotPacket = true;
        nextTransport = (index + 1) % 3;
      }
    }
    if (!gotPacket) {
      vTaskDelay(pdMS_TO_TICKS(5));
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
    if (routeSpyRelayPacket(packet)) {
      continue;
    }
    if (isLocalNetworkSource(packet)) {
      continue;
    }
    if (header->flags & kAudioFlagWebPeerTarget) {
      if (header->payloadLen <= sizeof(WebPeerAudioTarget) ||
          packet.len < sizeof(PacketHeader) + header->payloadLen) {
        continue;
      }
      WebPeerAudioTarget target = {};
      memcpy(target.identity, packet.data + sizeof(PacketHeader),
             sizeof(target.identity));
      uint8_t targetPacket[kMaxPacketSize] = {};
      memcpy(targetPacket, packet.data, sizeof(PacketHeader));
      PacketHeader* targetHeader =
          reinterpret_cast<PacketHeader*>(targetPacket);
      targetHeader->flags &= ~kAudioFlagWebPeerTarget;
      targetHeader->payloadLen -= sizeof(WebPeerAudioTarget);
      const size_t payloadLength = targetHeader->payloadLen;
      memcpy(targetPacket + sizeof(PacketHeader),
             packet.data + sizeof(PacketHeader) + sizeof(WebPeerAudioTarget),
             payloadLength);
      if (!_transport.web()->sendToWebPeer(
              target.identity, targetPacket,
              sizeof(PacketHeader) + payloadLength)) {
        Serial.println("[web] Targeted web peer is not connected here");
      }
      continue;
    }

    if (header->flags & kAudioFlagWebPeer) {
      if (isDuplicateAudioPacket(packet)) {
        continue;
      }
      if (!_transport.web()->broadcast(packet.data, packet.len)) {
        Serial.println("[web] Could not forward audio to the web peer");
      }
      continue;
    }
    const bool webSpyAudio =
        packet.port == _spyTargetPort && isWebClientPort(packet.port) &&
        (_spyRequestLive.load() || _spyRequestClip.load() ||
         _spyAwaitClip.load()) &&
        memcmp(packet.src, _spyTargetAddr, sizeof(_spyTargetAddr)) == 0;
    if (!webSpyAudio && !isWebClientPort(packet.port) &&
        !acceptAudio(packet.src, packet.port)) {
      continue;
    }
    if (isDuplicateAudioPacket(packet)) {
      continue;
    }

    if (isWebClientPort(packet.port) && !webSpyAudio) {
      uint8_t targetAddr[6] = {};
      uint8_t targetIdentity[6] = {};
      uint16_t targetPort = 0;
      bool targetViaWifi = false;
      bool targetWebPeer = false;
      bool targetAll = true;
      if (!_transport.web()->audioTarget(packet.port, targetAddr, targetPort,
                                         targetViaWifi, targetWebPeer,
                                         targetIdentity, targetAll)) {
        continue;
      }

      bool routed = false;
      bool playLocally = false;
      if (targetAll) {
        routed = _transport.espNow()->broadcast(packet.data, packet.len);
        routed = _transport.wifi()->broadcast(packet.data, packet.len) ||
                 routed;
        broadcastToWebPeers(packet.data, packet.len);
        _transport.web()->broadcastToWebExcept(packet.port, packet.data,
                                               packet.len);
        playLocally = true;
      } else if (memcmp(targetAddr, _selfMac, sizeof(targetAddr)) == 0) {
        routed = true;
        playLocally = true;
      } else {
        uint8_t targetPacket[kMaxPacketSize];
        size_t targetLength = packet.len;
        if (targetWebPeer && !isWebClientPort(targetPort)) {
          if (!wrapWebPeerAudioPacket(packet.data, packet.len, targetIdentity,
                                      targetPacket, targetLength)) {
            Serial.println("[web] Could not encode targeted web peer audio");
            continue;
          }
        } else {
          memcpy(targetPacket, packet.data, packet.len);
          if (targetWebPeer)
            ((PacketHeader*)targetPacket)->flags |= kAudioFlagWebPeer;
        }
        if (targetWebPeer && targetViaWifi) {
          routed = _transport.wifi()->sendTo(targetAddr, targetPort,
                                             targetPacket, targetLength);
        } else {
          routed = sendToPeer(targetAddr, targetPort, targetPacket,
                              targetLength);
        }
      }
      if (!routed) {
        Serial.println("[web] Audio target is unavailable");
      }
      if (!playLocally) {
        continue;
      }
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
  const bool webPeer = isWebClientPort(packet.port) ||
                       (header->flags & kHelloFlagWebPeer) != 0;
  const bool hasIdentityMac =
      (header->flags & kHelloFlagHasDeviceMac) != 0 &&
      header->payloadLen >=
          sizeof(HelloPayload) + sizeof(HelloDeviceIdentity);
  const HelloDeviceIdentity* identity =
      hasIdentityMac
          ? (const HelloDeviceIdentity*)(packet.data + sizeof(PacketHeader) +
                                         sizeof(HelloPayload))
          : nullptr;

  if (strncmp(hello->name, _deviceName, sizeof(_deviceName)) == 0) {
    return;
  }
  if (packet.port == 0) {
    if (memcmp(packet.src, _selfMac, sizeof(_selfMac)) == 0) {
      return;
    }
  } else if (!isWebClientPort(packet.port)) {
    IPAddress local = WiFi.localIP();
    if (local != IPAddress(0, 0, 0, 0) &&
        packet.src[0] == local[0] && packet.src[1] == local[1] &&
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
    if (!_peers[i].used || _peers[i].webPeer != webPeer) {
      continue;
    }
    const bool sameName =
        strncmp(_peers[i].name, hello->name, sizeof(_peers[i].name)) == 0;
    const bool sameIdentity =
        hasIdentityMac && _peers[i].hasIdentityMac &&
        memcmp(_peers[i].identityMac, identity->mac,
               sizeof(_peers[i].identityMac)) == 0;
    const bool samePeer = sameName || sameIdentity;
    if (samePeer) {
      if (slot < 0) {
        slot = (int)i;
        existing = true;
      } else {
        Peer& primary = _peers[slot];
        const Peer& duplicate = _peers[i];
        primary.selected = primary.selected || duplicate.selected;
        if (!primary.hasIdentityMac && duplicate.hasIdentityMac) {
          memcpy(primary.identityMac, duplicate.identityMac,
                 sizeof(primary.identityMac));
          primary.hasIdentityMac = true;
        }
        if (!primary.hasEspNowRoute && duplicate.hasEspNowRoute) {
          primary.hasEspNowRoute = true;
          primary.lastEspNowMs = duplicate.lastEspNowMs;
          if (primary.port != 0) {
            memcpy(primary.addr, duplicate.addr, sizeof(primary.addr));
            primary.port = 0;
            primary.viaWifi = false;
          }
        }
        if (!primary.hasWifiRoute && duplicate.hasWifiRoute) {
          primary.hasWifiRoute = true;
          primary.lastWifiMs = duplicate.lastWifiMs;
          memcpy(primary.wifiAddr, duplicate.wifiAddr,
                 sizeof(primary.wifiAddr));
          primary.wifiPort = duplicate.wifiPort;
        }
        _peers[i] = Peer();
      }
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
  const uint32_t now = millis();
  if (packet.port == 0) {
    memcpy(peer.addr, packet.src, sizeof(peer.addr));
    peer.port = packet.port;
    peer.viaWifi = false;
    peer.hasEspNowRoute = true;
    peer.lastEspNowMs = now;
  } else if (!isWebClientPort(packet.port)) {
    memcpy(peer.wifiAddr, packet.src, sizeof(peer.wifiAddr));
    peer.wifiPort = packet.port;
    peer.hasWifiRoute = true;
    peer.lastWifiMs = now;
    const bool espNowRecent =
        peer.hasEspNowRoute && now - peer.lastEspNowMs <= kPeerTimeoutMs;
    if (!espNowRecent) {
      memcpy(peer.addr, peer.wifiAddr, sizeof(peer.addr));
      peer.port = peer.wifiPort;
      peer.viaWifi = true;
    }
  } else if (!existing) {
    memcpy(peer.addr, packet.src, sizeof(peer.addr));
    peer.port = packet.port;
    peer.viaWifi = false;
  }
  if (identity) {
    memcpy(peer.identityMac, identity->mac, sizeof(peer.identityMac));
    peer.hasIdentityMac = true;
  }
  peer.webPeer = webPeer;
  peer.battery = hello->battery;
  peer.mode = hello->mode;
  peer.rssi = packet.rssi;
  peer.lastSeenMs = now;
  peer.online = true;
  peer.receiveOnly = (header->flags & kHelloFlagReceiveOnly) != 0;
  strncpy(peer.name, hello->name, sizeof(peer.name) - 1);
  peer.name[sizeof(peer.name) - 1] = 0;
  char registeredName[sizeof(peer.name)];
  strncpy(registeredName, peer.name, sizeof(registeredName) - 1);
  registeredName[sizeof(registeredName) - 1] = 0;

  xSemaphoreGive(_peerMutex);

  if (!existing) {
    Serial.printf("[peer] Registered %s via %s (port %u)\n", registeredName,
                  isWebClientPort(packet.port) ? "web" :
                  packet.port != 0 ? "WiFi" : "ESP-NOW",
                  packet.port);
  }
  sendWebPeerList();
}

void IntercomApp::expirePeers() {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }

  const uint32_t now = millis();
  bool removed = false;
  bool routeChanged = false;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    Peer& peer = _peers[i];
    if (peer.used && !peer.webPeer) {
      if (peer.hasEspNowRoute &&
          now - peer.lastEspNowMs > kPeerTimeoutMs) {
        peer.hasEspNowRoute = false;
      }
      if (peer.hasWifiRoute && now - peer.lastWifiMs > kPeerTimeoutMs) {
        peer.hasWifiRoute = false;
      }
      if (!peer.hasEspNowRoute && peer.hasWifiRoute &&
          (!peer.viaWifi || peer.port != peer.wifiPort ||
           memcmp(peer.addr, peer.wifiAddr, sizeof(peer.addr)) != 0)) {
        memcpy(peer.addr, peer.wifiAddr, sizeof(peer.addr));
        peer.port = peer.wifiPort;
        peer.viaWifi = true;
        routeChanged = true;
      }
    }
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
  if (routeChanged) {
    sendWebPeerList();
  }
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
    memcpy(out[count].identity, _peers[i].identityMac,
           sizeof(out[count].identity));
    out[count].port = _peers[i].port;
    out[count].viaWifi = _peers[i].viaWifi;
    out[count].webPeer = _peers[i].webPeer;
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
  Transport* t = nullptr;
  if (isWebClientPort(port)) {
    t = (Transport*)_transport.web();
  } else if (port != 0) {
    t = (Transport*)_transport.wifi();
  } else {
    t = (Transport*)_transport.espNow();
  }
  return t ? t->sendTo(addr, port, data, len) : false;
}

bool IntercomApp::isLocalNetworkSource(const RxPacket& packet) const {
  if (packet.port == 0) {
    return memcmp(packet.src, _selfMac, sizeof(_selfMac)) == 0;
  }
  if (isWebClientPort(packet.port)) {
    return false;
  }

  const IPAddress local = WiFi.localIP();
  return local != IPAddress(0, 0, 0, 0) &&
         packet.src[0] == local[0] && packet.src[1] == local[1] &&
         packet.src[2] == local[2] && packet.src[3] == local[3];
}

bool IntercomApp::broadcastAll(const uint8_t* data, size_t len) {
  // Send on every available transport so ESP-NOW-only receivers such as the
  // ATOM Lite speaker still hear ALL traffic when the manager prefers WiFi.
  bool ok = _transport.espNow()->broadcast(data, len);
  ok = _transport.wifi()->broadcast(data, len) || ok;
  ok = _transport.web()->broadcast(data, len) || ok;
  broadcastToWebPeers(data, len);
  return ok;
}

bool IntercomApp::broadcastToWebPeers(const uint8_t* data, size_t len) {
  if (!data || len < sizeof(PacketHeader) || len > kMaxPacketSize) {
    return false;
  }
  bool hasWebPeer = false;
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (_peers[i].used && _peers[i].online && _peers[i].webPeer) {
      hasWebPeer = true;
      break;
    }
  }
  xSemaphoreGive(_peerMutex);
  if (!hasWebPeer) return false;

  uint8_t packet[kMaxPacketSize];
  memcpy(packet, data, len);
  ((PacketHeader*)packet)->flags |= kAudioFlagWebPeer;
  bool ok = _transport.espNow()->broadcast(packet, len);
  ok = _transport.wifi()->broadcast(packet, len) || ok;
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
    if (targets[i].webPeer && len >= sizeof(PacketHeader)) {
      uint8_t targetPacket[kMaxPacketSize];
      size_t targetLength = len;
      if (isWebClientPort(targets[i].port)) {
        memcpy(targetPacket, data, len);
        ((PacketHeader*)targetPacket)->flags |= kAudioFlagWebPeer;
      } else if (!wrapWebPeerAudioPacket(data, len, targets[i].identity,
                                         targetPacket, targetLength)) {
        Serial.println("[web] Could not encode targeted web peer audio");
        continue;
      }
      ok = sendToPeer(targets[i].addr, targets[i].port, targetPacket,
                      targetLength) || ok;
    } else {
      ok = sendToPeer(targets[i].addr, targets[i].port, data, len) || ok;
    }
  }
  return ok;
}

bool IntercomApp::isDuplicateAudioPacket(const RxPacket& packet) {
  constexpr uint32_t kDuplicateWindowMs = 1000;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < packet.len; ++i) {
    hash = (hash ^ packet.data[i]) * 16777619u;
  }

  const uint32_t now = millis();
  for (const RecentAudioPacket& recent : _recentAudioPackets) {
    if (recent.len == packet.len && recent.hash == hash &&
        now - recent.receivedAtMs <= kDuplicateWindowMs) {
      return true;
    }
  }

  RecentAudioPacket& recent =
      _recentAudioPackets[_recentAudioPacketIndex++ %
                          (sizeof(_recentAudioPackets) /
                           sizeof(_recentAudioPackets[0]))];
  recent.hash = hash;
  recent.len = packet.len;
  recent.receivedAtMs = now;
  return false;
}

void IntercomApp::fillPeerUi(UiState& s) const {
  s.peerOnline = false;
  s.peerName[0] = 0;
  s.peerAddress[0] = 0;
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
          const Peer& peer = _peers[i];
          strncpy(s.peerName, peer.name, sizeof(s.peerName) - 1);
          s.peerOnline = peer.online;
          s.peerRssi = peer.rssi;
          s.peerSelected = _targetAll || peer.selected;
          if (!peer.webPeer && peer.hasWifiRoute) {
            snprintf(s.peerAddress, sizeof(s.peerAddress), "%u.%u.%u.%u",
                     peer.wifiAddr[0], peer.wifiAddr[1], peer.wifiAddr[2],
                     peer.wifiAddr[3]);
          }
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
    const Peer& peer = _peers[first];
    strncpy(s.peerName, peer.name, sizeof(s.peerName) - 1);
    s.peerRssi = peer.rssi;
    if (!peer.webPeer && peer.hasWifiRoute) {
      snprintf(s.peerAddress, sizeof(s.peerAddress), "%u.%u.%u.%u",
               peer.wifiAddr[0], peer.wifiAddr[1], peer.wifiAddr[2],
               peer.wifiAddr[3]);
    }
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
  const SpyPayload* payload =
      (const SpyPayload*)(packet.data + sizeof(PacketHeader));
  const uint8_t command = payload->command;

  if (isWebClientPort(packet.port) &&
      header->payloadLen >= sizeof(SpyTargetPayload)) {
    const SpyTargetPayload* target =
        (const SpyTargetPayload*)(packet.data + sizeof(PacketHeader));
    const bool localTarget =
        (target->port == 0 &&
         memcmp(target->addr, _selfMac, sizeof(_selfMac)) == 0) ||
        (target->port != 0 && !isWebClientPort(target->port) &&
         target->addr[0] == WiFi.localIP()[0] &&
         target->addr[1] == WiFi.localIP()[1] &&
         target->addr[2] == WiFi.localIP()[2] &&
         target->addr[3] == WiFi.localIP()[3]);
    _pendingSpyFromWeb = !localTarget;
    memcpy(_pendingSpyTargetAddr, target->addr,
           sizeof(_pendingSpyTargetAddr));
    memcpy(_pendingSpyTargetIdentity, target->identity,
           sizeof(_pendingSpyTargetIdentity));
    _pendingSpyTargetPort = target->port;
    _pendingSpyTargetFlags = target->flags;
    memcpy(_pendingSpyAddr, packet.src, sizeof(_pendingSpyAddr));
    _pendingSpyPort = packet.port;
    _pendingSpyCmd.store(command);
    return;
  }

  if (_spyRelayActive.load()) {
    bool relayFromTarget = false;
    if (_spyRelayFromWeb) {
      relayFromTarget =
          packet.port == _spyRelayTargetPort &&
          memcmp(packet.src, _spyRelayTargetAddr,
                 sizeof(_spyRelayTargetAddr)) == 0;
    } else if (isWebClientPort(packet.port)) {
      uint8_t identity[6] = {};
      relayFromTarget =
          _transport.web()->webClientIdentity(packet.port, identity) &&
          memcmp(identity, _spyRelayTargetIdentity, sizeof(identity)) == 0;
    }
    if (relayFromTarget) {
      const bool forwarded =
          _spyRelayFromWeb
              ? _transport.web()->sendTo(_spyRelayRequesterAddr,
                                         _spyRelayRequesterPort,
                                         packet.data, packet.len)
              : sendToPeer(_spyRelayRequesterAddr, _spyRelayRequesterPort,
                           packet.data, packet.len);
      if (!forwarded) {
        Serial.println("[spy] Could not relay spy response");
      }
      if (command == kSpyClipDone || command == kSpyBusy) {
        _spyRelayActive.store(false);
      }
      return;
    }
  }

  if ((header->flags & kSpyFlagWebPeerTarget) != 0 &&
      !isWebClientPort(packet.port)) {
    uint8_t targetIdentity[6] = {};
    const bool hasTargetIdentity =
        header->payloadLen >= sizeof(SpyTargetPayload);
    if (hasTargetIdentity) {
      const SpyTargetPayload* target =
          reinterpret_cast<const SpyTargetPayload*>(
              packet.data + sizeof(PacketHeader));
      memcpy(targetIdentity, target->identity, sizeof(targetIdentity));
    }
    if (command == kSpyLiveStart || command == kSpyClipStart) {
      memcpy(_spyRelayRequesterAddr, packet.src,
             sizeof(_spyRelayRequesterAddr));
      _spyRelayRequesterPort = packet.port;
      _spyRelayFromWeb = false;
      memcpy(_spyRelayTargetIdentity, targetIdentity,
             sizeof(_spyRelayTargetIdentity));
      _spyRelayActive.store(true);
    }
    if (hasTargetIdentity) {
      if (!_transport.web()->sendToWebPeer(targetIdentity, packet.data,
                                           packet.len)) {
        Serial.println("[spy] Target web peer is not connected here");
      }
    } else {
      _transport.web()->sendTo(packet.src, kWebClientPort,
                               packet.data, packet.len);
    }
    if (command == kSpyLiveStop) {
      _spyRelayActive.store(false);
    }
    return;
  }

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

  _pendingSpyFromWeb = false;
  memcpy(_pendingSpyAddr, packet.src, sizeof(_pendingSpyAddr));
  _pendingSpyPort = packet.port;
  _pendingSpyCmd.store(command);
}

void IntercomApp::handlePendingSpy() {
  const uint8_t command = _pendingSpyCmd.exchange(0);
  if (command == 0) {
    return;
  }

  if (_pendingSpyFromWeb) {
    _pendingSpyFromWeb = false;
    if (command == kSpyLiveStart || command == kSpyClipStart) {
      memcpy(_spyRelayRequesterAddr, _pendingSpyAddr,
             sizeof(_spyRelayRequesterAddr));
      _spyRelayRequesterPort = _pendingSpyPort;
      memcpy(_spyRelayTargetAddr, _pendingSpyTargetAddr,
             sizeof(_spyRelayTargetAddr));
      _spyRelayTargetPort = _pendingSpyTargetPort;
      memcpy(_spyRelayTargetIdentity, _pendingSpyTargetIdentity,
             sizeof(_spyRelayTargetIdentity));
      _spyRelayFromWeb = true;
      _spyRelayActive.store(true);
      memcpy(_spyTargetAddr, _pendingSpyTargetAddr,
             sizeof(_spyTargetAddr));
      memcpy(_spyTargetIdentity, _pendingSpyTargetIdentity,
             sizeof(_spyTargetIdentity));
      _spyTargetPort = _pendingSpyTargetPort;
    }
    const bool targetWebPeer =
        (_pendingSpyTargetFlags & kSpyTargetWebPeer) != 0;
    sendSpyCommand(command, _pendingSpyTargetAddr,
                   _pendingSpyTargetPort, targetWebPeer,
                   _pendingSpyTargetIdentity);
    if (command == kSpyLiveStop) {
      _spyRelayActive.store(false);
    }
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
                                 uint16_t port, bool webPeer,
                                 const uint8_t* webPeerIdentity) {
  if (!addr) {
    return;
  }
  const size_t payloadSize =
      webPeer ? sizeof(SpyTargetPayload) : sizeof(SpyPayload);
  alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(SpyTargetPayload)] = {};
  PacketHeader* header = (PacketHeader*)buf;

  header->magic = kMagic;
  header->version = kVersion;
  header->type = kPacketSpy;
  header->flags = webPeer ? kSpyFlagWebPeerTarget : 0;
  header->seq = 0;
  header->predictor = 0;
  header->adpcmIndex = 0;
  header->payloadLen = payloadSize;
  if (webPeer) {
    if (!webPeerIdentity) {
      Serial.println("[spy] Missing selected web peer identity");
      return;
    }
    SpyTargetPayload* target =
        reinterpret_cast<SpyTargetPayload*>(buf + sizeof(PacketHeader));
    target->command = cmd;
    memcpy(target->addr, addr, sizeof(target->addr));
    target->port = port;
    target->flags = kSpyTargetWebPeer;
    memcpy(target->identity, webPeerIdentity, sizeof(target->identity));
  } else {
    SpyPayload* payload = reinterpret_cast<SpyPayload*>(
        buf + sizeof(PacketHeader));
    payload->command = cmd;
  }

  sendToPeer(addr, port, buf, sizeof(PacketHeader) + payloadSize);
}

bool IntercomApp::routeSpyRelayPacket(const RxPacket& packet) {
  if (!_spyRelayActive.load()) {
    return false;
  }

  bool fromTarget = false;
  bool sent = false;
  if (_spyRelayFromWeb) {
    fromTarget =
        packet.port == _spyRelayTargetPort &&
        memcmp(packet.src, _spyRelayTargetAddr,
               sizeof(_spyRelayTargetAddr)) == 0;
    if (fromTarget) {
      sent = _transport.web()->sendTo(
          _spyRelayRequesterAddr, _spyRelayRequesterPort,
          packet.data, packet.len);
    }
  } else if (isWebClientPort(packet.port)) {
    uint8_t identity[6] = {};
    fromTarget =
        _transport.web()->webClientIdentity(packet.port, identity) &&
        memcmp(identity, _spyRelayTargetIdentity, sizeof(identity)) == 0;
    if (fromTarget) {
      sent = sendToPeer(_spyRelayRequesterAddr, _spyRelayRequesterPort,
                        packet.data, packet.len);
    }
  }
  if (fromTarget && !sent) {
    Serial.println("[spy] Could not relay spy audio");
  }
  return fromTarget;
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