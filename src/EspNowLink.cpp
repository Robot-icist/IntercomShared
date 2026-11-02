#include "EspNowLink.h"

#include <cstring>

namespace intercom {
namespace {

constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint32_t kPeerAddRetryMs = 500;
EspNowLink* g_link = nullptr;

}  // namespace

bool EspNowLink::begin() {
  _queue = xQueueCreate(16, sizeof(RxPacket));
  _peerMutex = xSemaphoreCreateMutex();
  if (!_queue || !_peerMutex) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[espnow] init failed");
    return false;
  }

  esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

  g_link = this;
  esp_now_register_recv_cb(&EspNowLink::onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcast, sizeof(kBroadcast));
  peer.channel = kChannel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[espnow] broadcast peer add failed");
    return false;
  }

  Serial.printf("[espnow] channel %u, MAC %s\n", (unsigned)kChannel,
                WiFi.macAddress().c_str());
  return true;
}

void EspNowLink::end() {
  esp_now_deinit();
  if (g_link == this) {
    g_link = nullptr;
  }
  if (_queue) {
    vQueueDelete(_queue);
    _queue = nullptr;
  }
  if (_peerMutex) {
    vSemaphoreDelete(_peerMutex);
    _peerMutex = nullptr;
  }
}

bool EspNowLink::send(const uint8_t* data, size_t len) {
  if (!data || len == 0 || len > kMaxPacketSize) {
    return false;
  }

  uint8_t dst[6];
  bool peer = false;
  if (_peerMutex && xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    peer = _hasPeer && _peerReady.load() &&
           (millis() - _lastPeerMs.load() < kPeerTimeoutMs);
    memcpy(dst, _peerMac, sizeof(dst));
    xSemaphoreGive(_peerMutex);
  }
  if (!peer) {
    memcpy(dst, kBroadcast, sizeof(dst));
  }

  esp_err_t err = esp_now_send(dst, data, len);
  if (err != ESP_OK && peer) {
    // The peer table entry may have been lost; fall back to broadcast so the
    // other unit still receives the frame.
    err = esp_now_send(kBroadcast, data, len);
  }
  return err == ESP_OK;
}

bool EspNowLink::sendHello(const char* name, uint8_t battery, Mode mode) {
  alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(HelloPayload)];
  PacketHeader* header = (PacketHeader*)buf;
  HelloPayload* payload = (HelloPayload*)(buf + sizeof(PacketHeader));

  header->magic = kMagic;
  header->version = kVersion;
  header->type = kPacketHello;
  header->flags = 0;
  header->seq = 0;
  header->predictor = 0;
  header->adpcmIndex = 0;
  header->payloadLen = sizeof(HelloPayload);

  payload->battery = battery;
  payload->mode = (uint8_t)mode;
  memset(payload->name, 0, sizeof(payload->name));
  if (name) {
    strncpy(payload->name, name, sizeof(payload->name) - 1);
  }

  return send(buf, sizeof(buf));
}

bool EspNowLink::hasPeer() const {
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }
  bool online = _hasPeer && (millis() - _lastPeerMs.load() < kPeerTimeoutMs);
  xSemaphoreGive(_peerMutex);
  return online;
}

void EspNowLink::peerName(char* out, size_t outSize) const {
  if (!out || outSize == 0) {
    return;
  }
  out[0] = 0;
  if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return;
  }
  strncpy(out, _peerName, outSize - 1);
  out[outSize - 1] = 0;
  xSemaphoreGive(_peerMutex);
}

bool EspNowLink::popPacket(RxPacket& out, uint32_t timeoutMs) {
  if (!_queue) {
    return false;
  }
  return xQueueReceive(_queue, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void EspNowLink::onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!g_link || !info || !info->src_addr || !data || len <= 0) {
    return;
  }
  int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
  g_link->handlePacket(info->src_addr, rssi, data, len);
}

void EspNowLink::handlePacket(const uint8_t* src, int8_t rssi, const uint8_t* data,
                              int len) {
  if (len < (int)sizeof(PacketHeader) || len > (int)kMaxPacketSize) {
    return;
  }

  const PacketHeader* header = (const PacketHeader*)data;
  if (header->magic != kMagic || header->version != kVersion) {
    return;
  }
  if (header->payloadLen > len - (int)sizeof(PacketHeader)) {
    return;
  }

  if (header->type == kPacketHello) {
    if (header->payloadLen < sizeof(HelloPayload)) {
      return;
    }
    const HelloPayload* payload =
        (const HelloPayload*)(data + sizeof(PacketHeader));
    notePeer(src);
    updatePeerName(payload->name);
    _peerRssi.store(rssi);
    _lastPeerMs.store(millis());
    return;
  }

  if (header->type == kPacketAudio) {
    if (header->payloadLen == 0) {
      return;
    }
    notePeer(src);
    _peerRssi.store(rssi);
    _lastPeerMs.store(millis());

    RxPacket packet;
    memcpy(packet.src, src, sizeof(packet.src));
    packet.rssi = rssi;
    packet.len = (uint16_t)len;
    memcpy(packet.data, data, len);
    if (xQueueSend(_queue, &packet, 0) != pdTRUE) {
      _dropped.fetch_add(1);
    }
  }
}

void EspNowLink::notePeer(const uint8_t* mac) {
  if (!mac || !_peerMutex || xSemaphoreTake(_peerMutex, 0) != pdTRUE) {
    return;
  }

  if (!_hasPeer || memcmp(_peerMac, mac, sizeof(_peerMac)) != 0) {
    memcpy(_peerMac, mac, sizeof(_peerMac));
    memcpy(_pendingMac, mac, sizeof(_pendingMac));
    _hasPeer = true;
    _pendingPeer = true;
    _peerReady.store(false);
  }

  xSemaphoreGive(_peerMutex);
}

bool EspNowLink::poll() {
  if (!_peerMutex) {
    return false;
  }

  uint8_t mac[6];
  bool pending = false;
  if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }
  if (_pendingPeer) {
    memcpy(mac, _pendingMac, sizeof(mac));
    _pendingPeer = false;
    pending = true;
  }
  xSemaphoreGive(_peerMutex);

  if (!pending) {
    return false;
  }

  const uint32_t now = millis();
  if (_lastPeerAddMs != 0 &&
      (uint32_t)(now - _lastPeerAddMs) < kPeerAddRetryMs) {
    if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      _pendingPeer = true;
      xSemaphoreGive(_peerMutex);
    }
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, sizeof(mac));
  peer.channel = kChannel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  esp_err_t err = esp_now_add_peer(&peer);
  if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
    _peerReady.store(true);
    _lastPeerAddMs = 0;
    return true;
  }

  // Try again later instead of silently staying on broadcast forever.
  _lastPeerAddMs = now;
  Serial.printf("[espnow] peer add failed: %d\n", (int)err);
  if (xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(_pendingMac, mac, sizeof(_pendingMac));
    _pendingPeer = true;
    xSemaphoreGive(_peerMutex);
  }
  return false;
}

void EspNowLink::updatePeerName(const char* name) {
  if (!name) {
    return;
  }
  // Called from the Wi-Fi receive callback: never block there.
  if (!_peerMutex || xSemaphoreTake(_peerMutex, 0) != pdTRUE) {
    return;
  }
  strncpy(_peerName, name, sizeof(_peerName) - 1);
  _peerName[sizeof(_peerName) - 1] = 0;
  xSemaphoreGive(_peerMutex);
}

}  // namespace intercom
