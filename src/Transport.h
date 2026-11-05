#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "IntercomConfig.h"
#include "IntercomTypes.h"

namespace intercom
{

  // Forward declarations
  class TransportManager;

  // Transport interface - all transports implement this for unified API
  class Transport
  {
  public:
    virtual ~Transport() = default;

    // Initialize transport, return true on success
    virtual bool begin() = 0;

    // Cleanup transport
    virtual void end() = 0;

    // Send audio/hello packet
    virtual bool send(const uint8_t *data, size_t len) = 0;
    virtual bool sendTo(const uint8_t *addr, uint16_t port, const uint8_t *data,
                        size_t len) = 0;
    virtual bool broadcast(const uint8_t *data, size_t len) = 0;
    virtual bool sendHello(const char *name, uint8_t battery, Mode mode) = 0;

    // Poll for maintenance (peer management, etc.)
    virtual bool poll() = 0;

    // Check if peer is connected
    virtual bool hasPeer() const = 0;
    virtual void peerName(char *out, size_t outSize) const = 0;
    virtual int8_t peerRssi() const = 0;
    virtual uint32_t droppedPackets() const = 0;

    // Get received packet
    virtual bool popPacket(RxPacket &out, uint32_t timeoutMs = 0) = 0;

    // Transport type identification
    virtual const char *typeName() const = 0;
    virtual bool isLocalNetwork() const = 0; // WiFi vs ESP-NOW

    // Configuration callbacks
    using ConfigCallback = std::function<void(const char *ssid, const char *pass)>;
    virtual void setConfigCallback(ConfigCallback cb) { (void)cb; }
  };

  // ESP-NOW transport (existing implementation wrapped)
  class EspNowTransport : public Transport
  {
  public:
    bool begin() override;
    void end() override;

    bool send(const uint8_t *data, size_t len) override;
    bool sendTo(const uint8_t *addr, uint16_t port, const uint8_t *data,
                size_t len) override;
    bool broadcast(const uint8_t *data, size_t len) override;
    bool sendHello(const char *name, uint8_t battery, Mode mode) override;

    bool poll() override;

    bool hasPeer() const override;
    void peerName(char *out, size_t outSize) const override;
    int8_t peerRssi() const override { return _peerRssi.load(); }
    uint32_t droppedPackets() const override { return _dropped.load(); }

    bool popPacket(RxPacket &out, uint32_t timeoutMs = 0) override;

    const char *typeName() const override { return "ESP-NOW"; }
    bool isLocalNetwork() const override { return false; }

    // Public setters for TransportManager
    void setPeerName(const char *name)
    {
      if (name)
      {
        strncpy(_peerName, name, sizeof(_peerName) - 1);
        _peerName[sizeof(_peerName) - 1] = 0;
      }
    }

  private:
    static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
    void handlePacket(const uint8_t *src, int8_t rssi, const uint8_t *data, int len);
    void queuePacket(const uint8_t *src, int8_t rssi, const uint8_t *data, int len);
    void notePeer(const uint8_t *mac);
    void updatePeerName(const char *name);

    QueueHandle_t _queue = nullptr;
    SemaphoreHandle_t _peerMutex = nullptr;
    uint8_t _peerMac[6] = {0};
    uint8_t _pendingMac[6] = {0};
    bool _hasPeer = false;
    bool _pendingPeer = false;
    uint32_t _lastPeerAddMs = 0;
    char _peerName[16] = {0};
    std::atomic<bool> _peerReady{false};
    std::atomic<uint32_t> _lastPeerMs{0};
    std::atomic<int8_t> _peerRssi{0};
    std::atomic<uint32_t> _dropped{0};

    static EspNowTransport *g_instance;
  };

  // Fix String comparison warning
  inline bool strEquals(const String &a, const char *b) { return a.equals(b); }

  // WiFi transport using UDP for local network communication
  class WiFiTransport : public Transport
  {
  public:
    WiFiTransport();
    ~WiFiTransport();

    bool begin() override;
    void end() override;

    bool send(const uint8_t *data, size_t len) override;
    bool sendTo(const uint8_t *addr, uint16_t port, const uint8_t *data,
                size_t len) override;
    bool broadcast(const uint8_t *data, size_t len) override;
    bool sendHello(const char *name, uint8_t battery, Mode mode) override;

    bool poll() override;

    bool hasPeer() const override;
    void peerName(char *out, size_t outSize) const override;
    int8_t peerRssi() const override { return _peerRssi.load(); }
    uint32_t droppedPackets() const override { return _dropped.load(); }

    bool popPacket(RxPacket &out, uint32_t timeoutMs = 0) override;

    const char *typeName() const override { return "WiFi"; }
    bool isLocalNetwork() const override { return true; }

    // WiFi configuration
    void setConfigCallback(ConfigCallback cb) override { _configCb = cb; }
    bool connectToWiFi(const char *ssid, const char *pass, uint32_t timeoutMs = 15000);
    void startConfigPortal(); // Starts AP + captive portal
    bool isConnected() const { return _wifiConnected; }
    void setHostname(const char *name);
    bool isWiFiConfigured() const;

    // mDNS service discovery
    void startMDNS(const char *hostname);
    void stopMDNS();

  private:
    static void udpRecvTask(void *arg);
    void processUDP();
    void sendDiscovery();
    void queuePacket(const IPAddress &srcIP, uint16_t port, int8_t rssi,
                     const uint8_t *data, int len);
    void handleDiscoveryResponse(const IPAddress &srcIP, uint16_t port, const uint8_t *data, int len);
    void notePeer(const IPAddress &srcIP, uint16_t port, int8_t rssi);
    void updatePeerName(const char *name);

    // WiFiManager-like configuration
    void runConfigPortal();
    static void configPortalTask(void *arg);
    bool loadSavedCredentials();
    void saveCredentials(const char *ssid, const char *pass);

    QueueHandle_t _queue = nullptr;
    SemaphoreHandle_t _peerMutex = nullptr;
    uint8_t _peerMac[6] = {0};
    uint16_t _peerPort = 0;
    bool _hasPeer = false;
    uint32_t _lastPeerMs = 0;
    char _peerName[16] = {0};
    std::atomic<int8_t> _peerRssi{0};
    std::atomic<uint32_t> _dropped{0};
    std::atomic<bool> _peerReady{false};

    WiFiUDP _udp;
    static constexpr uint16_t kIntercomPort = 42424;
    static constexpr uint32_t kDiscoveryIntervalMs = 2000;
    static constexpr uint32_t kPeerTimeoutMs = 5000;
    uint32_t _lastDiscoveryMs = 0;
    uint32_t _lastHelloMs = 0;

    bool _wifiConnected = false;
    bool _mdnsStarted = false;
    char _hostname[32] = {0};
    ConfigCallback _configCb = nullptr;

    TaskHandle_t _udpTask = nullptr;
    TaskHandle_t _configTask = nullptr;
    std::atomic<bool> _running{false};
    std::atomic<bool> _inConfigMode{false};

    String _savedSsid;
    String _savedPass;
  };

  // Web transport using Espressif's HTTPS server and HTTPD WebSocket support.
  class WebTransport : public Transport
  {
  public:
    WebTransport();
    ~WebTransport();

    bool begin() override;
    void end() override;

    bool send(const uint8_t *data, size_t len) override;
    bool sendTo(const uint8_t *addr, uint16_t port, const uint8_t *data,
                size_t len) override;
    bool broadcast(const uint8_t *data, size_t len) override;
    bool sendHello(const char *name, uint8_t battery, Mode mode) override;

    bool poll() override;

    bool hasPeer() const override;
    void peerName(char *out, size_t outSize) const override;
    int8_t peerRssi() const override { return _peerRssi.load(); }
    uint32_t droppedPackets() const override { return _dropped.load(); }

    bool popPacket(RxPacket &out, uint32_t timeoutMs = 0) override;

    const char *typeName() const override { return "Web"; }
    bool isLocalNetwork() const override { return true; }

    // Web configuration
    void setConfigCallback(ConfigCallback cb) override { _configCb = cb; }
    void setDeviceName(const char *name);

    // TransportManager integration
    void setTransportManager(TransportManager *mgr);

    // Web client management
    size_t connectedClients() const;
    void broadcastToWeb(const uint8_t *data, size_t len);
    bool sendToWebPeer(const uint8_t identity[6], const uint8_t *data,
                       size_t len);
    bool webClientIdentity(uint16_t clientPort, uint8_t identity[6]) const;
    bool audioTarget(uint16_t clientPort, uint8_t *addr, uint16_t &port, bool &viaWifi,
                     bool &webPeer, uint8_t *identity, bool &all) const;
    bool isRunning() const { return _running.load(); }
    void broadcastToWebExcept(uint16_t excludedPort, const uint8_t *data,
                              size_t len);

  private:
    static void webServerStartTask(void *arg);
    bool setupWebServer();
    void stopWebServer();
    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleAppJs(httpd_req_t *req);
    static esp_err_t handleStyleCss(httpd_req_t *req);
    static esp_err_t handleManifestJson(httpd_req_t *req);
    static esp_err_t handleWebSocket(httpd_req_t *req);
    esp_err_t serveFileFromFS(httpd_req_t *req, const char *path,
                              const char *contentType);
    esp_err_t handleWebSocketMessage(httpd_req_t *req, int socket,
                                     httpd_ws_frame_t *frame);
    bool registerWebClient(int socket, size_t &clientIndex);
    void queueWebSocketFrame(int socket, const uint8_t *data, size_t len,
                             httpd_ws_type_t type);
    void scheduleWebSocketSend();
    void clearWebSocketSendQueue();
    static void sendWebSocketWork(void *arg);
    void queueWebPacket(const uint8_t *src, uint16_t port, int8_t rssi,
                        const uint8_t *data, int len);
    void noteWebPeer(const uint8_t *mac, uint16_t port);
    void sendWebPacket(size_t clientIndex, const uint8_t *data, size_t len);
    void broadcastWebPacket(const uint8_t *data, size_t len);
    void broadcastWebPacketExcept(uint16_t excludedPort, const uint8_t *data,
                                  size_t len);

    QueueHandle_t _queue = nullptr;
    QueueHandle_t _sendQueue = nullptr;
    std::atomic<bool> _sendWorkScheduled{false};
    std::atomic<uint32_t> _lastSendQueueErrorMs{0};
    SemaphoreHandle_t _peerMutex = nullptr;
    uint8_t _peerMac[6] = {0};
    uint16_t _peerPort = 0;
    bool _hasPeer = false;
    uint32_t _lastPeerMs = 0;
    static constexpr uint32_t kPeerTimeoutMs = 5000;
    char _peerName[16] = {0};
    std::atomic<int8_t> _peerRssi{0};
    std::atomic<uint32_t> _dropped{0};
    std::atomic<bool> _peerReady{false};

    httpd_handle_t _httpsServer = nullptr;
    std::string _certificatePem;
    std::string _privateKeyPem;
    std::atomic<bool> _running{false};
    std::atomic<bool> _wifiConnected{false};
    std::atomic<bool> _serverStartInProgress{false};
    std::atomic<bool> _ending{false};
    char _deviceName[32] = {0};
    ConfigCallback _configCb = nullptr;
    bool _mdnsStarted = false;

    // Connected web clients (each client is a "peer")
    struct WebClient {
      int socket = -1;
      uint8_t mac[6] = {0};
      uint16_t port = 0;
      bool active = false;
      uint32_t lastSeen = 0;
      char name[16] = {0};
      uint8_t identity[6] = {0};
      uint8_t audioTargetMac[6] = {0};
      uint8_t audioTargetIdentity[6] = {0};
      uint16_t audioTargetPort = 0;
      bool audioTargetViaWifi = false;
      bool audioTargetWebPeer = false;
      bool audioTargetAll = true;
    };
    WebClient _webClients[kMaxWebClients];
    SemaphoreHandle_t _clientMutex = nullptr;
    uint32_t _lastServerAttemptMs = 0;

    static WebTransport *g_instance;

    // TransportManager pointer for cross-transport communication
    class TransportManager *_transportManager = nullptr;
  };

  // Transport manager - handles multiple transports, auto-selection, fallback
  class TransportManager
  {
  public:
    TransportManager() = default;
    ~TransportManager() = default;

    // Initialize with preferred transport order
    // Returns true if at least one transport started
    bool begin(const char *deviceName);

    void loop();

    // Get active transport (for sending)
    Transport *activeTransport() { return _activeTransport; }

    // Get transport by type
    EspNowTransport *espNow() { return &_espNow; }
    WiFiTransport *wifi() { return &_wifi; }
    WebTransport *web() { return &_web; }

    // Transport selection
    void setPreferredTransport(const char *type); // "espnow", "wifi", "web", "auto"
    const char *currentTransportType() const;

    // WiFi configuration
    void startWiFiConfig();
    bool isWiFiConfigured() const;
    const String& otaPassword() const { return _otaPassword; }

    // Callbacks
    using TransportChangeCallback = std::function<void(const char *type)>;
    void onTransportChange(TransportChangeCallback cb) { _transportChangeCb = cb; }

  private:
    void selectBestTransport();
    bool tryTransport(Transport *t);

    EspNowTransport _espNow;
    WiFiTransport _wifi;
    WebTransport _web;
    Transport *_activeTransport = nullptr;
    String _preferredTransport = "auto";
    TransportChangeCallback _transportChangeCb = nullptr;
    char _deviceName[32] = {0};
    String _otaPassword;
    bool _otaPasswordReady = false;
    bool _otaStarted = false;

    // Hardcoded WiFi credentials fallback (compile-time)
    static constexpr const char *kFallbackSSIDs[] = {
        // "HOME_WIFI",
        // "OFFICE_WIFI",
        nullptr};
    static constexpr const char *kFallbackPasses[] = {
        // "password123",
        // "officepass",
        nullptr};
  };

} // namespace intercom