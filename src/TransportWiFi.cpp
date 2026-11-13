#include "Transport.h"

#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>

namespace intercom
{
  // Fallback WiFi credentials (also accessible from TransportManager)
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
  // WiFiTransport Implementation
  // ============================================================================
  // ============================================================================
  // WiFiTransport Implementation
  // ============================================================================

  WiFiTransport::WiFiTransport()
  {
    _udpTask = nullptr;
    _configTask = nullptr;
    _running = false;
    _inConfigMode = false;
  }

  WiFiTransport::~WiFiTransport()
  {
    end();
  }

  bool WiFiTransport::begin()
  {
    _queue = xQueueCreate(16, sizeof(RxPacket));
    _peerMutex = xSemaphoreCreateMutex();
    if (!_queue || !_peerMutex)
    {
      return false;
    }

    // Try to load saved credentials first
    if (loadSavedCredentials())
    {
      Serial.println("[wifi] Using saved credentials");
      if (connectToWiFi(_savedSsid.c_str(), _savedPass.c_str()))
      {
        _wifiConnected = true;
        startMDNS(_hostname);
        _running = true;
        xTaskCreatePinnedToCore(&WiFiTransport::udpRecvTask, "wifi_udp_recv",
                                4096, this, 3, &_udpTask, 1);
        return true;
      }
    }

    // Try hardcoded fallbacks
    for (int i = 0; kFallbackSSIDs[i]; ++i)
    {
      Serial.printf("[wifi] Trying fallback: %s\n", kFallbackSSIDs[i]);
      if (connectToWiFi(kFallbackSSIDs[i], kFallbackPasses[i], 8000))
      {
        _wifiConnected = true;
        saveCredentials(kFallbackSSIDs[i], kFallbackPasses[i]);
        startMDNS(_hostname);
        _running = true;
        xTaskCreatePinnedToCore(&WiFiTransport::udpRecvTask, "wifi_udp_recv",
                                4096, this, 3, &_udpTask, 1);
        return true;
      }
    }

    // No credentials - stay in STA mode for ESP-NOW discovery
    // Delay config portal (AP mode) to allow ESP-NOW to find peers
    Serial.println("[wifi] No credentials, staying in STA mode for ESP-NOW discovery");
    Serial.printf("[wifi] Config portal will start in %lu seconds if no peer found\n",
                  kConfigPortalDelayMs / 1000);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    _configPortalStartMs = millis();
    _configPortalDelayActive = true;
    _running = true; // Running in STA mode for ESP-NOW
    return true;
  }

  void WiFiTransport::end()
  {
    _running = false;
    if (_udpTask)
    {
      vTaskDelete(_udpTask);
      _udpTask = nullptr;
    }
    if (_configTask)
    {
      vTaskDelete(_configTask);
      _configTask = nullptr;
    }
    stopMDNS();
    _udp.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

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
    _wifiConnected = false;
    _hasPeer = false;
  }

  // Disconnect from WiFi AP but keep transport infrastructure alive for reconnection
  void WiFiTransport::disconnect()
  {
    if (!_wifiConnected)
      return;

    Serial.println("[wifi] Disconnecting from AP");
    stopMDNS();
    if (_udpTask)
    {
      vTaskDelete(_udpTask);
      _udpTask = nullptr;
    }
    _udp.stop();
    WiFi.disconnect(true);
    // Stay in STA mode for ESP-NOW coexistence
    WiFi.mode(WIFI_STA);
    _wifiConnected = false;
    _hasPeer = false;
    _peerReady.store(false);
  }

  bool WiFiTransport::send(const uint8_t *data, size_t len)
  {
    if (!data || len == 0 || len > kMaxPacketSize || !_hasPeer || !_wifiConnected)
    {
      return false;
    }

    uint8_t dst[6];
    uint16_t port;
    if (_peerMutex && xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      memcpy(dst, _peerMac, sizeof(dst));
      port = _peerPort;
      xSemaphoreGive(_peerMutex);
    }
    else
    {
      return false;
    }

    IPAddress ip(dst[0], dst[1], dst[2], dst[3]);
    _udp.beginPacket(ip, port);
    size_t written = _udp.write(data, len);
    _udp.endPacket();
    return written == len;
  }

  bool WiFiTransport::sendTo(const uint8_t *addr, uint16_t port,
                             const uint8_t *data, size_t len)
  {
    if (!addr || !data || len == 0 || len > kMaxPacketSize || !_wifiConnected)
    {
      return false;
    }
    IPAddress ip(addr[0], addr[1], addr[2], addr[3]);
    if (port == 0)
    {
      port = kIntercomPort;
    }
    _udp.beginPacket(ip, port);
    size_t written = _udp.write(data, len);
    _udp.endPacket();
    return written == len;
  }

  bool WiFiTransport::broadcast(const uint8_t *data, size_t len)
  {
    if (!data || len == 0 || len > kMaxPacketSize || !_wifiConnected)
    {
      return false;
    }
    IPAddress broadcastIP = WiFi.broadcastIP();
    _udp.beginPacket(broadcastIP, kIntercomPort);
    size_t written = _udp.write(data, len);
    _udp.endPacket();
    return written == len;
  }

  bool WiFiTransport::sendHello(const char *name, uint8_t battery, Mode mode)
  {
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

  bool WiFiTransport::poll()
  {
    // Check if config portal delay has expired and start config portal
    if (_configPortalDelayActive && !_wifiConnected && !_inConfigMode)
    {
      uint32_t now = millis();
      if (now - _configPortalStartMs >= kConfigPortalDelayMs)
      {
        Serial.println("[wifi] Config portal delay expired, starting AP mode");
        _configPortalDelayActive = false;
        startConfigPortal();
        return true;
      }
    }

    if (!_wifiConnected || !_running)
    {
      // Try to reconnect if we have credentials
      if (!_inConfigMode && _savedSsid.length() > 0)
      {
        Serial.println("[wifi] Reconnecting...");
        if (connectToWiFi(_savedSsid.c_str(), _savedPass.c_str()))
        {
          _wifiConnected = true;
          startMDNS(_hostname);
          _running = true;
          xTaskCreatePinnedToCore(&WiFiTransport::udpRecvTask, "wifi_udp_recv",
                                  4096, this, 3, &_udpTask, 1);
        }
      }
      return false;
    }

    // Clear config portal delay once WiFi is connected
    if (_wifiConnected)
    {
      _configPortalDelayActive = false;
    }

    // Send periodic discovery
    uint32_t now = millis();
    if (now - _lastDiscoveryMs >= kDiscoveryIntervalMs)
    {
      sendDiscovery();
      _lastDiscoveryMs = now;
    }

    // Check peer timeout
    if (_hasPeer && (now - _lastPeerMs > kPeerTimeoutMs))
    {
      if (_peerMutex && xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) == pdTRUE)
      {
        _hasPeer = false;
        _peerReady.store(false);
        xSemaphoreGive(_peerMutex);
        Serial.println("[wifi] Peer timeout");
      }
    }

    return _hasPeer;
  }

  bool WiFiTransport::hasPeer() const
  {
    if (!_wifiConnected)
    {
      return false;
    }
    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return false;
    }
    bool online = _hasPeer && _peerReady.load() &&
                  (millis() - _lastPeerMs < kPeerTimeoutMs);
    xSemaphoreGive(_peerMutex);
    return online;
  }

  void WiFiTransport::peerName(char *out, size_t outSize) const
  {
    if (!out || outSize == 0)
      return;
    out[0] = 0;
    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return;
    }
    strncpy(out, _peerName, outSize - 1);
    out[outSize - 1] = 0;
    xSemaphoreGive(_peerMutex);
  }

  bool WiFiTransport::popPacket(RxPacket &out, uint32_t timeoutMs)
  {
    if (!_queue)
      return false;
    return xQueueReceive(_queue, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }

  void WiFiTransport::sendDiscovery()
  {
    if (!_wifiConnected)
      return;

    alignas(4) uint8_t buf[sizeof(PacketHeader) + sizeof(HelloPayload)];
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
    if (_hostname[0])
    {
      strncpy(payload->name, _hostname, sizeof(payload->name) - 1);
    }

    IPAddress broadcast = WiFi.broadcastIP();
    _udp.beginPacket(broadcast, kIntercomPort);
    _udp.write(buf, sizeof(buf));
    _udp.endPacket();
  }

  void WiFiTransport::udpRecvTask(void *arg)
  {
    static_cast<WiFiTransport *>(arg)->processUDP();
  }

  void WiFiTransport::processUDP()
  {
    _udp.begin(kIntercomPort);

    while (_running)
    {
      int packetSize = _udp.parsePacket();
      if (packetSize > 0)
      {
        IPAddress remoteIP = _udp.remoteIP();
        uint16_t remotePort = _udp.remotePort();

        uint8_t buffer[256];
        int len = _udp.read(buffer, sizeof(buffer));

        if (len >= (int)sizeof(PacketHeader))
        {
          const PacketHeader *header = (const PacketHeader *)buffer;
          if (header->magic == kMagic && header->version == kVersion)
          {
            int8_t rssi = WiFi.RSSI();

            if (header->type == kPacketHello)
            {
              if (header->flags & 0x80)
              {
                // Discovery packet: transport-level only, the app gets the
                // regular hello once the link is selected.
                handleDiscoveryResponse(remoteIP, remotePort, buffer, len);
              }
              else if (header->payloadLen >= sizeof(HelloPayload))
              {
                if (remoteIP != WiFi.localIP())
                {
                  const HelloPayload *payload =
                      (const HelloPayload *)(buffer + sizeof(PacketHeader));
                  notePeer(remoteIP, remotePort, rssi);
                  updatePeerName(payload->name);
                  queuePacket(remoteIP, remotePort, rssi, buffer, len);
                }
              }
            }
            else if (header->type == kPacketAudio ||
                     header->type == kPacketSpy)
            {
              notePeer(remoteIP, remotePort, rssi);
              _peerRssi.store(rssi);
              _lastPeerMs = millis();
              queuePacket(remoteIP, remotePort, rssi, buffer, len);
            }
          }
        }
      }
      vTaskDelay(1);
    }

    _udp.stop();
  }

  void WiFiTransport::queuePacket(const IPAddress &srcIP, uint16_t port,
                                  int8_t rssi, const uint8_t *data, int len)
  {
    if (!_queue || !data || len <= 0 || len > (int)kMaxPacketSize)
    {
      return;
    }
    RxPacket packet;
    packet.src[0] = srcIP[0];
    packet.src[1] = srcIP[1];
    packet.src[2] = srcIP[2];
    packet.src[3] = srcIP[3];
    packet.src[4] = 0;
    packet.src[5] = 0;
    packet.rssi = rssi;
    packet.port = port;
    packet.len = (uint16_t)len;
    memcpy(packet.data, data, len);
    if (xQueueSend(_queue, &packet, 0) != pdTRUE)
    {
      _dropped.fetch_add(1);
    }
  }

  void WiFiTransport::handleDiscoveryResponse(const IPAddress &srcIP, uint16_t srcPort,
                                              const uint8_t *data, int len)
  {
    const PacketHeader *header = (const PacketHeader *)data;
    if (header->payloadLen < sizeof(HelloPayload))
      return;

    const HelloPayload *payload = (const HelloPayload *)(data + sizeof(PacketHeader));

    // Don't discover ourselves
    if (strcmp(payload->name, _hostname) == 0)
      return;

    notePeer(srcIP, srcPort, WiFi.RSSI());
  }

  void WiFiTransport::notePeer(const IPAddress &srcIP, uint16_t srcPort, int8_t rssi)
  {
    uint8_t mac[6];
    mac[0] = srcIP[0];
    mac[1] = srcIP[1];
    mac[2] = srcIP[2];
    mac[3] = srcIP[3];
    mac[4] = 0;
    mac[5] = 0;

    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return;
    }

    if (!_hasPeer || memcmp(_peerMac, mac, sizeof(mac)) != 0 || _peerPort != srcPort)
    {
      memcpy(_peerMac, mac, sizeof(mac));
      _peerPort = srcPort;
      _hasPeer = true;
      _peerReady.store(true);
      _lastPeerMs = millis();
    }
    else
    {
      _lastPeerMs = millis();
    }

    xSemaphoreGive(_peerMutex);
  }

  void WiFiTransport::updatePeerName(const char *name)
  {
    if (!name)
      return;
    if (!_peerMutex || xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      return;
    }
    strncpy(_peerName, name, sizeof(_peerName) - 1);
    _peerName[sizeof(_peerName) - 1] = 0;
    xSemaphoreGive(_peerMutex);
  }

  // ESP-NOW peer coordination - called by TransportManager
  void WiFiTransport::notifyEspNowPeerStatus(bool hasPeer)
  {
    if (hasPeer)
    {
      // ESP-NOW has a peer - cancel config portal delay
      if (_configPortalDelayActive)
      {
        Serial.println("[wifi] ESP-NOW peer found, cancelling config portal delay");
        _configPortalDelayActive = false;
      }
    }
    else
    {
      // ESP-NOW lost peer - restart config portal delay if not in config mode and not connected
      if (!_inConfigMode && !_wifiConnected && !_configPortalDelayActive)
      {
        Serial.println("[wifi] ESP-NOW peer lost, restarting config portal delay");
        _configPortalStartMs = millis();
        _configPortalDelayActive = true;
      }
    }
  }

  // WiFi Configuration
  bool WiFiTransport::connectToWiFi(const char *ssid, const char *pass, uint32_t timeoutMs)
  {
    if (!ssid || strlen(ssid) == 0)
      return false;

    // Clear config portal delay when attempting connection
    _configPortalDelayActive = false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    while (millis() - start < timeoutMs)
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.printf("[wifi] Connected to %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());
        return true;
      }
      vTaskDelay(100);
    }
    Serial.printf("[wifi] Connection to %s timed out\n", ssid);
    return false;
  }

  void WiFiTransport::startConfigPortal()
  {
    _configPortalDelayActive = false; // Clear delay when explicitly starting
    _inConfigMode = true;
    WiFi.mode(WIFI_AP);

    snprintf(_hostname, sizeof(_hostname), "Intercom-%06llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    WiFi.softAP(_hostname, "intercom123");

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[wifi] Config portal started: %s, IP: %s\n", _hostname, apIP.toString().c_str());

    // Start captive portal task
    xTaskCreatePinnedToCore(&WiFiTransport::configPortalTask, "wifi_config",
                            8192, this, 2, &_configTask, 0);
  }

  void WiFiTransport::configPortalTask(void *arg)
  {
    static_cast<WiFiTransport *>(arg)->runConfigPortal();
  }

  void WiFiTransport::runConfigPortal()
  {
    WebServer server(80);
    DNSServer dnsServer;

    // DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());

    // Serve config page
    server.on("/", [this, &server]()
              {
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Intercom WiFi Setup</title>
  <style>
    body { font-family: sans-serif; max-width: 400px; margin: 2rem auto; padding: 1rem; }
    h1 { color: #333; }
    .form-group { margin-bottom: 1rem; }
    label { display: block; margin-bottom: 0.5rem; font-weight: bold; }
    input { width: 100%; padding: 0.75rem; font-size: 1rem; box-sizing: border-box; }
    button { width: 100%; padding: 1rem; font-size: 1.1rem; background: #007bff; color: white; border: none; border-radius: 4px; }
    button:disabled { background: #ccc; }
    .networks { max-height: 200px; overflow-y: auto; border: 1px solid #ddd; padding: 0.5rem; }
    .network-item { padding: 0.5rem; cursor: pointer; border-bottom: 1px solid #eee; }
    .network-item:last-child { border-bottom: none; }
    .network-item:hover { background: #f5f5f5; }
    .signal { float: right; font-size: 0.8rem; color: #666; }
  </style>
</head>
<body>
  <h1>Intercom WiFi Setup</h1>
  <p>Select or enter WiFi network:</p>
  <div class="form-group">
    <label>Available Networks</label>
    <div class="networks" id="networks">Scanning...</div>
  </div>
  <div class="form-group">
    <label>SSID</label>
    <input type="text" id="ssid" placeholder="Network name">
  </div>
  <div class="form-group">
    <label>Password</label>
    <input type="password" id="pass" placeholder="Password (leave empty for open)">
  </div>
  <button id="connectBtn" onclick="connect()">Connect</button>
  <p id="status" style="margin-top: 1rem;"></p>

  <script>
    async function scan() {
      const res = await fetch('/scan');
      const nets = await res.json();
      const div = document.getElementById('networks');
      div.innerHTML = nets.map(n => 
        `<div class="network-item" onclick="selectNet('${n.ssid}', ${n.rssi})">
          ${n.ssid} <span class="signal">${n.rssi} dBm ${n.enc ? '🔒' : '🔓'}</span>
        </div>`
      ).join('');
    }
    function selectNet(ssid, rssi) {
      document.getElementById('ssid').value = ssid;
      document.getElementById('pass').focus();
    }
    async function connect() {
      const btn = document.getElementById('connectBtn');
      const status = document.getElementById('status');
      btn.disabled = true;
      btn.textContent = 'Connecting...';
      const res = await fetch('/connect', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          ssid: document.getElementById('ssid').value,
          pass: document.getElementById('pass').value
        })
      });
      const result = await res.json();
      if (result.success) {
        status.innerHTML = '<span style="color:green">Connected! Device will restart...</span>';
        setTimeout(() => location.reload(), 3000);
      } else {
        status.innerHTML = '<span style="color:red">Failed: ' + result.error + '</span>';
        btn.disabled = false;
        btn.textContent = 'Connect';
      }
    }
    scan();
    setInterval(scan, 10000);
  </script>
</body>
</html>
)HTML";
    server.send(200, "text/html", html); });

    // Scan endpoint
    server.on("/scan", [this, &server]()
              {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      server.send(200, "application/json", "[]");
      return;
    }
    if (n == 0) {
      WiFi.scanNetworks(true);
      server.send(200, "application/json", "[]");
      return;
    }
    
    String json = "[";
    for (int i = 0; i < n; ++i) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    server.send(200, "application/json", json); });

    // Connect endpoint
    server.on("/connect", HTTP_POST, [this, &server]()
              {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"No data\"}");
      return;
    }
    
    String body = server.arg("plain");
    String ssid, pass;
    
    // Simple JSON parse
    int ssidStart = body.indexOf("\"ssid\":\"");
    if (ssidStart >= 0) {
      ssidStart += 8;
      int ssidEnd = body.indexOf("\"", ssidStart);
      ssid = body.substring(ssidStart, ssidEnd);
    }
    int passStart = body.indexOf("\"pass\":\"");
    if (passStart >= 0) {
      passStart += 8;
      int passEnd = body.indexOf("\"", passStart);
      pass = body.substring(passStart, passEnd);
    }
    
    if (ssid.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Empty SSID\"}");
      return;
    }
    
    server.send(200, "application/json", "{\"success\":true}");
    
    // Save and connect after response sent
    vTaskDelay(500);
    saveCredentials(ssid.c_str(), pass.c_str());
    if (connectToWiFi(ssid.c_str(), pass.c_str())) {
      _wifiConnected = true;
      _inConfigMode = false;
      WiFi.softAPdisconnect(true);
      startMDNS(_hostname);
      _running = true;
      xTaskCreatePinnedToCore(&WiFiTransport::udpRecvTask, "wifi_udp_recv",
                              4096, this, 3, &_udpTask, 1);
      if (_configCb) {
        _configCb(ssid.c_str(), pass.c_str());
      }
      // Restart to apply cleanly
      ESP.restart();
    } });

    // Captive portal redirects
    server.onNotFound([this, &server]()
                      {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", ""); });

    server.begin();

    while (_inConfigMode)
    {
      dnsServer.processNextRequest();
      server.handleClient();
      vTaskDelay(10);
    }

    server.stop();
  }

  bool WiFiTransport::loadSavedCredentials()
  {
    Preferences prefs;
    if (prefs.begin("intercom", true))
    {
      _savedSsid = prefs.getString("wifi_ssid", "");
      _savedPass = prefs.getString("wifi_pass", "");
      prefs.end();
      return _savedSsid.length() > 0;
    }
    return false;
  }

  void WiFiTransport::saveCredentials(const char *ssid, const char *pass)
  {
    Preferences prefs;
    if (prefs.begin("intercom", false))
    {
      prefs.putString("wifi_ssid", ssid);
      prefs.putString("wifi_pass", pass);
      prefs.end();
      _savedSsid = ssid;
      _savedPass = pass;
    }
  }

  void WiFiTransport::startMDNS(const char *hostname)
  {
    if (!_mdnsStarted && hostname && strlen(hostname) > 0)
    {
      if (MDNS.begin(hostname))
      {
        MDNS.addService("intercom", "udp", kIntercomPort);
        _mdnsStarted = true;
        Serial.printf("[mdns] Started as %s.local\n", hostname);
      }
    }
  }

  void WiFiTransport::stopMDNS()
  {
    if (_mdnsStarted)
    {
      MDNS.end();
      _mdnsStarted = false;
    }
  }

  void WiFiTransport::setHostname(const char *name)
  {
    if (name)
    {
      snprintf(_hostname, sizeof(_hostname), "%s-%06llX",
               name, (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));
    }
  }

  bool WiFiTransport::isWiFiConfigured() const
  {
    return _savedSsid.length() > 0;
  }

} // namespace intercom