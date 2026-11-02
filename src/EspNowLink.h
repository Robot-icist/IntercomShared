#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <atomic>

#include "IntercomConfig.h"
#include "IntercomTypes.h"

namespace intercom {

// ESP-NOW transport with a small receive queue.
//
// The link broadcasts hello packets until it hears a peer. Audio then goes to
// that peer's unicast address, and falls back to broadcast after a timeout.
class EspNowLink {
 public:
  bool begin();
  void end();

  bool send(const uint8_t* data, size_t len);
  bool sendHello(const char* name, uint8_t battery, Mode mode);

  bool hasPeer() const;
  void peerName(char* out, size_t outSize) const;
  int8_t peerRssi() const { return _peerRssi.load(); }
  uint32_t droppedPackets() const { return _dropped.load(); }

  bool popPacket(RxPacket& out, uint32_t timeoutMs = 0);

 private:
  static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
  void handlePacket(const uint8_t* src, int8_t rssi, const uint8_t* data, int len);
  void addPeer(const uint8_t* mac);
  void updatePeerName(const char* name);

  QueueHandle_t _queue = nullptr;
  SemaphoreHandle_t _peerMutex = nullptr;
  uint8_t _peerMac[6] = {0};
  bool _hasPeer = false;
  char _peerName[16] = {0};
  std::atomic<uint32_t> _lastPeerMs{0};
  std::atomic<int8_t> _peerRssi{0};
  std::atomic<uint32_t> _dropped{0};
};

}  // namespace intercom
