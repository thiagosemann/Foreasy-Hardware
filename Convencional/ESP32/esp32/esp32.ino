// ============================================================================
// Foreasy ESP32 — Modelos 3 e 4 — Convencional
// Hardware: ESP32 + shield relé 30A | Modelo 4 usa SSR acionado pelo relé 30A
//
// SISTEMA: Convencional — controle de energia pelo RELÉ (liga/desliga).
//   WS 0x01 => RELAY ON  (fica ligado até receber OFF)
//   WS 0x02 => RELAY OFF
//
// RELAY INVERT (relayInvert) — útil para relay NF (Normalmente Fechado):
//   false => normal   (ON = HIGH | OFF = LOW)
//   true  => invertido (ON = LOW  | OFF = HIGH)
//
// RELAY MODE (persistente):
//   relayMode 0 = Normal (obedece WS)
//   relayMode 1 = Sempre ON   (ignora WS)
//   relayMode 2 = Sempre OFF  (ignora WS)
//
// PROTOCOLO WEBSOCKET (binário):
//   0x01 => Relay ON  (se relayMode=0)
//   0x02 => Relay OFF (se relayMode=0)
//   0x03 => JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp,
//                 machineMode, pulse, chip, fw
//   0x04 => OTA: payload 0x04 + "url|sha256" (sha opcional, 64 hex). Baixa o .bin,
//                grava com Update, valida SHA256 e reinicia. Respostas async:
//                "OTA:QUEUED" → "OTA:START" → "OTA:OK:restart" | "OTA:FAIL:<motivo>"
//   0x06 => Restart remoto. Responde "Restarting" e reinicia após ~300ms.
//
// WIFI: Dual WiFi com failover automático (sem restart)
//   Conexão não-bloqueante, credenciais nunca apagadas por falha
//
// WEBSOCKET:
//   Backoff exponencial: 10s base → 120s máx
//   Watchdog WS down  : sem WS >5min  → failover
//   Watchdog global   : sem WiFi+WS >8min → failover
//   Zumbi             : sem ping/pong >5min → reconecta
//   wsRestartEnabled  : reinicia após 1h sem WS (configurável)
//
// AP: ativo 10 min após boot | SSID: <nodeId>-AP | Senha: 12345678
//
// ARMAZENAMENTO: Preferences (NVS)
//   ssid, pass, ssid2, pass2, nodeid, relayMode, relayInvert,
//   wsrestart, bootCount
//   bootCount incrementado em RAM; salvo apenas no /save
//
// SCAN WIFI: assíncrono | TEMPERATURA: temprature_sens_read()
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>

// Identidade do firmware (reportada no WS 0x03 — auditoria da frota / seleção de OTA)
#define FW_VERSION "1.0.0"
#define FW_CHIP    "esp32"    // ESP32 (Modelos 3 e 4)

float readInternalTempC() {
  return temperatureRead();
}

WebServer server(80);
WebSocketsClient webSocket;
Preferences prefs;

// ---------- IO ----------
const int ledPin   = 4;
const int relayPin = 2;

// ---------- Relay ----------
int  relayMode      = 0;   // 0=Normal, 1=Sempre ON, 2=Sempre OFF
bool relayInvert    = false;
bool relayLogicalOn = false;
int  relayOnLevel   = HIGH;
int  relayOffLevel  = LOW;

// ---------- Identity ----------
String nodeId = "FOREASY";

// ---------- Credenciais cacheadas ----------
String sSsid, sPass, sSsid2, sPass2;
uint8_t wifiSlot = 0;

const char* activeSSID() { return wifiSlot == 0 ? sSsid.c_str() : sSsid2.c_str(); }
const char* activePass() { return wifiSlot == 0 ? sPass.c_str() : sPass2.c_str(); }
bool hasSavedWiFi()      { return strlen(activeSSID()) > 0; }

// ---------- AP ----------
IPAddress apIP(192,168,4,1);
const uint32_t AP_LIFETIME_MS = 10UL * 60UL * 1000UL;
bool     apEnabled  = true;
uint32_t bootTimeMs = 0;

// ---------- WiFi STA ----------
bool     wifiConnecting     = false;
uint32_t wifiConnectStartMs = 0;
const uint32_t WIFI_MAX_WAIT_MS        = 40000;
const uint32_t WIFI_RETRY_INTERVAL_MS  = 5000;
uint32_t lastWiFiAttemptMs = 0;

// ---------- WebSocket ----------
const uint16_t wsPort = 80;
const char*    wsHost = "frst-back-02b607761078.herokuapp.com";
bool isWebSocketConnected = false;

// ---------- WS backoff ----------
uint32_t lastWSConnectAttemptMs = 0;
uint8_t  wsRetryStreak          = 0;
const uint32_t WS_RETRY_BASE_MS = 10000;
const uint32_t WS_RETRY_MAX_MS  = 120000;
uint32_t wsNextRetryMs = WS_RETRY_BASE_MS;

// ---------- Ping/pong ----------
uint32_t lastPingMs = 0;
const uint32_t PING_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// ---------- App ping ----------
uint32_t lastAppPingMs = 0;
const uint32_t APP_PING_INTERVAL_MS = 30UL * 1000UL;

// ---------- Watchdogs ----------
uint32_t wsDownSinceMs = 0;
const uint32_t WS_DOWN_RESET_MS = 5UL * 60UL * 1000UL;
uint32_t lastConnectivityOkMs = 0;
const uint32_t GLOBAL_DOWN_RESET_MS = 8UL * 60UL * 1000UL;

// ---------- WS auto-restart ----------
uint32_t wsLastOkMs = 0;
const uint32_t WS_RESTART_TIMEOUT_MS = 60UL * 60UL * 1000UL;
bool wsRestartEnabled = false;

// ---------- Boot count ----------
uint32_t bootCount = 0;

// ---------- OTA (enfileirado; executa em otaTick, fora do callback WS) ----------
bool   otaRequested  = false;
bool   otaInProgress = false;
String otaPendingUrl;
String otaPendingSha;

// ---------- Restart remoto (enfileirado; executa em restartTick) ----------
bool     restartRequested = false;
uint32_t restartAtMs      = 0;

// ---------- Wizard: teste ao vivo de WiFi/WS (não-bloqueante) ----------
enum TestState : uint8_t { TST_IDLE, TST_RUN, TST_OK, TST_FAIL };
TestState wifiTestState   = TST_IDLE;
bool      wifiTestActive  = false;
uint32_t  wifiTestStartMs = 0;
const uint32_t WIFI_TEST_TIMEOUT_MS = 12000;
String    wifiTestSsid, wifiTestPass;
String    lastGoodSsid, lastGoodPass;   // última rede testada com sucesso (p/ o teste de WS)

// ================= RELAY HELPERS =================
void updateRelayLevels() {
  if (!relayInvert) { relayOnLevel = HIGH; relayOffLevel = LOW; }
  else              { relayOnLevel = LOW;  relayOffLevel = HIGH; }
}

bool isRelayEffectiveOn() {
  if (relayMode == 1) return true;
  if (relayMode == 2) return false;
  return relayLogicalOn;
}

void applyRelayOutput() {
  digitalWrite(relayPin, isRelayEffectiveOn() ? relayOnLevel : relayOffLevel);
}

// ================= WS BACKOFF =================
uint32_t computeWsBackoffMs() {
  uint32_t v = WS_RETRY_BASE_MS;
  uint8_t  s = wsRetryStreak;
  while (s > 0 && v < WS_RETRY_MAX_MS) { v <<= 1; s--; }
  return (v > WS_RETRY_MAX_MS) ? WS_RETRY_MAX_MS : v;
}
void resetWsBackoff() { wsRetryStreak = 0; wsNextRetryMs = WS_RETRY_BASE_MS; }
void bumpWsBackoff()  { if (wsRetryStreak < 10) wsRetryStreak++; wsNextRetryMs = computeWsBackoffMs(); }

// ================= FAILOVER =================
void fullReconnectWiFiWS() {
  webSocket.disconnect();
  isWebSocketConnected = false;
  delay(50);
  WiFi.disconnect(true);
  delay(150);
  wifiConnecting = false;
  lastWiFiAttemptMs = 0;
  lastWSConnectAttemptMs = 0;
  lastPingMs = 0;
  wsDownSinceMs = 0;
  resetWsBackoff();
  if (hasSavedWiFi()) connectToWiFi_begin();
}

void switchWiFiSlot() {
  wifiSlot = (wifiSlot == 0) ? 1 : 0;
  Serial.printf("FAILOVER: slot %u SSID=%s\n", wifiSlot + 1, activeSSID());
  webSocket.disconnect();
  isWebSocketConnected = false;
  delay(50);
  WiFi.disconnect(true);
  delay(150);
  wifiConnecting = false;
  lastWiFiAttemptMs = 0;
  lastWSConnectAttemptMs = 0;
  lastPingMs = 0;
  wsDownSinceMs = 0;
  resetWsBackoff();
  if (hasSavedWiFi()) connectToWiFi_begin();
}

void failoverReconnect() {
  if (sSsid2.length() > 0) switchWiFiSlot();
  else                     fullReconnectWiFiWS();
}

// ================= PREFS =================
void loadPrefs() {
  nodeId           = prefs.getString("nodeid",      "FOREASY");
  sSsid            = prefs.getString("ssid",        "");
  sPass            = prefs.getString("pass",        "");
  sSsid2           = prefs.getString("ssid2",       "");
  sPass2           = prefs.getString("pass2",       "");
  relayMode        = prefs.getInt("relayMode",      0);
  relayInvert      = (prefs.getInt("relayInvert",   0) != 0);
  wsRestartEnabled = (prefs.getInt("wsrestart",     0) != 0);
  bootCount        = prefs.getUInt("bootCount",     0);
}

// ================= AP + STA =================
void setupAPSTA() {
  WiFi.mode(WIFI_AP_STA);
  String apName = nodeId + "-AP";
  WiFi.softAP(apName.c_str(), "12345678", 1, false);
  delay(200);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  Serial.printf("AP: %s | IP: %s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
}

void connectToWiFi_begin() {
  if (!hasSavedWiFi()) { Serial.println("Sem credenciais WiFi."); return; }
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(activeSSID(), activePass());
  wifiConnecting = true;
  wifiConnectStartMs = millis();
  Serial.printf("WiFi begin: SSID=%s (slot %u)\n", activeSSID(), wifiSlot + 1);
}

void connectToWebSocket() {
  if (WiFi.status() != WL_CONNECTED) return;
  webSocket.disconnect();
  delay(20);
  webSocket.begin(wsHost, wsPort, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);
  lastPingMs = millis();
  lastAppPingMs = 0;
  lastWSConnectAttemptMs = millis();
  Serial.printf("WS begin: %s:%u\n", wsHost, wsPort);
}

// ================= TESTE AO VIVO (wizard) =================
// Conecta o STA na rede candidata enquanto o AP segue de pé. Casa o canal do AP com
// o da rede alvo para reduzir a queda do celular (rádio único).
void startWifiTest(const String& ssid, const String& pass, int ch) {
  wifiTestActive  = true;
  wifiTestState   = TST_RUN;
  wifiTestStartMs = millis();
  wifiTestSsid = ssid; wifiTestPass = pass;
  webSocket.disconnect();
  isWebSocketConnected = false;
  WiFi.disconnect(false);
  delay(40);
  if (ch >= 1 && ch <= 13) {
    String apName = nodeId + "-AP";
    WiFi.softAP(apName.c_str(), "12345678", ch, false);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
    delay(60);
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("TEST WiFi: SSID=%s ch=%d\n", ssid.c_str(), ch);
}

void wifiTestTick() {
  if (wifiTestState != TST_RUN) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiTestState = TST_OK;
    lastGoodSsid = wifiTestSsid;
    lastGoodPass = wifiTestPass;
    Serial.printf("TEST WiFi OK. IP=%s\n", WiFi.localIP().toString().c_str());
  } else if ((millis() - wifiTestStartMs) > WIFI_TEST_TIMEOUT_MS) {
    wifiTestState = TST_FAIL;
    WiFi.disconnect(false);
    Serial.println("TEST WiFi FAIL.");
  }
}

static bool wsReadN(WiFiClient* c, uint8_t* buf, size_t len, uint32_t deadline) {
  size_t got = 0;
  while (got < len && (int32_t)(deadline - millis()) > 0) {
    if (c->available()) buf[got++] = (uint8_t)c->read();
    else delay(2);
  }
  return got == len;
}

static String jsonStr(const String& src, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int i = src.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int e = src.indexOf('"', i);
  return (e < 0) ? "" : src.substring(i, e);
}

// Teste de WebSocket (síncrono): handshake → "WhoAmI:<nodeId>" → resposta com prédio/máquina.
bool testWsSync(const String& host, uint16_t port, const String& nodeId,
                String& building, String& machine, bool& found) {
  building = ""; machine = ""; found = false;

  if (WiFi.status() != WL_CONNECTED && lastGoodSsid.length()) {
    WiFi.begin(lastGoodSsid.c_str(), lastGoodPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 8000) delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient       plain;
  WiFiClientSecure sec; sec.setInsecure();
  WiFiClient* c = (port == 443) ? (WiFiClient*)&sec : &plain;
  c->setTimeout(5000);
  if (!c->connect(host.c_str(), port)) return false;

  c->printf("GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
            host.c_str());

  String resp; uint32_t t0 = millis();
  while (c->connected() && (millis() - t0) < 5000 && resp.length() < 512) {
    while (c->available()) resp += (char)c->read();
    if (resp.indexOf("\r\n\r\n") >= 0) break;
    delay(5);
  }
  if (resp.indexOf("101") < 0) { c->stop(); return false; }

  String payload = "WhoAmI:" + nodeId;
  size_t n = payload.length();
  if (n < 126) {
    uint8_t mk[4]  = { 0x21, 0x53, 0xAE, 0x42 };
    uint8_t hdr[2] = { 0x81, (uint8_t)(0x80 | n) };
    c->write(hdr, 2); c->write(mk, 4);
    for (size_t i = 0; i < n; i++) { uint8_t b = (uint8_t)payload[i] ^ mk[i & 3]; c->write(&b, 1); }
    c->flush();

    uint32_t deadline = millis() + 6000;
    uint8_t h[2];
    if (wsReadN(c, h, 2, deadline)) {
      uint32_t plen = h[1] & 0x7F;
      if (plen == 126) { uint8_t ext[2]; plen = wsReadN(c, ext, 2, deadline) ? (((uint32_t)ext[0] << 8) | ext[1]) : 0; }
      String j; j.reserve(plen + 1);
      for (uint32_t i = 0; i < plen; i++) { uint8_t b; if (!wsReadN(c, &b, 1, deadline)) break; j += (char)b; }
      int br = j.indexOf("WhoAmI:");
      if (br >= 0) {
        String body = j.substring(br + 7);
        found    = (body.indexOf("\"found\":true") >= 0);
        building = jsonStr(body, "building");
        machine  = jsonStr(body, "machine");
      }
    }
  }
  c->stop();
  return true;
}

// ================= OTA (Over-The-Air) =================
void otaReport(String s) {
  Serial.println(s);
  if (isWebSocketConnected) webSocket.sendTXT(s);
}

bool doOTA(const String& url, const String& expectedSha) {
  if (WiFi.status() != WL_CONNECTED) { otaReport("OTA:FAIL:nowifi"); return false; }
  if (url.length() == 0)             { otaReport("OTA:FAIL:nourl");  return false; }

  otaInProgress = true;
  otaReport("OTA:START");

  bool https = url.startsWith("https");
  WiFiClient       plainClient;
  WiFiClientSecure secureClient;
  if (https) secureClient.setInsecure();
  WiFiClient* netClient = https ? (WiFiClient*)&secureClient : &plainClient;

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(*netClient, url)) { otaReport("OTA:FAIL:begin"); otaInProgress = false; return false; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaReport("OTA:FAIL:http" + String(code));
    http.end(); otaInProgress = false; return false;
  }

  int len = http.getSize();
  if (len <= 0) { otaReport("OTA:FAIL:len"); http.end(); otaInProgress = false; return false; }

  if (!Update.begin(len)) {
    otaReport("OTA:FAIL:space" + String(Update.getError()));
    http.end(); otaInProgress = false; return false;
  }

  bool verify = (expectedSha.length() == 64);
  mbedtls_sha256_context shaCtx;
  if (verify) { mbedtls_sha256_init(&shaCtx); mbedtls_sha256_starts(&shaCtx, 0); }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = len;
  uint32_t lastDataMs = millis();

  while (http.connected() && remaining > 0) {
    size_t avail = stream->available();
    if (avail) {
      int toRead = (avail > sizeof(buf)) ? sizeof(buf) : (int)avail;
      int n = stream->readBytes(buf, toRead);
      if (n <= 0) break;
      if (verify) mbedtls_sha256_update(&shaCtx, buf, n);
      if (Update.write(buf, (size_t)n) != (size_t)n) {
        if (verify) mbedtls_sha256_free(&shaCtx);
        Update.abort();
        otaReport("OTA:FAIL:write" + String(Update.getError()));
        http.end(); otaInProgress = false; return false;
      }
      remaining -= n;
      lastDataMs = millis();
    } else {
      if ((millis() - lastDataMs) > 15000) {
        if (verify) mbedtls_sha256_free(&shaCtx);
        Update.abort();
        otaReport("OTA:FAIL:timeout");
        http.end(); otaInProgress = false; return false;
      }
      delay(1);
    }
  }

  if (remaining != 0) {
    if (verify) mbedtls_sha256_free(&shaCtx);
    Update.abort();
    otaReport("OTA:FAIL:incomplete");
    http.end(); otaInProgress = false; return false;
  }

  if (verify) {
    uint8_t hash[32];
    mbedtls_sha256_finish(&shaCtx, hash);
    mbedtls_sha256_free(&shaCtx);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    if (!expectedSha.equalsIgnoreCase(hex)) {
      Update.abort();
      otaReport("OTA:FAIL:sha");
      http.end(); otaInProgress = false; return false;
    }
  }

  if (!Update.end(true)) {
    otaReport("OTA:FAIL:end" + String(Update.getError()));
    http.end(); otaInProgress = false; return false;
  }

  http.end();
  otaReport("OTA:OK:restart");
  delay(300);
  ESP.restart();
  return true;
}

void otaTick() {
  if (!otaRequested) return;
  otaRequested = false;
  doOTA(otaPendingUrl, otaPendingSha);
  otaInProgress = false;
}

void restartTick() {
  if (!restartRequested) return;
  if ((int32_t)(millis() - restartAtMs) < 0) return;
  Serial.println("WS 0x06: restart remoto.");
  ESP.restart();
}

// ================= WS EVENT =================
void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      isWebSocketConnected = true;
      lastPingMs   = millis();
      wsLastOkMs   = millis();
      wsDownSinceMs = 0;
      resetWsBackoff();
      String msg = isRelayEffectiveOn() ? ("ID:" + nodeId) : ("NID:" + nodeId);
      webSocket.sendTXT(msg);
      Serial.printf("WS conectado. Sent: %s | mode=CONVENCIONAL\n", msg.c_str());
      break;
    }

    case WStype_DISCONNECTED:
      isWebSocketConnected = false;
      bumpWsBackoff();
      Serial.printf("WS desconectado. Backoff=%ums (streak=%u)\n", wsNextRetryMs, wsRetryStreak);
      break;

    case WStype_PING:
      lastPingMs = millis();
      webSocket.sendTXT("Pong");
      break;

    case WStype_PONG:
      lastPingMs = millis();
      break;

    case WStype_BIN:
      if (length > 0) {
        uint8_t b = payload[0];

        if (b == 0x03) {
          bool staOk = (WiFi.status() == WL_CONNECTED);
          char buf[320];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%u,\"block\":%u,\"cpu\":%u,"
            "\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u,\"temp\":%.1f,"
            "\"machineMode\":0,\"pulse\":false,\"chip\":\"%s\",\"fw\":\"%s\"}",
            staOk ? WiFi.RSSI() : 0,
            staOk ? (int)WiFi.channel() : 0,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getCpuFreqMHz(),
            (unsigned long)(millis() / 1000UL),
            (unsigned long)bootCount,
            (unsigned)wifiSlot,
            readInternalTempC(),
            FW_CHIP,
            FW_VERSION
          );
          webSocket.sendTXT(buf);
          break;
        }

        // 0x04 => OTA. Payload: 0x04 + "url|sha256" (sha opcional). Enfileira; roda em otaTick().
        if (b == 0x04) {
          if (otaInProgress || otaRequested) { webSocket.sendTXT("OTA:BUSY"); break; }
          String params;
          for (size_t i = 1; i < length; i++) params += (char)payload[i];
          params.trim();
          int sep = params.indexOf('|');
          if (sep < 0) sep = params.indexOf('\n');
          if (sep < 0) sep = params.indexOf(' ');
          otaPendingUrl = (sep < 0) ? params : params.substring(0, sep);
          otaPendingSha = (sep < 0) ? ""     : params.substring(sep + 1);
          otaPendingUrl.trim();
          otaPendingSha.trim();
          if (otaPendingUrl.length() == 0) { webSocket.sendTXT("OTA:FAIL:nourl"); break; }
          otaRequested = true;
          webSocket.sendTXT("OTA:QUEUED");
          break;
        }

        // 0x06 => restart remoto (enfileira; reinicia em restartTick após a resposta sair)
        if (b == 0x06) {
          webSocket.sendTXT("Restarting");
          restartRequested = true;
          restartAtMs = millis() + 300;
          break;
        }

        if (relayMode == 0) {
          if      (b == 0x01) { relayLogicalOn = true;  applyRelayOutput(); }
          else if (b == 0x02) { relayLogicalOn = false; applyRelayOutput(); }
        }
        webSocket.sendTXT(isRelayEffectiveOn() ? "RelayStatus:ON" : "RelayStatus:OFF");
      }
      break;

    default:
      break;
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(100);

  prefs.begin("wifi", false);
  loadPrefs();
  bootCount++;

  updateRelayLevels();

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(relayPin, OUTPUT);
  relayLogicalOn = false;
  applyRelayOutput();

  bootTimeMs           = millis();
  apEnabled            = true;
  lastConnectivityOkMs = millis();
  wsLastOkMs           = millis();
  resetWsBackoff();

  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  setupAPSTA();
  startWebServer();
  server.begin();
  Serial.printf("HTTP server iniciado. modo=CONVENCIONAL relayInvert=%s\n",
                relayInvert ? "true" : "false");

  WiFi.scanNetworks(true);

  if (hasSavedWiFi()) connectToWiFi_begin();
  else Serial.println("Sem WiFi salvo. Configure pelo AP.");
}

// ================= LOOP =================
void loop() {
  server.handleClient();
  wifiTestTick();
  wifiTick();
  wsTick();
  watchdogTick();
  apLifetimeTick();
  wsRestartTick();
  otaTick();
  restartTick();

  digitalWrite(ledPin, isWebSocketConnected ? HIGH : LOW);
}

// ================= TICKS =================
void wifiTick() {
  if (wifiTestActive) return;   // STA é gerido pelo teste do wizard
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      Serial.printf("WiFi conectado. IP=%s\n", WiFi.localIP().toString().c_str());
      if (!isWebSocketConnected) connectToWebSocket();
    } else if ((millis() - wifiConnectStartMs) > WIFI_MAX_WAIT_MS) {
      wifiConnecting = false;
      Serial.println("WiFi timeout. Tentará novamente.");
      WiFi.disconnect(false);
    }
  }

  if (WiFi.status() != WL_CONNECTED && !wifiConnecting && hasSavedWiFi()) {
    if ((millis() - lastWiFiAttemptMs) >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiAttemptMs = millis();
      connectToWiFi_begin();
    }
  }
}

void wsTick() {
  if (wifiTestActive) return;   // sem WS de produção durante um teste do wizard
  webSocket.loop();

  if (isWebSocketConnected) {
    wsLastOkMs = millis();
    if ((millis() - lastAppPingMs) > APP_PING_INTERVAL_MS) {
      lastAppPingMs = millis();
      webSocket.sendTXT("Ping");
    }
    if ((millis() - lastPingMs) > PING_TIMEOUT_MS) {
      Serial.println("WS zumbi (sem ping/pong). Reconectando.");
      webSocket.disconnect();
      isWebSocketConnected = false;
      bumpWsBackoff();
      lastWSConnectAttemptMs = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED && !isWebSocketConnected) {
    if ((millis() - lastWSConnectAttemptMs) >= wsNextRetryMs) {
      lastWSConnectAttemptMs = millis();
      connectToWebSocket();
    }
  }
}

void watchdogTick() {
  if (WiFi.status() == WL_CONNECTED || isWebSocketConnected)
    lastConnectivityOkMs = millis();

  if (!apEnabled) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool wsOk   = isWebSocketConnected;

    if (wifiOk && !wsOk) {
      if (wsDownSinceMs == 0) wsDownSinceMs = millis();
      if ((millis() - wsDownSinceMs) > WS_DOWN_RESET_MS) {
        Serial.println("WATCHDOG: WS down >5min. Failover.");
        failoverReconnect();
      }
    } else {
      wsDownSinceMs = 0;
    }

    if (!wifiOk && !wsOk) {
      if ((millis() - lastConnectivityOkMs) > GLOBAL_DOWN_RESET_MS) {
        Serial.println("WATCHDOG: sem WiFi+WS >8min. Failover.");
        lastConnectivityOkMs = millis();
        failoverReconnect();
      }
    }
  } else {
    wsDownSinceMs = 0;
  }
}

void apLifetimeTick() {
  if (apEnabled && (millis() - bootTimeMs >= AP_LIFETIME_MS)) {
    Serial.println("AP lifetime expirou. Desligando AP.");
    WiFi.softAPdisconnect(true);
    apEnabled = false;
    lastConnectivityOkMs = millis();
    if (wifiTestActive) {
      wifiTestActive = false;
      wifiTestState  = TST_IDLE;
      WiFi.disconnect(false);
      if (hasSavedWiFi()) connectToWiFi_begin();
    }
  }
}

void wsRestartTick() {
  if (!wsRestartEnabled) return;
  if (isWebSocketConnected) { wsLastOkMs = millis(); return; }
  if (isRelayEffectiveOn())  { wsLastOkMs = millis(); return; }
  if ((millis() - wsLastOkMs) > WS_RESTART_TIMEOUT_MS) {
    Serial.println("WS_RESTART: sem WS por 1h. Reiniciando.");
    delay(200);
    ESP.restart();
  }
}

// ================= HTTP ROUTES =================
void startWebServer() {
  server.on("/",             handleRoot);            // landing: Wizard / Administrador
  server.on("/wizard",       handleConfigPage);      // assistente passo a passo
  server.on("/config",       handleConfigPage);      // alias (compat)
  server.on("/admin",        handleAdminPage);       // edições pontuais
  server.on("/config-data",  HTTP_GET,  handleConfigData);
  server.on("/info",         handleInfoPage);
  server.on("/scan",         HTTP_GET,  handleScan);
  server.on("/save",         HTTP_POST, handleSave);
  server.on("/status",       HTTP_GET,  handleStatusJson);
  server.on("/test-wifi",        HTTP_GET, handleTestWifi);
  server.on("/test-wifi-status", HTTP_GET, handleTestWifiStatus);
  server.on("/test-ws",          HTTP_GET, handleTestWs);
  server.on("/relay",        HTTP_GET,  handleRelayPage);
  server.on("/relay/on",     HTTP_GET,  handleRelayOn);
  server.on("/relay/off",    HTTP_GET,  handleRelayOff);
  server.on("/relay/config", HTTP_POST, handleRelayConfigSave);
  server.on("/nodeid",       HTTP_GET,  handleNodeIdPage);
  server.on("/savenodeid",   HTTP_POST, handleSaveNodeId);
  server.on("/wifistatus",   HTTP_GET,  handleWiFiStatusPage);
  server.on("/wsstatus",     HTTP_GET,  handleWSStatusPage);
  server.on("/resetwifi",    HTTP_GET,  handleResetWiFi);
  server.on("/restart",      HTTP_GET,  handleRestart);
  server.onNotFound(handleNotFound);
}

// ========== / (landing) ==========
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--mu:#557060;--tx:#d4f5e0}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;min-height:100vh}
header{padding:34px 20px 8px;text-align:center}
.logo{color:var(--ac);font-size:26px;font-weight:700;letter-spacing:6px}
.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:4px;text-transform:uppercase}
main{max-width:460px;margin:0 auto;padding:18px;display:flex;flex-direction:column;gap:14px}
a.card{display:block;text-decoration:none;background:var(--cd);border:1px solid var(--bd);border-radius:10px;padding:22px 20px;transition:all .15s}
a.card:hover{border-color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.1)}
.ct{color:var(--ac);font-size:17px;font-weight:700;letter-spacing:1px}
.cd{color:var(--mu);font-size:11px;line-height:1.6;margin-top:8px}
.foot{margin-top:6px;text-align:center;font-size:11px;line-height:2}
.foot a{color:var(--mu);text-decoration:none;margin:0 6px}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">esp32 convencional · configuração</div></header>
<main>
  <a class="card" href="/wizard"><div class="ct">▶ Assistente (Wizard)</div><div class="cd">Configuração guiada passo a passo: testa o Wi-Fi e o servidor antes de salvar. Recomendado na primeira instalação.</div></a>
  <a class="card" href="/admin"><div class="ct">⚙ Administrador</div><div class="cd">Editar configurações pontuais (Node ID, redes, relé) sem refazer tudo.</div></a>
  <div class="foot"><a href="/info">status (/info)</a><a href="/relay">controle do relé</a></div>
</main>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /wizard (assistente passo a passo) ==========
void handleConfigPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Configuração</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--cd2:#0b110d;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0b130e;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;font-size:13px;min-height:100vh}
header{padding:20px 20px 8px;text-align:center}
.logo{color:var(--ac);font-size:22px;font-weight:700;letter-spacing:5px}
.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:3px;text-transform:uppercase}
main{max-width:560px;margin:0 auto;padding:6px 16px 36px}
.steps{display:flex;gap:6px;margin:14px 0 6px}
.pill{flex:1;display:flex;flex-direction:column;align-items:center;gap:4px;color:var(--mu);font-size:9px;letter-spacing:1px;text-transform:uppercase}
.pill b{display:flex;align-items:center;justify-content:center;width:26px;height:26px;border-radius:50%;border:1px solid var(--bd);font-size:12px;background:var(--cd)}
.pill.cur{color:var(--ac)}.pill.cur b{border-color:var(--ac);color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.12)}
.pill.done{color:var(--lb)}.pill.done b{border-color:var(--ac2);background:#04261447;color:var(--ac)}
.track{height:2px;background:var(--bd);border-radius:2px;overflow:hidden;margin-bottom:18px}
.fill{height:100%;width:0;background:linear-gradient(90deg,var(--ac2),var(--ac));transition:width .35s ease}
.card-wrap{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px 16px;min-height:230px}
.step{display:none;animation:fade .35s ease}
.step.on{display:block}
@keyframes fade{from{opacity:0;transform:translateX(14px)}to{opacity:1;transform:none}}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:16px 0 8px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
.sec:first-child{margin-top:0}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:11px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none}
input:focus,select:focus{border-color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.1)}
select option{background:var(--cd)}
.trow{display:flex;gap:8px;margin-top:8px}
.tgl{flex:1;padding:11px 8px;border:1px solid var(--bd);border-radius:6px;cursor:pointer;text-align:center;background:var(--cd2);color:var(--mu);font-size:12px;line-height:1.4}
.tgl.active{border-color:var(--ac);background:#04261433;color:var(--ac);font-weight:700}
.tgl small{display:block;font-size:10px;opacity:.7;margin-top:2px}
.chk{display:flex;align-items:center;gap:10px;margin-top:14px;padding:10px 12px;border:1px solid var(--bd);border-radius:4px;background:var(--cd2)}
.chk input{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}
.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0;cursor:pointer}
.hint{color:var(--mu);font-size:10px;line-height:1.5;margin-top:10px;padding:8px 10px;border-left:2px solid var(--bd)}
.nav{display:flex;gap:8px;margin-top:16px}
.btn{flex:1;padding:13px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}
.btn:hover{background:var(--ac2);color:var(--tx)}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu);flex:0 0 110px}
.btn.ghost:hover{border-color:var(--ac);color:var(--ac)}
.tbtn{margin-top:14px;flex:1}
.ts{margin-top:9px;font-size:11px;line-height:1.5;min-height:14px;color:var(--mu)}
.ts.ok{color:var(--ac)}.ts.err{color:var(--red)}.ts.run{color:var(--lb)}
.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}
.foot{margin-top:16px;text-align:center;font-size:11px}
.foot a{color:var(--mu);text-decoration:none}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">assistente de configuração · convencional</div></header>
<main>
  <div class="steps">
    <div class="pill cur" id="pill0"><b>1</b><span>Rede 1</span></div>
    <div class="pill" id="pill1"><b>2</b><span>Rede 2</span></div>
    <div class="pill" id="pill2"><b>3</b><span>Servidor</span></div>
    <div class="pill" id="pill3"><b>4</b><span>Relé</span></div>
  </div>
  <div class="track"><div class="fill" id="bar"></div></div>

  <div class="card-wrap">
    <div class="step on" id="step0">
      <div class="sec">Rede 1 — primária</div>
      <label>Redes encontradas</label>
      <select id="ssid"></select>
      <label>Ou SSID manual</label>
      <input id="manual_ssid" placeholder="nome da rede">
      <label>Senha</label>
      <input id="pass" type="text" placeholder="vazio se aberta">
      <button class="btn tbtn" id="t0">Testar conexão</button>
      <div class="ts" id="ts0">Teste a rede para poder avançar.</div>
    </div>

    <div class="step" id="step1">
      <div class="sec">Rede 2 — failover (opcional)</div>
      <label>Redes encontradas</label>
      <select id="ssid2_scan"></select>
      <label>Ou SSID manual</label>
      <input id="ssid2" placeholder="opcional">
      <label>Senha</label>
      <input id="pass2" type="text" placeholder="opcional">
      <button class="btn tbtn" id="t1">Testar conexão</button>
      <div class="ts" id="ts1">Opcional — toque em <b>Pular</b> se não houver rede 2.</div>
    </div>

    <div class="step" id="step2">
      <div class="sec">Identificação</div>
      <label>Node ID</label>
      <input id="nodeid" placeholder="ex: C00045">
      <div class="hint">O servidor é fixo de fábrica. O teste confirma o prédio/máquina deste Node ID.</div>
      <button class="btn tbtn" id="t2">Testar WebSocket</button>
      <div class="ts" id="ts2">Testa a conexão com o servidor.</div>
    </div>

    <div class="step" id="step3">
      <div class="sec">Configuração do relé</div>
      <label>Modo do relé</label>
      <div class="trow">
        <div id="modeNormal" class="tgl active">Normal<small>segue WS</small></div>
        <div id="modeOn"     class="tgl">Sempre ON</div>
        <div id="modeOff"    class="tgl">Sempre OFF</div>
      </div>
      <div class="chk"><input id="invert" type="checkbox"><label for="invert">Inverter lógica do relé — ON=LOW (NF, Normalmente Fechado)</label></div>
      <div class="hint">Em <b>Sempre ON/OFF</b> o relé ignora o WebSocket. Use <b>Normal</b> para o backend ligar/desligar.</div>
    </div>
  </div>

  <div class="nav">
    <button class="btn ghost" id="back">Voltar</button>
    <button class="btn ghost" id="skip" style="display:none">Pular</button>
    <button class="btn" id="next">Avançar</button>
  </div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/">← início</a> · <a href="/info">status (/info)</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
const N=4; let cur=0;
let scanList=[], net1ok=false, wsdone=false, relayModeVal=0;

function paint(){
  for(let i=0;i<N;i++){
    qs('step'+i).classList.toggle('on', i===cur);
    qs('pill'+i).classList.toggle('done', i<cur);
    qs('pill'+i).classList.toggle('cur', i===cur);
  }
  qs('bar').style.width=(cur/(N-1)*100)+'%';
  qs('back').style.visibility=cur?'visible':'hidden';
  qs('skip').style.display=(cur===1)?'block':'none';
  qs('next').textContent=cur===N-1?'Salvar e Reiniciar':'Avançar';
}
function msg(t){qs('msg').textContent=t||'';}
function setTs(id,cls,txt){let e=qs(id);e.className='ts '+cls;e.textContent=txt;}
function chFor(ssid){let f=scanList.find(x=>x.ssid===ssid);return f?f.ch:0;}
function setRelayMode(v){relayModeVal=v;['modeNormal','modeOn','modeOff'].forEach((id,i)=>qs(id).classList.toggle('active',i===v));}

function testWifi(ssidVal,passVal,tsId,cb){
  if(!ssidVal){setTs(tsId,'err','Selecione ou digite a rede.');return;}
  setTs(tsId,'run','Testando… o Wi-Fi do ESP pode cair alguns segundos — reconecte se precisar.');
  fetch('/test-wifi?ssid='+encodeURIComponent(ssidVal)+'&pass='+encodeURIComponent(passVal)+'&ch='+chFor(ssidVal)).catch(()=>{});
  let tries=0;
  const poll=()=>{
    fetch('/test-wifi-status').then(r=>r.json()).then(j=>{
      if(j.state==='ok'){setTs(tsId,'ok','✓ Conectou! (RSSI '+j.rssi+' dBm)');cb(true);}
      else if(j.state==='fail'){setTs(tsId,'err','✗ Não conectou. Confira a senha e tente de novo.');cb(false);}
      else if(tries++<30){setTimeout(poll,1500);}
      else{setTs(tsId,'err','✗ Tempo esgotado. Tente de novo.');cb(false);}
    }).catch(()=>{ if(tries++<30) setTimeout(poll,1800); else {setTs(tsId,'err','✗ Sem resposta — reconecte ao Wi-Fi do ESP e tente de novo.');cb(false);} });
  };
  setTimeout(poll,1800);
}

function testWs(){
  let nid=qs('nodeid').value.trim();
  if(!nid){setTs('ts2','err','Preencha o Node ID antes de testar.');return;}
  setTs('ts2','run','Testando servidor…');
  fetch('/test-ws?nodeid='+encodeURIComponent(nid)).then(r=>r.json()).then(j=>{
    wsdone=true;
    if(j.ok && j.found) setTs('ts2','ok','✓ Conectado! Prédio: '+(j.building||'?')+' · Máquina: '+(j.machine||'?'));
    else if(j.ok) setTs('ts2','err','⚠ Servidor OK, mas o Node ID "'+nid+'" não foi encontrado. Confira o Node ID.');
    else setTs('ts2','err','✗ Não conectou ao servidor. Você pode avançar e revisar depois.');
  }).catch(()=>{wsdone=true;setTs('ts2','err','✗ Falha no teste. Você pode avançar mesmo assim.');});
}

function next(){
  if(cur===0 && !net1ok){msg('Teste a rede 1 antes de avançar.');return;}
  if(cur===2 && !wsdone){msg('Teste o servidor antes de avançar.');return;}
  msg('');
  if(cur===N-1) return save();
  cur++; paint();
}
function back(){ if(cur>0){cur--; paint(); msg('');} }

function save(){
  let ss=qs('manual_ssid').value.trim()||qs('ssid').value;
  let ss2=qs('ssid2').value.trim()||qs('ssid2_scan').value;
  if(!qs('nodeid').value.trim()){msg('Preencha o Node ID');return;}
  msg('Salvando…');
  let b='ssid='+encodeURIComponent(ss)+
    '&pass='+encodeURIComponent(qs('pass').value)+
    '&ssid2='+encodeURIComponent(ss2)+
    '&pass2='+encodeURIComponent(qs('pass2').value)+
    '&nodeid='+encodeURIComponent(qs('nodeid').value.trim())+
    '&relayMode='+relayModeVal+
    '&relayInvert='+(qs('invert').checked?1:0);
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
    .then(r=>r.text()).then(t=>{msg(t+' Reconecte ao Wi-Fi em ~5s.');})
    .catch(()=>msg('Falha ao salvar.'));
}
function encText(e){return['Open','WEP','WPA-PSK','WPA2-PSK','WPA/WPA2'][e]||'?';}
function scan(retry){
  retry=retry||0;
  fetch('/scan').then(r=>r.json()).then(list=>{
    if(list.length===0&&retry<6){setTimeout(()=>scan(retry+1),2500);return;}
    scanList=list;
    let s=qs('ssid'); s.innerHTML='';
    let s2=qs('ssid2_scan'); s2.innerHTML='<option value="">— nenhuma —</option>';
    list.forEach(i=>{
      let o=document.createElement('option'); o.value=i.ssid;
      o.textContent=i.ssid+' · '+i.rssi+'dBm · ch'+i.ch+' · '+encText(i.enc);
      s.appendChild(o); s2.appendChild(o.cloneNode(true));
    });
  }).catch(()=>{if(retry<6)setTimeout(()=>scan(retry+1),2500);});
}
window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('manual_ssid').value=d.ssid||''; qs('pass').value=d.pass||'';
    qs('ssid2').value=d.ssid2||''; qs('pass2').value=d.pass2||'';
    qs('nodeid').value=d.nodeid||'';
    setRelayMode(d.relayMode||0);
    qs('invert').checked=(d.relayInvert===1);
  }).catch(()=>{});
  scan();
  qs('t0').onclick=()=>{net1ok=false;testWifi(qs('manual_ssid').value.trim()||qs('ssid').value,qs('pass').value,'ts0',(ok)=>{net1ok=ok;});};
  qs('t1').onclick=()=>{testWifi(qs('ssid2').value.trim()||qs('ssid2_scan').value,qs('pass2').value,'ts1',()=>{});};
  qs('t2').onclick=()=>{wsdone=false;testWs();};
  qs('modeNormal').onclick=()=>setRelayMode(0);
  qs('modeOn').onclick=()=>setRelayMode(1);
  qs('modeOff').onclick=()=>setRelayMode(2);
  qs('next').onclick=next; qs('back').onclick=back;
  qs('skip').onclick=()=>{if(cur===1){cur++;paint();msg('');}};
  paint();
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /admin (edições pontuais) ==========
void handleAdminPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Admin</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0b130e;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px;min-height:100vh}
header{padding:20px 20px 6px;text-align:center}
.logo{color:var(--ac);font-size:22px;font-weight:700;letter-spacing:5px}
.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:3px;text-transform:uppercase}
main{max-width:520px;margin:0 auto;padding:8px 16px 36px}
.box{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:14px;margin-top:12px}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:9px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:9px 11px;font-family:inherit;font-size:13px;outline:none}
input:focus,select:focus{border-color:var(--ac)}
select option{background:var(--cd)}
.trow{display:flex;gap:8px;margin-top:8px}
.tgl{flex:1;padding:10px 6px;border:1px solid var(--bd);border-radius:5px;cursor:pointer;text-align:center;background:var(--cd);color:var(--mu);font-size:12px}
.tgl.active{border-color:var(--ac);background:#04261433;color:var(--ac);font-weight:700}
.chk{display:flex;align-items:center;gap:10px;margin-top:10px}
.chk input{width:15px;height:15px;accent-color:var(--ac)}
.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0}
.btn{width:100%;margin-top:12px;padding:11px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:1px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}
.btn:hover{background:var(--ac2);color:var(--tx)}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu)}
.btn.ghost:hover{border-color:var(--ac);color:var(--ac)}
.btn.danger{background:transparent;border:1px solid var(--red);color:var(--red)}
.btn.danger:hover{background:var(--red);color:#000}
.row{display:flex;gap:8px}.row .btn{flex:1}
.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}
.foot{margin-top:16px;text-align:center;font-size:11px}
.foot a{color:var(--mu);text-decoration:none;margin:0 5px}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">administrador · convencional</div></header>
<main>
  <div class="box">
    <div class="sec">Node ID</div>
    <input id="nodeid">
    <button class="btn" id="bNode">Salvar Node ID</button>
  </div>
  <div class="box">
    <div class="sec">Rede 1</div>
    <label>Redes</label><select id="ssid"></select>
    <label>Ou SSID manual</label><input id="m1">
    <label>Senha</label><input id="p1" type="text">
    <button class="btn" id="bN1">Salvar rede 1</button>
  </div>
  <div class="box">
    <div class="sec">Rede 2 (failover)</div>
    <label>Redes</label><select id="ssid2"></select>
    <label>Ou SSID manual</label><input id="m2">
    <label>Senha</label><input id="p2" type="text">
    <button class="btn" id="bN2">Salvar rede 2</button>
  </div>
  <div class="box">
    <div class="sec">Relé</div>
    <label>Modo do relé</label>
    <div class="trow">
      <div id="modeNormal" class="tgl active">Normal</div>
      <div id="modeOn" class="tgl">Sempre ON</div>
      <div id="modeOff" class="tgl">Sempre OFF</div>
    </div>
    <div class="chk"><input id="invert" type="checkbox"><label for="invert">Inverter lógica (ON=LOW, NF)</label></div>
    <button class="btn" id="bRelay">Salvar relé</button>
  </div>
  <div class="box">
    <div class="sec">Avançado</div>
    <div class="chk"><input id="wsrestart" type="checkbox"><label for="wsrestart">Auto-restart se 1h sem WebSocket</label></div>
    <button class="btn" id="bAdv">Salvar avançado</button>
    <div class="row"><button class="btn ghost" id="bRst">Reiniciar</button><button class="btn danger" id="bClr">Apagar tudo</button></div>
  </div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/">← início</a><a href="/wizard">assistente</a><a href="/relay">relé</a><a href="/info">/info</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
function val(i){return qs(i).value.trim();}
function msg(t){qs('msg').textContent=t;}
let relayModeVal=0;
function setRelayMode(v){relayModeVal=v;['modeNormal','modeOn','modeOff'].forEach((id,i)=>qs(id).classList.toggle('active',i===v));}
function post(b){msg('Salvando…');fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.text()).then(t=>msg(t)).catch(()=>msg('Falha.'));}
function save(o){post(Object.keys(o).map(k=>k+'='+encodeURIComponent(o[k])).join('&'));}
function scan(){fetch('/scan').then(r=>r.json()).then(l=>{['ssid','ssid2'].forEach(id=>{let s=qs(id);s.innerHTML='<option value="">— escolher —</option>';l.forEach(i=>{let o=document.createElement('option');o.value=i.ssid;o.textContent=i.ssid+' · '+i.rssi+'dBm';s.appendChild(o);});});}).catch(()=>{});}
window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('nodeid').value=d.nodeid||''; setRelayMode(d.relayMode||0);
    qs('invert').checked=(d.relayInvert===1); qs('wsrestart').checked=(d.wsrestart===1);
  }).catch(()=>{});
  scan();
  qs('modeNormal').onclick=()=>setRelayMode(0);
  qs('modeOn').onclick=()=>setRelayMode(1);
  qs('modeOff').onclick=()=>setRelayMode(2);
  qs('bNode').onclick=()=>{if(!val('nodeid')){msg('Preencha o Node ID');return;}save({nodeid:val('nodeid')});};
  qs('bN1').onclick=()=>{let s=val('m1')||val('ssid');if(!s){msg('Escolha a rede 1');return;}save({ssid:s,pass:qs('p1').value});};
  qs('bN2').onclick=()=>{save({ssid2:(val('m2')||val('ssid2')),pass2:qs('p2').value});};
  qs('bRelay').onclick=()=>{save({relayMode:relayModeVal,relayInvert:(qs('invert').checked?1:0)});};
  qs('bAdv').onclick=()=>{save({wsrestart:(qs('wsrestart').checked?1:0)});};
  qs('bRst').onclick=()=>{if(confirm('Reiniciar o dispositivo?')){msg('Reiniciando…');fetch('/restart');}};
  qs('bClr').onclick=()=>{if(confirm('Apagar TODA a configuração e reiniciar?')){msg('Apagando…');fetch('/resetwifi');}};
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== Endpoints de teste do wizard ==========
void handleTestWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  int ch = server.hasArg("ch") ? server.arg("ch").toInt() : 0;
  if (ssid.length() == 0) { server.send(400, "application/json", "{\"started\":false}"); return; }
  startWifiTest(ssid, pass, ch);
  server.send(200, "application/json", "{\"started\":true}");
}

void handleTestWifiStatus() {
  const char* s = (wifiTestState == TST_OK) ? "ok" : (wifiTestState == TST_FAIL) ? "fail" : "testing";
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  server.send(200, "application/json", String("{\"state\":\"") + s + "\",\"rssi\":" + String(rssi) + "}");
}

void handleTestWs() {
  String   host = server.hasArg("host") ? server.arg("host") : String(wsHost);
  uint16_t port = server.hasArg("port") ? (uint16_t)server.arg("port").toInt() : wsPort;
  String   nid  = server.hasArg("nodeid") ? server.arg("nodeid") : nodeId;
  String building, machine; bool found = false;
  bool ok = testWsSync(host, port, nid, building, machine, found);
  building.replace("\\", "\\\\"); building.replace("\"", "\\\"");
  machine.replace("\\", "\\\\");  machine.replace("\"", "\\\"");
  String json = String("{\"ok\":") + (ok ? "true" : "false")
              + ",\"found\":" + (found ? "true" : "false")
              + ",\"building\":\"" + building + "\""
              + ",\"machine\":\""  + machine  + "\"}";
  server.send(200, "application/json", json);
}

// ========== /config-data ==========
void handleConfigData() {
  String json = "{";
  json += "\"ssid\":"        + (sSsid.length()  ? "\""+sSsid+"\""  : "\"\"") + ",";
  json += "\"pass\":"        + (sPass.length()  ? "\""+sPass+"\""  : "\"\"") + ",";
  json += "\"ssid2\":"       + (sSsid2.length() ? "\""+sSsid2+"\"" : "\"\"") + ",";
  json += "\"pass2\":"       + (sPass2.length() ? "\""+sPass2+"\"" : "\"\"") + ",";
  json += "\"nodeid\":\""    + nodeId + "\",";
  json += "\"relayMode\":"   + String(relayMode) + ",";
  json += "\"relayInvert\":" + String(relayInvert ? 1 : 0) + ",";
  json += "\"wsrestart\":"   + String(wsRestartEnabled ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

// ========== /save (parcial: grava só os campos enviados) ==========
// Usado pelo wizard (envia tudo) e pelo painel admin (envia subconjuntos).
void handleSave() {
  bool any = false, relayChanged = false;
  if (server.hasArg("ssid"))       { sSsid  = server.arg("ssid");  prefs.putString("ssid",  sSsid);  any = true; }
  if (server.hasArg("pass"))       { sPass  = server.arg("pass");  prefs.putString("pass",  sPass);  any = true; }
  if (server.hasArg("ssid2"))      { sSsid2 = server.arg("ssid2"); prefs.putString("ssid2", sSsid2); any = true; }
  if (server.hasArg("pass2"))      { sPass2 = server.arg("pass2"); prefs.putString("pass2", sPass2); any = true; }
  if (server.hasArg("nodeid"))     { nodeId = server.arg("nodeid"); prefs.putString("nodeid", nodeId); any = true; }
  if (server.hasArg("relayMode"))  { relayMode = constrain(server.arg("relayMode").toInt(), 0, 2); prefs.putInt("relayMode", relayMode); any = true; relayChanged = true; }
  if (server.hasArg("relayInvert")){ relayInvert = server.arg("relayInvert").toInt() == 1; prefs.putInt("relayInvert", relayInvert ? 1 : 0); any = true; relayChanged = true; }
  if (server.hasArg("wsrestart"))  { wsRestartEnabled = server.arg("wsrestart").toInt() == 1; prefs.putInt("wsrestart", wsRestartEnabled ? 1 : 0); any = true; }

  if (server.hasArg("ssid") || server.hasArg("ssid2")) wifiSlot = 0;
  prefs.putUInt("bootCount", bootCount);
  if (relayChanged) { updateRelayLevels(); applyRelayOutput(); }

  server.send(200, "text/plain", any ? "Configurado. Reiniciando..." : "Nada para salvar.");
  if (any) { delay(400); ESP.restart(); }
}

// ========== /status ==========
void handleStatusJson() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  String json = "{";
  json += "\"nodeId\":\""      + nodeId + "\",";
  json += "\"ssid\":\""        + (staOk ? WiFi.SSID() : String("")) + "\",";
  json += "\"rssi\":"          + String(staOk ? WiFi.RSSI() : 0) + ",";
  json += "\"ip_sta\":\""      + (staOk ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"ip_ap\":\""       + WiFi.softAPIP().toString() + "\",";
  json += "\"wsConnected\":"   + String(isWebSocketConnected ? "true" : "false") + ",";
  json += "\"temp\":"          + String(readInternalTempC(), 1) + ",";
  json += "\"relayOn\":"       + String(isRelayEffectiveOn() ? "true" : "false") + ",";
  json += "\"relayMode\":"     + String(relayMode) + ",";
  json += "\"relayInvert\":"   + String(relayInvert ? 1 : 0) + ",";
  json += "\"wifiSlot\":"      + String(wifiSlot) + ",";
  json += "\"boots\":"         + String(bootCount);
  json += "}";
  server.send(200, "application/json", json);
}

// ========== /info ==========
void handleInfoPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Info</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}
.logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:880px;margin:0 auto;padding:14px 16px 28px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:8px}
.it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:11px 13px}
.lb{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}
.vl{color:var(--tx);font-size:14px;font-weight:600}
pre{font-size:11px;background:#060a07;color:#4ade80;padding:10px;border-radius:3px;max-height:200px;overflow:auto;margin-top:14px;border:1px solid var(--bd);white-space:pre-wrap}
.nav{margin-top:14px;font-size:11px;display:flex;gap:16px}
.nav a{color:var(--mu);text-decoration:none}
.nav a:hover{color:var(--ac)}
</style></head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">info — refresh 3s</div>
</header>
<main>
  <div class="grid">
    <div class="it"><div class="lb">Node ID</div><div class="vl" id="nodeId">…</div></div>
    <div class="it"><div class="lb">Wi-Fi (STA)</div><div class="vl" id="ssid">…</div></div>
    <div class="it"><div class="lb">RSSI</div><div class="vl" id="rssi">…</div></div>
    <div class="it"><div class="lb">Temp (°C)</div><div class="vl" id="temp">…</div></div>
    <div class="it"><div class="lb">IP (STA)</div><div class="vl" id="ip_sta">…</div></div>
    <div class="it"><div class="lb">IP (AP)</div><div class="vl" id="ip_ap">…</div></div>
    <div class="it"><div class="lb">WebSocket</div><div class="vl" id="ws">…</div></div>
    <div class="it"><div class="lb">Relé</div><div class="vl" id="relay">…</div></div>
    <div class="it"><div class="lb">Relay Mode</div><div class="vl" id="relayMode">…</div></div>
    <div class="it"><div class="lb">Relay Invert</div><div class="vl" id="relayInvert">…</div></div>
    <div class="it"><div class="lb">WiFi Slot</div><div class="vl" id="wifiSlot">…</div></div>
    <div class="it"><div class="lb">Boots</div><div class="vl" id="boots">…</div></div>
  </div>
  <div class="nav">
    <a href="/config">← /config</a>
    <a href="/relay">→ /relay</a>
    <a href="/">← menu</a>
  </div>
  <pre id="raw"></pre>
</main>
<script>
const modeLabel=['Normal','Sempre ON','Sempre OFF'];
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    document.getElementById('nodeId').textContent     = j.nodeId||'—';
    document.getElementById('ssid').textContent       = j.ssid||'—';
    document.getElementById('rssi').textContent       = (j.rssi||0)+' dBm';
    document.getElementById('temp').textContent       = j.temp+'°C';
    document.getElementById('ip_sta').textContent     = j.ip_sta||'—';
    document.getElementById('ip_ap').textContent      = j.ip_ap||'—';
    document.getElementById('ws').textContent         = j.wsConnected ? 'Conectado' : 'Desconectado';
    document.getElementById('relay').textContent      = j.relayOn ? 'ON' : 'OFF';
    document.getElementById('relayMode').textContent  = modeLabel[j.relayMode]||j.relayMode;
    document.getElementById('relayInvert').textContent= j.relayInvert ? 'SIM (NF)' : 'NÃO (NA)';
    document.getElementById('wifiSlot').textContent   = 'Slot '+(j.wifiSlot+1);
    document.getElementById('boots').textContent      = j.boots;
    document.getElementById('raw').textContent        = JSON.stringify(j,null,2);
  });
}
setInterval(upd,3000); upd();
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /scan (assíncrono) ==========
void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "[]"); return; }
  if (n == WIFI_SCAN_FAILED || n < 0) {
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "[]");
    return;
  }
  String json = "[";
  int limit = (n > 25) ? 25 : n;
  for (int i = 0; i < limit; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) +
            "\",\"rssi\":"  + WiFi.RSSI(i) +
            ",\"ch\":"      + WiFi.channel(i) +
            ",\"enc\":"     + (int)WiFi.encryptionType(i) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  server.send(200, "application/json", json);
}

// ========== /relay ==========
void handleRelayPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Relé</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0d1710;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}
.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:480px;margin:0 auto;padding:18px 16px 32px}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:22px 0 10px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
.state{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:14px 16px;margin-bottom:4px;display:flex;align-items:center;justify-content:space-between}
.state-lbl{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase}
.state-val{font-size:20px;font-weight:700;color:var(--ac)}
.trow{display:flex;gap:8px;margin-top:8px}
.tgl{flex:1;padding:11px 8px;border:1px solid var(--bd);border-radius:3px;cursor:pointer;text-align:center;background:var(--cd);color:var(--mu);font-size:12px;transition:all .15s;line-height:1.4}
.tgl.active{background:#003d1a;border-color:var(--ac);color:var(--ac);font-weight:700}
.tgl small{display:block;font-size:10px;opacity:.7;margin-top:2px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:12px 0 4px}
.chk{display:flex;align-items:center;gap:10px;margin-top:10px;padding:10px 12px;border:1px solid var(--bd);border-radius:3px;background:var(--cd)}
.chk input[type=checkbox]{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}
.chk span{font-size:12px;color:var(--tx)}
.btn{display:block;width:100%;margin-top:14px;padding:12px;border:none;border-radius:3px;font-family:inherit;font-size:13px;font-weight:700;letter-spacing:1px;cursor:pointer;text-transform:uppercase}
.btn-save{background:var(--ac);color:#000}
.btn-save:hover{background:var(--ac2);color:var(--tx)}
.btn-row{display:flex;gap:8px;margin-top:14px}
.btn-on{flex:1;background:#003d1a;border:1px solid var(--ac);color:var(--ac)}
.btn-on:hover{background:var(--ac);color:#000}
.btn-off{flex:1;background:#3d0000;border:1px solid var(--red);color:var(--red)}
.btn-off:hover{background:var(--red);color:#000}
.st{margin-top:10px;font-size:12px;min-height:14px;color:var(--ac)}
.note{margin-top:12px;color:var(--mu);font-size:11px;line-height:1.5;padding:10px 12px;border:1px solid var(--bd);border-radius:3px;background:var(--cd)}
.nav{margin-top:20px;font-size:11px}
.nav a{color:var(--mu);text-decoration:none}
.nav a:hover{color:var(--ac)}
</style></head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">controle do relé</div>
</header>
<main>
  <div class="state">
    <div><div class="state-lbl">Estado atual</div></div>
    <div class="state-val" id="relayState">…</div>
  </div>

  <div class="sec">Configuração do relé</div>
  <label>Modo do relé</label>
  <div class="trow">
    <div id="modeNormal" class="tgl active">Normal<small>segue WS</small></div>
    <div id="modeOn"     class="tgl">Sempre ON</div>
    <div id="modeOff"    class="tgl">Sempre OFF</div>
  </div>

  <div class="chk" style="margin-top:12px">
    <input type="checkbox" id="invert">
    <span>Inverter lógica — ON=LOW (NF, Normalmente Fechado)</span>
  </div>

  <button class="btn btn-save" id="saveCfg">Salvar Configuração</button>
  <div class="st" id="cfgStatus"></div>

  <div class="sec">Controle manual</div>
  <div class="btn-row">
    <button class="btn btn-on"  id="btnOn">Ligar</button>
    <button class="btn btn-off" id="btnOff">Desligar</button>
  </div>
  <div class="note">Em <b>Sempre ON/OFF</b>: WS e botões ignorados.<br>Em <b>Normal</b>: "Ligar/Desligar" controla o relé e ele segue os comandos do WebSocket.</div>

  <div class="nav"><a href="/">← menu</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
let relayModeVal=0;

function setRelayMode(v){
  relayModeVal=v;
  ['modeNormal','modeOn','modeOff'].forEach((id,i)=>qs(id).classList.toggle('active',i===v));
}

function loadStatus(){
  fetch('/status').then(r=>r.json()).then(j=>{
    qs('relayState').textContent = j.relayOn ? 'ON' : 'OFF';
    qs('relayState').style.color = j.relayOn ? '#00e676' : '#f87171';
    setRelayMode(j.relayMode||0);
    qs('invert').checked=(j.relayInvert===1);
  });
}
window.onload=()=>{
  loadStatus();
  qs('modeNormal').onclick= ()=>setRelayMode(0);
  qs('modeOn').onclick    = ()=>setRelayMode(1);
  qs('modeOff').onclick   = ()=>setRelayMode(2);
  qs('saveCfg').onclick=()=>{
    qs('cfgStatus').textContent='Salvando...';
    fetch('/relay/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'mode='+relayModeVal+'&invert='+(qs('invert').checked?1:0)})
    .then(r=>r.text()).then(t=>{qs('cfgStatus').textContent=t;loadStatus();});
  };
  qs('btnOn').onclick =()=>fetch('/relay/on').then(()=>loadStatus());
  qs('btnOff').onclick=()=>fetch('/relay/off').then(()=>loadStatus());
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleRelayOn() {
  if (relayMode != 0) { server.send(200, "text/plain", "Modo fixo ativo, comando ignorado."); return; }
  relayLogicalOn = true;
  applyRelayOutput();
  if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
  server.send(200, "text/plain", "Relay ON");
}

void handleRelayOff() {
  if (relayMode != 0) { server.send(200, "text/plain", "Modo fixo ativo, comando ignorado."); return; }
  relayLogicalOn = false;
  applyRelayOutput();
  if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:OFF");
  server.send(200, "text/plain", "Relay OFF");
}

void handleRelayConfigSave() {
  if (server.hasArg("mode")) {
    relayMode = constrain(server.arg("mode").toInt(), 0, 2);
    prefs.putInt("relayMode", relayMode);
  }
  if (server.hasArg("invert")) {
    relayInvert = (server.arg("invert").toInt() == 1);
    prefs.putInt("relayInvert", relayInvert ? 1 : 0);
  }
  updateRelayLevels();
  applyRelayOutput();
  server.send(200, "text/plain", "Configuração do relé salva.");
}

// ========== /nodeid ==========
void handleNodeIdPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Node ID</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0d1710}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}
.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:420px;margin:0 auto;padding:24px 16px 32px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin-bottom:6px}
input{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:3px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none;transition:border-color .15s}
input:focus{border-color:var(--ac)}
.btn{display:block;width:100%;margin-top:16px;padding:13px;background:var(--ac);color:#000;border:none;border-radius:3px;font-family:inherit;font-size:13px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase}
.btn:hover{background:var(--ac2);color:var(--tx)}
.st{margin-top:10px;font-size:12px;min-height:16px;color:var(--ac)}
.note{margin-top:20px;padding:12px;background:var(--cd);border:1px solid var(--bd);border-radius:3px;color:var(--mu);font-size:11px;line-height:1.6}
.note b{color:var(--tx)}
.nav{margin-top:16px;font-size:11px}
.nav a{color:var(--mu);text-decoration:none}
.nav a:hover{color:var(--ac)}
</style></head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">configuração de node id</div>
</header>
<main>
  <label>Node ID</label>
  <input id="nodeid" placeholder="ex: C00045">
  <button class="btn" id="save">Salvar e Reiniciar</button>
  <div class="st" id="status"></div>
  <div class="note">AP: SSID = <b>NodeID-AP</b> | Senha = <b>12345678</b><br>Ativo por 10 min após cada boot.</div>
  <div class="nav"><a href="/">← menu</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
window.onload=()=>{
  fetch('/status').then(r=>r.json()).then(s=>{qs('nodeid').value=s.nodeId||'';});
  qs('save').onclick=()=>{
    let id=qs('nodeid').value.trim();
    if(!id){qs('status').textContent='Preencha o NodeID!';return;}
    qs('status').textContent='Salvando...';
    fetch('/savenodeid',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'nodeid='+encodeURIComponent(id)}).then(r=>r.text()).then(t=>{qs('status').textContent=t;});
  };
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleSaveNodeId() {
  if (!server.hasArg("nodeid")) { server.send(400, "text/plain", "nodeid obrigatório"); return; }
  nodeId = server.arg("nodeid");
  prefs.putString("nodeid", nodeId);
  server.send(200, "text/plain", "NodeID salvo. Reiniciando...");
  delay(400);
  ESP.restart();
}

// ========== /wifistatus ==========
void handleWiFiStatusPage() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  String html =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Foreasy WiFi</title>"
    "<style>"
    ":root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}"
    "header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}"
    ".logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}"
    ".sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}"
    "main{max-width:480px;margin:0 auto;padding:18px 16px}"
    ".it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}"
    ".lbl{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase}"
    ".vl{color:var(--tx);font-size:13px;font-weight:600}"
    ".nav{margin-top:16px;font-size:11px}"
    ".nav a{color:var(--mu);text-decoration:none}"
    ".nav a:hover{color:var(--ac)}"
    "</style></head><body>"
    "<header><div class='logo'>FOREASY</div><div class='sub'>status wi-fi</div></header>"
    "<main>"
    "<div class='it'><span class='lbl'>SSID</span><span class='vl'>" + (staOk ? WiFi.SSID() : String("—")) + "</span></div>"
    "<div class='it'><span class='lbl'>RSSI</span><span class='vl'>" + (staOk ? String(WiFi.RSSI()) + " dBm" : String("—")) + "</span></div>"
    "<div class='it'><span class='lbl'>IP (STA)</span><span class='vl'>" + (staOk ? WiFi.localIP().toString() : String("—")) + "</span></div>"
    "<div class='it'><span class='lbl'>IP (AP)</span><span class='vl'>" + WiFi.softAPIP().toString() + "</span></div>"
    "<div class='it'><span class='lbl'>Slot ativo</span><span class='vl'>Rede " + String(wifiSlot + 1) + "</span></div>"
    "<div class='nav'><a href='/'>← menu</a></div>"
    "</main></body></html>";
  server.send(200, "text/html", html);
}

// ========== /wsstatus ==========
void handleWSStatusPage() {
  String html =
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Foreasy WS</title>"
    "<style>"
    ":root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}"
    "header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}"
    ".logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}"
    ".sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}"
    "main{max-width:480px;margin:0 auto;padding:18px 16px}"
    ".it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}"
    ".lbl{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase}"
    ".vl{color:var(--tx);font-size:13px;font-weight:600}"
    ".nav{margin-top:16px;font-size:11px}"
    ".nav a{color:var(--mu);text-decoration:none}"
    ".nav a:hover{color:var(--ac)}"
    "</style></head><body>"
    "<header><div class='logo'>FOREASY</div><div class='sub'>status websocket</div></header>"
    "<main>"
    "<div class='it'><span class='lbl'>Conectado</span><span class='vl'>" + String(isWebSocketConnected ? "SIM" : "NÃO") + "</span></div>"
    "<div class='it'><span class='lbl'>Servidor</span><span class='vl'>" + String(wsHost) + "</span></div>"
    "<div class='it'><span class='lbl'>Backoff atual</span><span class='vl'>" + String(wsNextRetryMs) + " ms</span></div>"
    "<div class='it'><span class='lbl'>Auto-restart</span><span class='vl'>" + String(wsRestartEnabled ? "SIM" : "NÃO") + "</span></div>"
    "<div class='nav'><a href='/'>← menu</a></div>"
    "</main></body></html>";
  server.send(200, "text/html", html);
}

// ========== /resetwifi ==========
void handleResetWiFi() {
  prefs.remove("ssid");        prefs.remove("pass");
  prefs.remove("ssid2");       prefs.remove("pass2");
  prefs.remove("nodeid");      prefs.remove("relayMode");
  prefs.remove("relayInvert"); prefs.remove("wsrestart");
  prefs.remove("bootCount");
  server.send(200, "text/plain", "Credenciais removidas. Reiniciando...");
  delay(400);
  ESP.restart();
}

// ========== /restart ==========
void handleRestart() {
  server.send(200, "text/plain", "Reiniciando...");
  delay(300);
  ESP.restart();
}

// ========== 404 ==========
void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
