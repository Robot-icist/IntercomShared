#include "Transport.h"

#include <esp_now.h>
#include <esp_wifi.h>

namespace intercom
{
  // Constants for ESP-NOW transport (not in IntercomConfig.h)
  constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  constexpr uint32_t kPeerAddRetryMs = 500;

  // ============================================================================
  // EspNowTransport Implementation
  // ============================================================================

  EspNowTransport *EspNowTransport::g_instance = nullptr;

  bool EspNowTransport::begin()
  {
    _queue = xQueueCreate(16, sizeof(RxPacket));
    _peerMutex = xSemaphoreCreateMutex();
    if (!_queue || !_peerMutex)
    {
      return false;
    }

    _enabled = true;

    // ESP-NOW requires WiFi to be in STA mode. Force STA mode here.
    // The WiFi transport will re-configure WiFi mode as needed (e.g., AP for config portal).
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK)
    {
      Serial.println("[espnow] init failed");
      return false;
    }

    // Ensure channel is set after init
    esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

    g_instance = this;
    esp_now_register_recv_cb(&EspNowTransport::onRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcast, sizeof(kBroadcast));
    peer.channel = kChannel;  // Use fixed channel, not 0 (which follows WiFi)
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
    {
      Serial.println("[espnow] broadcast peer add failed");
      return false;
    }

    Serial.printf("[espnow] channel %u (fixed), MAC %s\n", (unsigned)kChannel,
                  WiFi.macAddress().c_str());
    return true;
  }

  void EspNowTransport::end()
  {
    esp_now_deinit();
    if (g_instance == this)
    {
      g_instance = nullptr;
    }
    if (_queue)
    {
      vQueueDelete(_queue);
      _queue = nullptr;
    }
    if (_peerMutex)
    {
      vSemaphoreDelete(_peerMutex);
      _peerMutex = nullptr;
    }
  }

  // Ensure ESP-NOW is on the fixed channel (call after WiFi state changes)
  void EspNowTransport::ensureChannel()
  {
    if (!_enabled) return;
    esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);
  }

  bool EspNowTransport::send(const uint8_t *data, size_t len)
  {
    if (!_enabled) return false;
    if (!data || len == 0 || len > kMaxPacketSize)
    {
      return false;
    }

    uint8_t dst[6];
    bool peer = false;
    if (_peerMutex && xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      peer = _hasPeer && _peerReady.load() &&
             (millis() - _lastPeerMs.load() < kPeerTimeoutMs);
      memcpy(dst, _peerMac, sizeof(dst));
      xSemaphoreGive(_peerMutex);
    }
    if (!peer)
    {
      memcpy(dst, kBroadcast, sizeof(dst));
    }

    esp_err_t err = esp_now_send(dst, data, len);
    if (err != ESP_OK && peer)
    {
      err = esp_now_send(kBroadcast, data, len);
    }
    return err == ESP_OK;
  }

  bool EspNowTransport::sendTo(const uint8_t *addr, uint16_t port,
                               const uint8_t *data, size_t len)
  {
    if (!_enabled) return false;
    (void)port;
    if (!addr || !data || len == 0 || len > kMaxPacketSize)
    {
      return false;
    }
    if (!esp_now_is_peer_exist(addr))
    {
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, addr, 6);
      peer.channel = kChannel;  // Use fixed channel
      peer.ifidx = WIFI_IF_STA;
      peer.encrypt = false;
      esp_err_t err = esp_now_add_peer(&peer);
      if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
      {
        Serial.printf("[espnow] add peer failed: %d\n", (int)err);
        return false;
      }
    }
    return esp_now_send(addr, data, len) == ESP_OK;
  }

  bool EspNowTransport::broadcast(const uint8_t *data, size_t len)
  {
    if (!_enabled) return false;
    if (!data || len == 0 || len > kMaxPacketSize)
    {
      return false;
    }
    return esp_now_send(kBroadcast, data, len) == ESP_OK;
  }

  bool EspNowTransport::sendHello(const char *name, uint8_t battery, Mode mode)
  {
    if (!_enabled) return false;
    alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(HelloPayload) +
                           sizeof(HelloDeviceIdentity)] = {};
    PacketHeader *header = (PacketHeader *)buf;
    HelloPayload *payload = (HelloPayload *)(buf + sizeof(PacketHeader));
    HelloDeviceIdentity *identity =
        (HelloDeviceIdentity *)(buf + sizeof(PacketHeader) +
                                sizeof(HelloPayload));

    header->magic = kMagic;
    header->version = kVersion;
    header->type = kPacketHello;
    header->flags = kHelloFlagHasDeviceMac;
    header->seq = 0;
    header->predictor = 0;
    header->adpcmIndex = 0;
    header->payloadLen = sizeof(HelloPayload) + sizeof(HelloDeviceIdentity);

    payload->battery = battery;
    payload->mode = (uint8_t)mode;
    memset(payload->name, 0, sizeof(payload->name));
    if (name)
    {
      strncpy(payload->name, name, sizeof(payload->name) - 1);
    }
    WiFi.macAddress(identity->mac);

    return broadcast(buf, sizeof(buf));
  }

  bool EspNowTransport::poll()
  {
    if (!_enabled) return false;
    if (!_peerMutex)
    {
      return false;
    }

    // Ensure ESP-NOW is on fixed channel (WiFi may have changed mode)
    ensureChannel();

    // Send periodic discovery if no peer connected
    uint32_t now = millis();
    bool hasPeer = false;
    if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      hasPeer = _hasPeer && _peerReady.load() &&
                (now - _lastPeerMs.load() < kPeerTimeoutMs);
      xSemaphoreGive(_peerMutex);
    }

    if (!hasPeer && (now - _lastDiscoveryMs >= kDiscoveryIntervalMs))
    {
      _lastDiscoveryMs = now;
      sendDiscovery();
    }

    uint8_t mac[6];
    bool pending = false;
    if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return false;
    }
    if (_pendingPeer)
    {
      memcpy(mac, _pendingMac, sizeof(mac));
      _pendingPeer = false;
      pending = true;
    }
    xSemaphoreGive(_peerMutex);

    if (!pending)
    {
      return false;
    }

    if (_lastPeerAddMs != 0 &&
        (uint32_t)(now - _lastPeerAddMs) < kPeerAddRetryMs)
    {
      if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
      {
        _pendingPeer = true;
        xSemaphoreGive(_peerMutex);
      }
      return false;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, sizeof(mac));
    peer.channel = kChannel;  // Use fixed channel
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
    {
      _peerReady.store(true);
      _lastPeerAddMs = 0;
      _lastPeerMs.store(now);  // Critical: update last peer time so hasPeer() works
      return true;
    }

    _lastPeerAddMs = now;
    Serial.printf("[espnow] peer add failed: %d\n", (int)err);
    if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      memcpy(_pendingMac, mac, sizeof(_pendingMac));
      _pendingPeer = true;
      xSemaphoreGive(_peerMutex);
    }
    return false;
  }

  void EspNowTransport::sendDiscovery()
  {
    if (!_enabled) return;

    // Send a hello packet with discovery flag
    alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(HelloPayload)] = {};
    PacketHeader *header = (PacketHeader *)buf;
    HelloPayload *payload = (HelloPayload *)(buf + sizeof(PacketHeader));

    header->magic = kMagic;
    header->version = kVersion;
    header->type = kPacketHello;
    header->flags = 0x80; // Discovery flag
    header->seq = 0;
    header->predictor = 0;
    header->adpcmIndex = 0;
    header->payloadLen = sizeof(HelloPayload);

    payload->battery = 0xFF;
    payload->mode = (uint8_t)Mode::Live;
    memset(payload->name, 0, sizeof(payload->name));
    // Use device name if set
    if (_deviceName[0])
    {
      strncpy(payload->name, _deviceName, sizeof(payload->name) - 1);
    }

    broadcast(buf, sizeof(buf));
  }

  bool EspNowTransport::hasPeer() const
  {
    if (!_enabled) return false;
    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return false;
    }
    bool online = _hasPeer && (millis() - _lastPeerMs.load() < kPeerTimeoutMs);
    xSemaphoreGive(_peerMutex);
    return online;
  }

  void EspNowTransport::peerName(char *out, size_t outSize) const
  {
    if (!out || outSize == 0)
    {
      return;
    }
    out[0] = 0;
    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return;
    }
    strncpy(out, _peerName, outSize - 1);
    out[outSize - 1] = 0;
    xSemaphoreGive(_peerMutex);
  }

  bool EspNowTransport::popPacket(RxPacket &out, uint32_t timeoutMs)
  {
    if (!_queue)
    {
      return false;
    }
    return xQueueReceive(_queue, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }

  void EspNowTransport::onRecv(const esp_now_recv_info_t *info,
                               const uint8_t *data, int len)
  {
    if (!g_instance || !info || !info->src_addr || !data || len <= 0)
    {
      return;
    }
    int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    g_instance->handlePacket(info->src_addr, rssi, data, len);
  }

  void EspNowTransport::handlePacket(const uint8_t *src, int8_t rssi,
                                     const uint8_t *data, int len)
  {
    if (len < (int)sizeof(PacketHeader) || len > (int)kMaxPacketSize)
    {
      return;
    }

    const PacketHeader *header = (const PacketHeader *)data;
    if (header->magic != kMagic || header->version != kVersion)
    {
      return;
    }
    if (header->payloadLen > len - (int)sizeof(PacketHeader))
    {
      return;
    }

    if (header->type == kPacketHello)
    {
      if (header->payloadLen < sizeof(HelloPayload))
      {
        return;
      }
      const HelloPayload *payload =
          (const HelloPayload *)(data + sizeof(PacketHeader));
      notePeer(src);
      updatePeerName(payload->name);
      _peerRssi.store(rssi);
      _lastPeerMs.store(millis());
      queuePacket(src, rssi, data, len);
      return;
    }

    if (header->type == kPacketAudio || header->type == kPacketSpy)
    {
      if (header->payloadLen == 0)
      {
        return;
      }
      notePeer(src);
      _peerRssi.store(rssi);
      _lastPeerMs.store(millis());
      queuePacket(src, rssi, data, len);
    }
  }

  void EspNowTransport::queuePacket(const uint8_t *src, int8_t rssi,
                                    const uint8_t *data, int len)
  {
    if (!_queue || !src || !data || len <= 0 || len > (int)kMaxPacketSize)
    {
      return;
    }
    RxPacket packet;
    memcpy(packet.src, src, sizeof(packet.src));
    packet.rssi = rssi;
    packet.port = 0;
    packet.len = (uint16_t)len;
    memcpy(packet.data, data, len);
    if (xQueueSend(_queue, &packet, 0) != pdTRUE)
    {
      _dropped.fetch_add(1);
    }
  }

  void EspNowTransport::notePeer(const uint8_t *mac)
  {
    if (!mac || !_peerMutex || xSemaphoreTake(_peerMutex, 0) != pdTRUE)
    {
      return;
    }

    if (!_hasPeer || memcmp(_peerMac, mac, sizeof(_peerMac)) != 0)
    {
      memcpy(_peerMac, mac, sizeof(_peerMac));
      memcpy(_pendingMac, mac, sizeof(_pendingMac));
      _hasPeer = true;
      _pendingPeer = true;
      _peerReady.store(false);
    }

    xSemaphoreGive(_peerMutex);
  }

  void EspNowTransport::updatePeerName(const char *name)
  {
    if (!name)
    {
      return;
    }
    if (!_peerMutex || xSemaphoreTake(_peerMutex, 0) != pdTRUE)
    {
      return;
    }
    strncpy(_peerName, name, sizeof(_peerName) - 1);
    _peerName[sizeof(_peerName) - 1] = 0;
    xSemaphoreGive(_peerMutex);
  }

} // namespace intercom