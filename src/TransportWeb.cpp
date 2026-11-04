#include "Transport.h"

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform.h>
#include <mbedtls/x509_crt.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
  constexpr char kTlsPreferencesNamespace[] = "intercomtls";
  constexpr size_t kMaxWebSocketFrameSize = 250;
  constexpr UBaseType_t kWebSocketRxQueueDepth = 32;
  constexpr UBaseType_t kWebSocketSendQueueDepth = 24;
  constexpr UBaseType_t kWebSocketSendBatchSize = 12;
  constexpr uint32_t kSendQueueErrorLogIntervalMs = 5000;

  void *tlsCalloc(size_t count, size_t size)
  {
    return heap_caps_calloc_prefer(
        count, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  void tlsFree(void *memory)
  {
    heap_caps_free(memory);
  }

  bool configureTlsAllocator()
  {
    const int error = mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    if (error != 0)
    {
      Serial.printf("[web] ERROR: Could not configure TLS allocator (%d)\n",
                    error);
      return false;
    }
    return true;
  }

  struct CertificateContext
  {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_pk_context key;
    mbedtls_x509write_cert certificate;

    CertificateContext()
    {
      mbedtls_entropy_init(&entropy);
      mbedtls_ctr_drbg_init(&random);
      mbedtls_pk_init(&key);
      mbedtls_x509write_crt_init(&certificate);
    }

    ~CertificateContext()
    {
      mbedtls_x509write_crt_free(&certificate);
      mbedtls_pk_free(&key);
      mbedtls_ctr_drbg_free(&random);
      mbedtls_entropy_free(&entropy);
    }
  };

  struct WebSocketSendWork
  {
    httpd_handle_t server;
    int socket;
    httpd_ws_type_t type;
    size_t length;
    uint8_t payload[1];
  };

  String extractJsonString(const char *json, const char *key)
  {
    char quotedKey[24];
    snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
    const char *value = strstr(json, quotedKey);
    if (!value) return "";

    const char *colon = strchr(value, ':');
    const char *start = colon ? strchr(colon, '"') : nullptr;
    if (!start) return "";
    ++start;

    const char *end = strchr(start, '"');
    return end ? String(start, end - start) : String();
  }

  bool extractJsonBool(const char *json, const char *key)
  {
    char quotedKey[24];
    snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
    const char *value = strstr(json, quotedKey);
    if (!value) return false;
    const char *colon = strchr(value, ':');
    if (!colon) return false;
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    return strncmp(value, "true", 4) == 0;
  }

  bool extractJsonUint16(const char *json, const char *key, uint16_t &result)
  {
    char quotedKey[24];
    snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
    const char *value = strstr(json, quotedKey);
    if (!value) return false;
    const char *colon = strchr(value, ':');
    if (!colon) return false;
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    if (*value < '0' || *value > '9') return false;
    char *end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (!end || parsed > UINT16_MAX) return false;
    result = static_cast<uint16_t>(parsed);
    return true;
  }

  bool parseMacAddress(const char *text, uint8_t *addr)
  {
    unsigned int bytes[6];
    char trailing;
    if (!text || !addr ||
        sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &bytes[0], &bytes[1],
               &bytes[2], &bytes[3], &bytes[4], &bytes[5], &trailing) != 6)
    {
      return false;
    }
    for (size_t i = 0; i < 6; ++i)
    {
      if (bytes[i] > 0xFF) return false;
      addr[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
  }

  bool parseAudioTarget(const char *text, uint8_t *addr, uint16_t &port)
  {
    if (!text || !addr) return false;
    const char *separator = strchr(text, '/');
    if (!separator) return false;

    char mac[18];
    const size_t macLength = separator - text;
    if (macLength != 17) return false;
    memcpy(mac, text, macLength);
    mac[macLength] = 0;
    if (!parseMacAddress(mac, addr)) return false;

    char *end = nullptr;
    const unsigned long parsedPort = strtoul(separator + 1, &end, 10);
    if (!end || end == separator + 1 || *end != 0 || parsedPort > 65535)
      return false;
    port = static_cast<uint16_t>(parsedPort);
    return true;
  }

  bool generateCertificate(const std::string &hostname, const IPAddress &ip,
                           std::string &certificatePem,
                           std::string &privateKeyPem, int &error)
  {
    CertificateContext ctx;
    static const unsigned char personalization[] = "Intercom HTTPS server";
    error = mbedtls_ctr_drbg_seed(
        &ctx.random, mbedtls_entropy_func, &ctx.entropy, personalization,
        sizeof(personalization) - 1);
    if (error != 0) return false;

    error = mbedtls_pk_setup(
        &ctx.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (error != 0) return false;

    error = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ctx.key),
                                mbedtls_ctr_drbg_random, &ctx.random);
    if (error != 0) return false;

    mbedtls_x509write_crt_set_version(&ctx.certificate,
                                      MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&ctx.certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&ctx.certificate, &ctx.key);
    mbedtls_x509write_crt_set_issuer_key(&ctx.certificate, &ctx.key);

    const std::string distinguishedName =
        "CN=" + hostname + ",O=Intercom,C=US";
    error = mbedtls_x509write_crt_set_subject_name(
        &ctx.certificate, distinguishedName.c_str());
    if (error != 0) return false;
    error = mbedtls_x509write_crt_set_issuer_name(
        &ctx.certificate, distinguishedName.c_str());
    if (error != 0) return false;
    error = mbedtls_x509write_crt_set_validity(
        &ctx.certificate, "20260101000000", "20360101000000");
    if (error != 0) return false;
    error = mbedtls_x509write_crt_set_basic_constraints(
        &ctx.certificate, 0, -1);
    if (error != 0) return false;
    error = mbedtls_x509write_crt_set_key_usage(
        &ctx.certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (error != 0) return false;

    std::array<unsigned char, 128> subjectAltName{};
    const bool hasIpAddress = ip != IPAddress(0, 0, 0, 0);
    const size_t dnsLength = hostname.length();
    const size_t sequenceLength = dnsLength + 2 + (hasIpAddress ? 6 : 0);
    if (dnsLength == 0 || sequenceLength >= 128)
    {
      error = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
      return false;
    }

    size_t sanLength = 0;
    subjectAltName[sanLength++] = 0x30;
    subjectAltName[sanLength++] = (unsigned char)sequenceLength;
    subjectAltName[sanLength++] = 0x82;
    subjectAltName[sanLength++] = (unsigned char)dnsLength;
    memcpy(subjectAltName.data() + sanLength, hostname.data(), dnsLength);
    sanLength += dnsLength;
    if (hasIpAddress)
    {
      subjectAltName[sanLength++] = 0x87;
      subjectAltName[sanLength++] = 4;
      for (size_t i = 0; i < 4; ++i)
      {
        subjectAltName[sanLength++] = ip[i];
      }
    }
    error = mbedtls_x509write_crt_set_extension(
        &ctx.certificate, MBEDTLS_OID_SUBJECT_ALT_NAME,
        MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0,
        subjectAltName.data(), sanLength);
    if (error != 0) return false;

    std::array<unsigned char, 16> serialBytes{};
    error = mbedtls_ctr_drbg_random(&ctx.random, serialBytes.data(),
                                    serialBytes.size());
    if (error != 0) return false;
    serialBytes[0] &= 0x7F;
    serialBytes[0] |= 0x01;
    error = mbedtls_x509write_crt_set_serial_raw(
        &ctx.certificate, serialBytes.data(), serialBytes.size());
    if (error != 0) return false;

    std::array<unsigned char, 2048> certificateBuffer{};
    std::array<unsigned char, 1024> privateKeyBuffer{};
    error = mbedtls_x509write_crt_pem(
        &ctx.certificate, certificateBuffer.data(), certificateBuffer.size(),
        mbedtls_ctr_drbg_random, &ctx.random);
    if (error != 0) return false;
    error = mbedtls_pk_write_key_pem(&ctx.key, privateKeyBuffer.data(),
                                     privateKeyBuffer.size());
    if (error != 0) return false;

    certificatePem.assign(
        reinterpret_cast<const char *>(certificateBuffer.data()));
    privateKeyPem.assign(
        reinterpret_cast<const char *>(privateKeyBuffer.data()));
    return true;
  }

  bool loadOrCreateCertificate(const std::string &hostname, const IPAddress &ip,
                               std::string &certificatePem,
                               std::string &privateKeyPem)
  {
    Preferences preferences;
    if (!preferences.begin(kTlsPreferencesNamespace, false))
    {
      Serial.println("[web] ERROR: Could not open TLS certificate storage");
      return false;
    }

    const std::string identity = hostname + "|" + ip.toString().c_str();
    const String savedIdentity = preferences.getString("identity", "");
    const String savedCertificate = preferences.getString("certificate", "");
    const String savedPrivateKey = preferences.getString("private_key", "");
    if (savedIdentity.equals(identity.c_str()) && savedCertificate.length() &&
        savedPrivateKey.length())
    {
      certificatePem.assign(savedCertificate.c_str());
      privateKeyPem.assign(savedPrivateKey.c_str());
      preferences.end();
      return true;
    }

    int error = 0;
    if (!generateCertificate(hostname, ip, certificatePem, privateKeyPem,
                             error))
    {
      Serial.printf("[web] ERROR: Self-signed certificate generation failed (%d)\n",
                    error);
      preferences.end();
      return false;
    }

    const bool saved =
        preferences.putString("identity", identity.c_str()) > 0 &&
        preferences.putString("certificate", certificatePem.c_str()) > 0 &&
        preferences.putString("private_key", privateKeyPem.c_str()) > 0;
    preferences.end();
    if (!saved)
    {
      Serial.println("[web] ERROR: Could not persist TLS certificate in NVS");
      return false;
    }
    return true;
  }
}

namespace intercom
{
  WebTransport *WebTransport::g_instance = nullptr;

  WebTransport::WebTransport()
  {
    _clientMutex = xSemaphoreCreateMutex();
    g_instance = this;
  }

  WebTransport::~WebTransport()
  {
    end();
    if (g_instance == this) g_instance = nullptr;
  }

  bool WebTransport::begin()
  {
    if (!_queue) _queue = xQueueCreate(kWebSocketRxQueueDepth,
                                       sizeof(RxPacket));
    if (!_sendQueue)
      _sendQueue = xQueueCreate(kWebSocketSendQueueDepth,
                                sizeof(WebSocketSendWork *));
    if (!_peerMutex) _peerMutex = xSemaphoreCreateMutex();
    if (!_clientMutex) _clientMutex = xSemaphoreCreateMutex();
    if (!_queue || !_sendQueue || !_peerMutex || !_clientMutex)
    {
      Serial.println("[web] ERROR: Could not allocate web transport resources");
      return false;
    }

    _ending = false;
    _wifiConnected = WiFi.status() == WL_CONNECTED;
    _lastServerAttemptMs = 0;
    return true;
  }

  void WebTransport::end()
  {
    _ending = true;
    while (_serverStartInProgress)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    _running = false;
    _wifiConnected = false;
    stopWebServer();

    if (_mdnsStarted)
    {
      MDNS.end();
      _mdnsStarted = false;
    }
    if (_queue)
    {
      vQueueDelete(_queue);
      _queue = nullptr;
    }
    if (_sendQueue)
    {
      clearWebSocketSendQueue();
      vQueueDelete(_sendQueue);
      _sendQueue = nullptr;
    }
    if (_peerMutex)
    {
      vSemaphoreDelete(_peerMutex);
      _peerMutex = nullptr;
    }
    if (_clientMutex)
    {
      vSemaphoreDelete(_clientMutex);
      _clientMutex = nullptr;
    }
    _hasPeer = false;
    _peerReady = false;
  }

  void WebTransport::stopWebServer()
  {
    if (_httpsServer)
    {
      const esp_err_t error = httpd_ssl_stop(_httpsServer);
      if (error != ESP_OK)
      {
        Serial.printf("[web] ERROR: HTTPS server stop failed: %s\n",
                      esp_err_to_name(error));
      }
      _httpsServer = nullptr;
    }
    clearWebSocketSendQueue();

    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      for (size_t i = 0; i < kMaxWebClients; ++i)
      {
        _webClients[i] = WebClient{};
      }
      xSemaphoreGive(_clientMutex);
    }
    _certificatePem.clear();
    _privateKeyPem.clear();
  }

  void WebTransport::webServerStartTask(void *arg)
  {
    auto *transport = static_cast<WebTransport *>(arg);
    bool started = transport->setupWebServer();
    const wifi_mode_t wifiMode = WiFi.getMode();
    const bool wifiConnected =
        WiFi.status() == WL_CONNECTED &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0) &&
        wifiMode != WIFI_AP && wifiMode != WIFI_AP_STA;

    if (started && !transport->_ending && wifiConnected)
    {
      transport->_running = true;
      if (!transport->_mdnsStarted)
      {
        char hostname[64];
        snprintf(hostname, sizeof(hostname), "%s-web",
                 transport->_deviceName[0] ? transport->_deviceName : "intercom");
        if (MDNS.begin(hostname))
        {
          MDNS.addService("https", "tcp", 443);
          transport->_mdnsStarted = true;
          Serial.printf("[web] mDNS available at https://%s.local/\n",
                        hostname);
        }
        else
        {
          Serial.printf("[web] ERROR: mDNS start failed for %s\n", hostname);
        }
      }
    }
    else if (started)
    {
      transport->stopWebServer();
    }

    transport->_serverStartInProgress = false;
    vTaskDelete(nullptr);
  }

  void WebTransport::setDeviceName(const char *name)
  {
    if (!name) return;
    strncpy(_deviceName, name, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = 0;
  }

  bool WebTransport::setupWebServer()
  {
    if (_httpsServer) return true;

    if (!configureTlsAllocator()) return false;
    Serial.printf(
        "[web] TLS heap before startup (internal free=%u, largest=%u, "
        "PSRAM free=%u)\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                          MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    char hostname[64];
    snprintf(hostname, sizeof(hostname), "%s-web.local",
             _deviceName[0] ? _deviceName : "intercom");
    const IPAddress localIp = WiFi.localIP();
    if (!loadOrCreateCertificate(hostname, localIp, _certificatePem,
                                  _privateKeyPem))
    {
      return false;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    // HTTPD reserves three sockets internally in addition to this client limit.
    config.httpd.max_open_sockets = kMaxWebClients + 3;
    config.httpd.max_uri_handlers = 5;
    config.httpd.lru_purge_enable = false;
    config.servercert =
        reinterpret_cast<const uint8_t *>(_certificatePem.c_str());
    config.servercert_len = _certificatePem.length() + 1;
    config.prvtkey_pem =
        reinterpret_cast<const uint8_t *>(_privateKeyPem.c_str());
    config.prvtkey_len = _privateKeyPem.length() + 1;
    config.port_secure = 443;

    esp_err_t error = httpd_ssl_start(&_httpsServer, &config);
    if (error != ESP_OK)
    {
      _httpsServer = nullptr;
      Serial.printf("[web] ERROR: HTTPS server start failed: %s\n",
                    esp_err_to_name(error));
      return false;
    }
    Serial.printf(
        "[web] HTTPS ready (sockets=%u, internal free=%u, largest=%u, "
        "PSRAM free=%u)\n",
        (unsigned)config.httpd.max_open_sockets,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const auto registerUri = [this](const char *path, httpd_method_t method,
                                    esp_err_t (*handler)(httpd_req_t *),
                                    bool websocket = false) {
      httpd_uri_t uri = {};
      uri.uri = path;
      uri.method = method;
      uri.handler = handler;
      uri.user_ctx = this;
      uri.is_websocket = websocket;
      return httpd_register_uri_handler(_httpsServer, &uri);
    };

    const bool registered =
        registerUri("/", HTTP_GET, handleRoot) == ESP_OK &&
        registerUri("/app.js", HTTP_GET, handleAppJs) == ESP_OK &&
        registerUri("/style.css", HTTP_GET, handleStyleCss) == ESP_OK &&
        registerUri("/manifest.json", HTTP_GET, handleManifestJson) == ESP_OK &&
        registerUri("/ws", HTTP_GET, handleWebSocket, true) == ESP_OK;
    if (!registered)
    {
      Serial.println("[web] ERROR: Failed to register HTTPS routes");
      stopWebServer();
      return false;
    }

    Serial.printf("[web] HTTPS server listening at https://%s/\n", hostname);
    return true;
  }

  esp_err_t WebTransport::handleRoot(httpd_req_t *req)
  {
    return static_cast<WebTransport *>(req->user_ctx)->serveFileFromFS(
        req, "/index.html", "text/html; charset=utf-8");
  }

  esp_err_t WebTransport::handleAppJs(httpd_req_t *req)
  {
    return static_cast<WebTransport *>(req->user_ctx)->serveFileFromFS(
        req, "/app.js", "application/javascript; charset=utf-8");
  }

  esp_err_t WebTransport::handleStyleCss(httpd_req_t *req)
  {
    return static_cast<WebTransport *>(req->user_ctx)->serveFileFromFS(
        req, "/style.css", "text/css; charset=utf-8");
  }

  esp_err_t WebTransport::handleManifestJson(httpd_req_t *req)
  {
    return static_cast<WebTransport *>(req->user_ctx)->serveFileFromFS(
        req, "/manifest.json", "application/manifest+json");
  }

  esp_err_t WebTransport::serveFileFromFS(httpd_req_t *req, const char *path,
                                           const char *contentType)
  {
    File file;
    if (LittleFS.exists(path))
    {
      file = LittleFS.open(path, "r");
    }
    else if (SPIFFS.exists(path))
    {
      file = SPIFFS.open(path, "r");
    }
    if (!file)
    {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
      return ESP_OK;
    }

    httpd_resp_set_type(req, contentType);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint8_t buffer[1024];
    while (true)
    {
      const size_t length = file.read(buffer, sizeof(buffer));
      if (length == 0) break;
      const esp_err_t error = httpd_resp_send_chunk(
          req, reinterpret_cast<const char *>(buffer), length);
      if (error != ESP_OK)
      {
        file.close();
        return error;
      }
    }
    file.close();
    return httpd_resp_send_chunk(req, nullptr, 0);
  }

  bool WebTransport::registerWebClient(int socket, size_t &clientIndex)
  {
    if (!_clientMutex ||
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
      return false;
    }

    bool registered = false;
    for (size_t i = 0; i < kMaxWebClients; ++i)
    {
      if (_webClients[i].active && _webClients[i].socket == socket)
      {
        clientIndex = i;
        registered = true;
        break;
      }
      if (_webClients[i].active &&
          httpd_ws_get_fd_info(_httpsServer, _webClients[i].socket) !=
              HTTPD_WS_CLIENT_WEBSOCKET)
      {
        _webClients[i] = WebClient{};
      }
      if (_webClients[i].active) continue;
      WebClient &client = _webClients[i];
      client = WebClient{};
      client.socket = socket;
      client.active = true;
      client.port = kWebClientPort + i;
      client.lastSeen = millis();
      client.mac[0] = 0x02;
      client.mac[4] = (uint8_t)(i >> 8);
      client.mac[5] = (uint8_t)(i + 1);
      clientIndex = i;
      registered = true;
      break;
    }
    xSemaphoreGive(_clientMutex);
    if (registered)
    {
      Serial.printf(
          "[web] WebSocket client connected (socket %d, internal free=%u, "
          "largest=%u)\n",
          socket,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                            MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                     MALLOC_CAP_8BIT));
    }
    else
    {
      Serial.printf("[web] ERROR: No client slot for socket %d\n", socket);
    }
    return registered;
  }

  esp_err_t WebTransport::handleWebSocket(httpd_req_t *req)
  {
    auto *transport = static_cast<WebTransport *>(req->user_ctx);
    const int socket = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET)
    {
      size_t clientIndex = 0;
      return transport->registerWebClient(socket, clientIndex) ? ESP_OK
                                                               : ESP_FAIL;
    }

    httpd_ws_frame_t frame = {};
    esp_err_t error = httpd_ws_recv_frame(req, &frame, 0);
    if (error != ESP_OK) return error;
    if (frame.len > kMaxWebSocketFrameSize) return ESP_ERR_INVALID_SIZE;

    std::array<uint8_t, kMaxWebSocketFrameSize> payload{};
    frame.payload = payload.data();
    if (frame.len > 0)
    {
      error = httpd_ws_recv_frame(req, &frame, frame.len);
      if (error != ESP_OK) return error;
    }
    return transport->handleWebSocketMessage(req, socket, &frame);
  }

  esp_err_t WebTransport::handleWebSocketMessage(httpd_req_t *req, int socket,
                                                   httpd_ws_frame_t *frame)
  {
    size_t clientIndex = kMaxWebClients;
    uint8_t clientMac[6] = {};
    uint16_t clientPort = 0;
    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      for (size_t i = 0; i < kMaxWebClients; ++i)
      {
        if (_webClients[i].active && _webClients[i].socket == socket)
        {
          clientIndex = i;
          memcpy(clientMac, _webClients[i].mac, sizeof(clientMac));
          clientPort = _webClients[i].port;
          _webClients[i].lastSeen = millis();
          break;
        }
      }
      xSemaphoreGive(_clientMutex);
    }
    if (clientIndex == kMaxWebClients)
    {
      if (!registerWebClient(socket, clientIndex))
      {
        return ESP_FAIL;
      }
      Serial.printf("[web] Recovered WebSocket client registration (socket %d)\n",
                    socket);
    }
    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      if (clientIndex < kMaxWebClients &&
          _webClients[clientIndex].active &&
          _webClients[clientIndex].socket == socket)
      {
        memcpy(clientMac, _webClients[clientIndex].mac, sizeof(clientMac));
        clientPort = _webClients[clientIndex].port;
      }
      xSemaphoreGive(_clientMutex);
    }
    if (clientPort == 0) return ESP_FAIL;

    if (frame->type == HTTPD_WS_TYPE_TEXT)
    {
      const std::string message(reinterpret_cast<const char *>(frame->payload),
                                frame->len);
      const String command = extractJsonString(message.c_str(), "cmd");
      if (command == "hello")
      {
        String name = extractJsonString(message.c_str(), "name");
        if (name.length() == 0) name = "WebClient";
        if (_clientMutex &&
            xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
          strncpy(_webClients[clientIndex].name, name.c_str(),
                  sizeof(_webClients[clientIndex].name) - 1);
          _webClients[clientIndex].name[
              sizeof(_webClients[clientIndex].name) - 1] = 0;
          xSemaphoreGive(_clientMutex);
        }

        uint8_t packet[sizeof(PacketHeader) + sizeof(HelloPayload)] = {};
        PacketHeader *header = reinterpret_cast<PacketHeader *>(packet);
        HelloPayload *hello =
            reinterpret_cast<HelloPayload *>(packet + sizeof(PacketHeader));
        header->magic = kMagic;
        header->version = kVersion;
        header->type = kPacketHello;
        header->payloadLen = sizeof(HelloPayload);
        hello->battery = 0xFF;
        hello->mode = static_cast<uint8_t>(Mode::Live);
        strncpy(hello->name, name.c_str(), sizeof(hello->name) - 1);
        noteWebPeer(clientMac, clientPort);
        queueWebPacket(clientMac, clientPort, 0, packet, sizeof(packet));
        Serial.printf("[web] Received peer hello from %s (port %u)\n",
                      name.c_str(), clientPort);
      }
      else if (command == "ping")
      {
        uint8_t response[] = "{\"type\":\"pong\"}";
        httpd_ws_frame_t pong = {};
        pong.type = HTTPD_WS_TYPE_TEXT;
        pong.payload = response;
        pong.len = sizeof(response) - 1;
        return httpd_ws_send_frame(req, &pong);
      }
      else if (command == "target")
      {
        const String target = extractJsonString(message.c_str(), "id");
        uint8_t targetMac[6] = {};
        uint16_t targetPort = 0;
        uint16_t parsedPort = 0;
        const bool targetWebPeer =
            extractJsonBool(message.c_str(), "webPeer");
        const bool targetAll = target == "all";
        bool targetValid = targetAll;
        if (!targetAll)
        {
          const String targetAddress =
              extractJsonString(message.c_str(), "addr");
          if (targetAddress.length() > 0 &&
              parseMacAddress(targetAddress.c_str(), targetMac) &&
              extractJsonUint16(message.c_str(), "port", parsedPort))
          {
            targetPort = parsedPort;
            targetValid = true;
          }
          else
          {
            targetValid =
                parseAudioTarget(target.c_str(), targetMac, targetPort);
          }
        }
        if (!targetValid)
        {
          Serial.println("[web] Ignored invalid audio target");
          return ESP_OK;
        }
        if (_clientMutex &&
            xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
          WebClient &client = _webClients[clientIndex];
          if (client.active && client.port == clientPort)
          {
            client.audioTargetAll = targetAll;
            if (!targetAll)
            {
              memcpy(client.audioTargetMac, targetMac,
                     sizeof(client.audioTargetMac));
              client.audioTargetPort = targetPort;
              client.audioTargetViaWifi =
                  extractJsonBool(message.c_str(), "viaWifi");
              client.audioTargetWebPeer = targetWebPeer;
            }
          }
          xSemaphoreGive(_clientMutex);
        }
      }
      else if (command == "spy")
      {
        uint16_t spyCommand = 0;
        uint16_t targetPort = 0;
        uint8_t targetAddr[6] = {};
        const String targetAddress =
            extractJsonString(message.c_str(), "addr");
        if (!extractJsonUint16(message.c_str(), "action", spyCommand) ||
            spyCommand < kSpyLiveStart || spyCommand > kSpyClipStop ||
            !parseMacAddress(targetAddress.c_str(), targetAddr) ||
            !extractJsonUint16(message.c_str(), "port", targetPort))
        {
          Serial.println("[web] Ignored invalid spy target");
          return ESP_OK;
        }

        uint8_t packet[sizeof(PacketHeader) + sizeof(SpyTargetPayload)] = {};
        PacketHeader *header = reinterpret_cast<PacketHeader *>(packet);
        SpyTargetPayload *target =
            reinterpret_cast<SpyTargetPayload *>(packet + sizeof(PacketHeader));
        header->magic = kMagic;
        header->version = kVersion;
        header->type = kPacketSpy;
        header->payloadLen = sizeof(SpyTargetPayload);
        target->command = static_cast<uint8_t>(spyCommand);
        memcpy(target->addr, targetAddr, sizeof(targetAddr));
        target->port = targetPort;
        target->flags = extractJsonBool(message.c_str(), "webPeer")
                            ? kSpyTargetWebPeer
                            : 0;
        queueWebPacket(clientMac, clientPort, 0, packet, sizeof(packet));
      }
      return ESP_OK;
    }

    if (frame->type == HTTPD_WS_TYPE_BINARY &&
        frame->len >= sizeof(PacketHeader))
    {
      const PacketHeader *header =
          reinterpret_cast<const PacketHeader *>(frame->payload);
      if (header->magic == kMagic && header->version == kVersion &&
          header->payloadLen <= frame->len - sizeof(PacketHeader))
      {
        queueWebPacket(clientMac, clientPort, 0, frame->payload, frame->len);
      }
    }
    return ESP_OK;
  }

  bool WebTransport::send(const uint8_t *data, size_t len)
  {
    if (!data || len == 0 || len > kMaxPacketSize || !_running) return false;
    broadcastWebPacket(data, len);
    return true;
  }

  bool WebTransport::sendTo(const uint8_t *addr, uint16_t port,
                            const uint8_t *data, size_t len)
  {
    if (!addr || !data || len == 0 || len > kMaxPacketSize || !_running)
      return false;

    size_t clientIndex = kMaxWebClients;
    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      for (size_t i = 0; i < kMaxWebClients; ++i)
      {
        if (_webClients[i].active && _webClients[i].port == port)
        {
          clientIndex = i;
          break;
        }
      }
      xSemaphoreGive(_clientMutex);
    }
    if (clientIndex == kMaxWebClients) return false;
    sendWebPacket(clientIndex, data, len);
    return true;
  }

  bool WebTransport::broadcast(const uint8_t *data, size_t len)
  {
    if (!data || len == 0 || len > kMaxPacketSize || !_running) return false;
    broadcastWebPacket(data, len);
    return true;
  }

  bool WebTransport::sendHello(const char *name, uint8_t battery, Mode mode)
  {
    if (!_running) return false;

    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    char macString[18];
    snprintf(macString, sizeof(macString), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char peerId[24];
    snprintf(peerId, sizeof(peerId), "esp:%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char message[220];
    snprintf(message, sizeof(message),
             "{\"type\":\"hello\",\"id\":\"%s\",\"mac\":\"%s\","
             "\"port\":0,\"viaWifi\":false,\"webPeer\":false,"
             "\"name\":\"%s\",\"battery\":%u,\"mode\":%u}",
             peerId, macString, name ? name : "intercom", (unsigned)battery,
             (unsigned)mode);
    broadcastWebPacket(reinterpret_cast<const uint8_t *>(message),
                       strlen(message));
    return true;
  }

  void WebTransport::queueWebSocketFrame(int socket, const uint8_t *data,
                                          size_t len, httpd_ws_type_t type)
  {
    if (!_running || !_httpsServer || !_sendQueue || !data || len == 0)
      return;
    const size_t allocationSize = sizeof(WebSocketSendWork) + len - 1;
    auto *work = static_cast<WebSocketSendWork *>(malloc(allocationSize));
    if (!work)
    {
      _dropped.fetch_add(1);
      Serial.println("[web] ERROR: Could not allocate WebSocket send buffer");
      return;
    }

    work->server = _httpsServer;
    work->socket = socket;
    work->type = type;
    work->length = len;
    memcpy(work->payload, data, len);
    if (xQueueSend(_sendQueue, &work, 0) != pdTRUE)
    {
      free(work);
      _dropped.fetch_add(1);
      const uint32_t now = millis();
      uint32_t last = _lastSendQueueErrorMs.load();
      if (now - last >= kSendQueueErrorLogIntervalMs &&
          _lastSendQueueErrorMs.compare_exchange_strong(last, now))
      {
        Serial.println("[web] WARNING: WebSocket send queue is full");
      }
      return;
    }
    scheduleWebSocketSend();
  }

  void WebTransport::scheduleWebSocketSend()
  {
    if (!_running || !_httpsServer || !_sendQueue ||
        uxQueueMessagesWaiting(_sendQueue) == 0)
    {
      return;
    }

    bool expected = false;
    if (!_sendWorkScheduled.compare_exchange_strong(expected, true)) return;
    const esp_err_t error =
        httpd_queue_work(_httpsServer, sendWebSocketWork, this);
    if (error == ESP_OK) return;

    _sendWorkScheduled.store(false);
    const uint32_t now = millis();
    uint32_t last = _lastSendQueueErrorMs.load();
    if (now - last >= kSendQueueErrorLogIntervalMs &&
        _lastSendQueueErrorMs.compare_exchange_strong(last, now))
    {
      Serial.printf("[web] WARNING: Could not schedule WebSocket send: %s\n",
                    esp_err_to_name(error));
    }
  }

  void WebTransport::sendWebSocketWork(void *arg)
  {
    auto *transport = static_cast<WebTransport *>(arg);
    WebSocketSendWork *work = nullptr;
    UBaseType_t sent = 0;
    while (sent < kWebSocketSendBatchSize && transport->_sendQueue &&
           xQueueReceive(transport->_sendQueue, &work, 0) == pdTRUE)
    {
      if (work->server == transport->_httpsServer)
      {
        httpd_ws_frame_t frame = {};
        frame.type = work->type;
        frame.payload = work->payload;
        frame.len = work->length;
        const esp_err_t error =
            httpd_ws_send_frame_async(work->server, work->socket, &frame);
        if (error != ESP_OK) transport->_dropped.fetch_add(1);
      }
      free(work);
      ++sent;
    }

    transport->_sendWorkScheduled.store(false);
    transport->scheduleWebSocketSend();
  }

  void WebTransport::clearWebSocketSendQueue()
  {
    if (!_sendQueue) return;
    WebSocketSendWork *work = nullptr;
    while (xQueueReceive(_sendQueue, &work, 0) == pdTRUE) free(work);
    _sendWorkScheduled.store(false);
  }

  void WebTransport::sendWebPacket(size_t clientIndex, const uint8_t *data,
                                   size_t len)
  {
    if (!data || len == 0 || clientIndex >= kMaxWebClients) return;
    int socket = -1;
    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
      if (_webClients[clientIndex].active)
        socket = _webClients[clientIndex].socket;
      xSemaphoreGive(_clientMutex);
    }
    if (socket >= 0)
      queueWebSocketFrame(socket, data, len, HTTPD_WS_TYPE_BINARY);
  }

  void WebTransport::broadcastWebPacket(const uint8_t *data, size_t len)
  {
    if (!data || len == 0 || !_clientMutex ||
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
      return;
    }

    int sockets[kMaxWebClients];
    size_t count = 0;
    for (size_t i = 0; i < kMaxWebClients; ++i)
    {
      if (_webClients[i].active) sockets[count++] = _webClients[i].socket;
    }
    xSemaphoreGive(_clientMutex);

    const bool isText = len > 0 && data[0] == '{';
    for (size_t i = 0; i < count; ++i)
    {
      queueWebSocketFrame(sockets[i], data, len,
                          isText ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY);
    }
  }

  void WebTransport::broadcastWebPacketExcept(uint16_t excludedPort,
                                               const uint8_t *data,
                                               size_t len)
  {
    if (!data || len == 0 || !_clientMutex ||
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    {
      return;
    }

    int sockets[kMaxWebClients];
    size_t count = 0;
    for (size_t i = 0; i < kMaxWebClients; ++i)
    {
      if (_webClients[i].active && _webClients[i].port != excludedPort)
        sockets[count++] = _webClients[i].socket;
    }
    xSemaphoreGive(_clientMutex);

    const bool isText = data[0] == '{';
    for (size_t i = 0; i < count; ++i)
    {
      queueWebSocketFrame(sockets[i], data, len,
                          isText ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY);
    }
  }

  void WebTransport::queueWebPacket(const uint8_t *src, uint16_t port,
                                    int8_t rssi, const uint8_t *data, int len)
  {
    if (!_queue || !src || !data || len <= 0 ||
        len > (int)kMaxPacketSize)
      return;

    RxPacket packet{};
    memcpy(packet.src, src, sizeof(packet.src));
    packet.rssi = rssi;
    packet.port = port;
    packet.len = (uint16_t)len;
    memcpy(packet.data, data, len);
    if (xQueueSend(_queue, &packet, 0) != pdTRUE) _dropped.fetch_add(1);
  }

  void WebTransport::noteWebPeer(const uint8_t *mac, uint16_t port)
  {
    if (!mac || !_peerMutex ||
        xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
      return;

    memcpy(_peerMac, mac, sizeof(_peerMac));
    _peerPort = port;
    _hasPeer = true;
    _peerReady.store(true);
    _lastPeerMs = millis();
    xSemaphoreGive(_peerMutex);
  }

  bool WebTransport::poll()
  {
    if (_ending) return false;

    const uint32_t now = millis();
    scheduleWebSocketSend();
    const wifi_mode_t wifiMode = WiFi.getMode();
    const bool wifiConnected =
        WiFi.status() == WL_CONNECTED &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0) &&
        wifiMode != WIFI_AP && wifiMode != WIFI_AP_STA;

    if (!wifiConnected)
    {
      _wifiConnected = false;
      if (!_serverStartInProgress)
      {
        if (_running.exchange(false) || _httpsServer) stopWebServer();
        if (_mdnsStarted)
        {
          MDNS.end();
          _mdnsStarted = false;
        }
      }
      return false;
    }

    _wifiConnected = true;
    if (!_running && !_serverStartInProgress &&
        (_lastServerAttemptMs == 0 || now - _lastServerAttemptMs >= 5000))
    {
      _lastServerAttemptMs = now;
      bool expected = false;
      if (_serverStartInProgress.compare_exchange_strong(expected, true))
      {
        const BaseType_t taskCreated = xTaskCreatePinnedToCore(
            webServerStartTask, "web_https_start", 16384, this, 3, nullptr, 1);
        if (taskCreated != pdPASS)
        {
          _serverStartInProgress = false;
          Serial.println("[web] ERROR: Could not create HTTPS startup task");
        }
      }
    }

    if (_httpsServer && _clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      for (size_t i = 0; i < kMaxWebClients; ++i)
      {
        if (_webClients[i].active &&
            httpd_ws_get_fd_info(_httpsServer, _webClients[i].socket) !=
                HTTPD_WS_CLIENT_WEBSOCKET)
        {
          _webClients[i] = WebClient{};
        }
      }
      xSemaphoreGive(_clientMutex);
    }

    return _running && hasPeer();
  }

  bool WebTransport::hasPeer() const
  {
    if (_clientMutex &&
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      for (size_t i = 0; i < kMaxWebClients; ++i)
      {
        if (_webClients[i].active)
        {
          xSemaphoreGive(_clientMutex);
          return true;
        }
      }
      xSemaphoreGive(_clientMutex);
    }

    if (!_peerMutex ||
        xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
      return false;
    const bool online = _hasPeer &&
                        millis() - _lastPeerMs < kPeerTimeoutMs;
    xSemaphoreGive(_peerMutex);
    return online;
  }

  void WebTransport::peerName(char *out, size_t outSize) const
  {
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!_peerMutex ||
        xSemaphoreTake(_peerMutex, pdMS_TO_TICKS(10)) != pdTRUE)
      return;
    strncpy(out, _peerName, outSize - 1);
    out[outSize - 1] = 0;
    xSemaphoreGive(_peerMutex);
  }

  bool WebTransport::popPacket(RxPacket &out, uint32_t timeoutMs)
  {
    if (!_queue) return false;
    return xQueueReceive(_queue, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }

  size_t WebTransport::connectedClients() const
  {
    if (!_clientMutex ||
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(10)) != pdTRUE)
      return 0;

    size_t count = 0;
    for (size_t i = 0; i < kMaxWebClients; ++i)
    {
      if (_webClients[i].active) ++count;
    }
    xSemaphoreGive(_clientMutex);
    return count;
  }

  void WebTransport::broadcastToWeb(const uint8_t *data, size_t len)
  {
    if (data && len) broadcastWebPacket(data, len);
  }

  void WebTransport::broadcastToWebExcept(uint16_t excludedPort,
                                          const uint8_t *data, size_t len)
  {
    if (data && len) broadcastWebPacketExcept(excludedPort, data, len);
  }

  bool WebTransport::audioTarget(uint16_t clientPort, uint8_t *addr,
                                 uint16_t &port,
                                 bool &viaWifi, bool &webPeer, bool &all) const
  {
    if (!_clientMutex ||
        xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      all = true;
      return false;
    }
    bool found = false;
    for (size_t i = 0; i < kMaxWebClients; ++i)
    {
      const WebClient &client = _webClients[i];
      if (!client.active || client.port != clientPort) continue;
      found = true;
      all = client.audioTargetAll;
      if (!all && addr)
      {
        memcpy(addr, client.audioTargetMac, sizeof(client.audioTargetMac));
        port = client.audioTargetPort;
        viaWifi = client.audioTargetViaWifi;
        webPeer = client.audioTargetWebPeer;
      }
      break;
    }
    xSemaphoreGive(_clientMutex);
    return found;
  }

  void WebTransport::setTransportManager(TransportManager *mgr)
  {
    _transportManager = mgr;
  }
}
