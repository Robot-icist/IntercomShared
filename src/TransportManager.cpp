#include "Transport.h"

namespace intercom
{
  // Hardcoded WiFi credentials fallback (compile-time)
  namespace
  {
    constexpr const char *kFallbackSSIDs[] = {
        // "HOME_WIFI",
        // "OFFICE_WIFI",
        nullptr};
    constexpr const char *kFallbackPasses[] = {
        // "password123",
        // "officepass",
        nullptr};
  }

  // ============================================================================
  // TransportManager Implementation
  // ============================================================================

  bool TransportManager::begin(const char *deviceName)
  {
    // Set hostname for WiFi
    _wifi.setHostname(deviceName);
    _espNow.setPeerName(deviceName);
    _web.setDeviceName(deviceName);
    _web.setTransportManager(this);

    // Always start ESP-NOW (works everywhere)
    bool espNowOk = _espNow.begin();
    if (espNowOk)
    {
      _activeTransport = &_espNow;
      Serial.println("[transport] ESP-NOW active");
    }

    // Start WiFi in background (may take time)
    _wifi.begin(); // Non-blocking, starts config portal if needed

    // Start web transport (deferred until WiFi connects)
    Serial.println("[transport] Starting web transport...");
    _web.begin();
    Serial.println("[transport] Web transport begin() returned");

    return espNowOk;
  }

  void TransportManager::loop()
  {
    static uint32_t lastTransportDebug = 0;
    uint32_t now = millis();

    // Poll all transports
    _espNow.poll();
    _wifi.poll();
    _web.poll();

    // Debug: print WiFi status periodically
    if (now - lastTransportDebug > 30000)
    {
      lastTransportDebug = now;
      Serial.printf("[transport] WiFi status: %d (connected=%d), Web running: %d, Web clients: %u\n",
                    WiFi.status(), WiFi.isConnected(), _web.isRunning(), _web.connectedClients());
    }

    // Start web transport if WiFi is connected and web not running
    if (_wifi.isConnected() && !_web.hasPeer() && !_web.connectedClients())
    {
      // Web transport starts automatically when WiFi is connected
    }

    // Auto-select best transport
    if (_preferredTransport == "auto")
    {
      selectBestTransport();
    }

    // Send periodic hellos from active transport
    static uint32_t lastHello = 0;
    if (millis() - lastHello >= kHelloIntervalMs)
    {
      lastHello = millis();
      if (_activeTransport)
      {
        char name[16];
        _activeTransport->peerName(name, sizeof(name)); // Not quite right, need device name
        // The app layer sends hello, transport just delivers
      }
    }
  }

  void TransportManager::selectBestTransport()
  {
    // Priority: Web (if clients connected) > WiFi (if peer connected) > ESP-NOW
    size_t webClients = _web.connectedClients();
    if (webClients > 0 && _activeTransport != &_web)
    {
      _activeTransport = &_web;
      if (_transportChangeCb)
        _transportChangeCb("web");
      Serial.printf("[transport] Switched to Web (%u clients)\n", (unsigned)webClients);
    }
    else if (_web.hasPeer() && _activeTransport != &_web)
    {
      _activeTransport = &_web;
      if (_transportChangeCb)
        _transportChangeCb("web");
      Serial.println("[transport] Switched to Web");
    }
    else if (_wifi.hasPeer() && _activeTransport != &_wifi && _web.connectedClients() == 0)
    {
      _activeTransport = &_wifi;
      if (_transportChangeCb)
        _transportChangeCb("wifi");
      Serial.println("[transport] Switched to WiFi");
    }
    else if (!_wifi.hasPeer() && !_web.hasPeer() && _activeTransport != &_espNow)
    {
      _activeTransport = &_espNow;
      if (_transportChangeCb)
        _transportChangeCb("espnow");
      Serial.println("[transport] Switched to ESP-NOW");
    }
  }

  bool TransportManager::tryTransport(Transport *t)
  {
    return t && t->begin();
  }

  void TransportManager::setPreferredTransport(const char *type)
  {
    _preferredTransport = type;
    if (strcmp(type, "wifi") == 0 || strcmp(type, "wlan") == 0)
    {
      _activeTransport = &_wifi;
    }
    else if (strcmp(type, "espnow") == 0)
    {
      _activeTransport = &_espNow;
    }
    else if (strcmp(type, "web") == 0)
    {
      _activeTransport = &_web;
    }
    else
    {
      selectBestTransport();
    }
  }

  const char *TransportManager::currentTransportType() const
  {
    if (_activeTransport == &_wifi)
      return "wifi";
    if (_activeTransport == &_espNow)
      return "espnow";
    if (_activeTransport == &_web)
      return "web";
    return "none";
  }

  void TransportManager::startWiFiConfig()
  {
    _wifi.startConfigPortal();
  }

  bool TransportManager::isWiFiConfigured() const
  {
    return _wifi.isWiFiConfigured();
  }

} // namespace intercom