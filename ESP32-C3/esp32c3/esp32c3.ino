// ============================================================================
// Foreasy ESP32-C3 Super Mini
// Hardware: ESP32-C3 + shield relé (Convencional) / saída START IN (Industrial)
//
// PÁGINAS WEB: somente /config (wizard) e /info (status). "/" redireciona p/ /config.
//
// SISTEMA: Industrial ou Convencional — selecionado por machineMode
//
// MODO INDUSTRIAL (machineMode = 1) — padrão de fábrica:
//   Pulso de START IN (Speed Queen, conector H3-7) via GPIO direto (startPin),
//   NÃO usa o relé. GPIO sobe por PULSE_MS (100ms) e desce — igual esp32_avail.
//   WS 0x01 => dispara pulso | WS 0x02 => ignorado
//
//   FAIL-SAFE AVAIL (availEnabled — opcional; modelos sem AVAIL deixam OFF):
//   máquina LIVRE (AVAIL LOW) → pulso → confirma LIVRE→OCUPADA (AVAIL HIGH) em até
//   CREDIT_CONFIRM_MS; se não confirmar, repulsa após CREDIT_GAP_MS, até
//   CREDIT_MAX_ATTEMPTS tentativas. Resposta assíncrona ao backend:
//   "RelayStatus:ON" (sucesso) | "CreditFail" (esgotou) | "CreditBusy" (já ocupada).
//   Sem availEnabled: comportamento antigo (um pulso, sem confirmação).
//
// MODO CONVENCIONAL (machineMode = 0):
//   Controle de energia pelo RELÉ (relayPin).
//   WS 0x01 => RELÉ ON (fica ligado até OFF) | WS 0x02 => RELÉ OFF
//
// CONFIGURAÇÃO DO RELÉ — só vale no Convencional:
// - relayType : 0 = NA — Normalmente Aberto (ON=HIGH) | 1 = NF — Normalmente Fechado (ON=LOW)
//
// PROTOCOLO WEBSOCKET (binário):
// - 0x01 => INDUSTRIAL: pulso START IN 100ms | CONVENCIONAL: Relé ON
// - 0x02 => INDUSTRIAL: ignorado             | CONVENCIONAL: Relé OFF
// - 0x03 => JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw
// - 0x04 => OTA: payload = 0x04 + "url|sha256" (sha256 opcional, 64 hex). Baixa o .bin,
//           grava com Update, valida SHA256 e reinicia. Respostas async:
//           "OTA:QUEUED" → "OTA:START" → "OTA:OK:restart" | "OTA:FAIL:<motivo>"
// - 0x05 => JSON status do AVAIL: {"type":"avail","livre":bool,"raw":-1/0/1,
//           "sinceMs":ms,"availEn":0/1,"machineMode":0/1}
// - 0x06 => Restart remoto. Responde "Restarting" e reinicia após ~300ms.
//
// WIFI:
// - Dual WiFi com failover automático entre rede 1 e rede 2 (sem restart)
// - Conexão não-bloqueante: wifiTick() com timeout 40s e retry a cada 5s
// - Credenciais NUNCA apagadas por falha de conexão
//
// WEBSOCKET:
// - Servidor (host/porta) configurável via /config e salvo na NVS
// - Backoff exponencial: 10s base → 120s máximo
// - Watchdog WS down  : sem WS por >5min  → failover WiFi+WS
// - Watchdog global   : sem WiFi+WS >8min → failover
// - Detecção de zumbi : sem ping/pong por >5min → reconecta
// - App ping a cada 30s | heartbeat: 15s/3s/2 tentativas
// - wsRestartEnabled  : reinicia ESP32-C3 após 1h sem WS (opcional, configurável)
//
// AP: ativo 10 min após boot (lean mode após expirar)
//     SSID: <nodeId>-AP | Senha: 12345678
//
// ARMAZENAMENTO: Preferences (NVS) — ssid, pass, ssid2, pass2, nodeid, machineMode,
//                relayType, wsHost, wsPort, relayPin, startPin, availPin, wsrestart, bootCount
// bootCount incrementado em RAM; salvo apenas no /save (evita desgaste da flash)
//
// SCAN WIFI: assíncrono (não bloqueia o loop)
// TEMPERATURA: sensor interno do ESP32-C3 via temperatureRead()
//
// PINOS (configuráveis via /config; strapping ESP32-C3: GPIO2, GPIO8, GPIO9):
// - ledPin   : GPIO8  (fixo — LED azul integrado, ATIVO LOW)
// - relayPin : GPIO3  (Convencional ON/OFF)
// - startPin : GPIO5  (START IN, ativo HIGH)
// - availPin : GPIO6  (AVAIL OUT, INPUT_PULLUP)
//
// ATENÇÃO LED: GPIO8 tem o LED azul integrado do Super Mini (ativo LOW).
//   Conectado = LOW (LED aceso) | Desconectado = HIGH (LED apagado).
//   Para usar LED externo em outro GPIO, troque ledPin no wizard e ajuste a lógica.
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#define FW_VERSION "1.0.0"   // reportado no 0x03 para auditoria da frota
#define FW_CHIP    "esp32c3" // identifica o chip na telemetria / seleção de OTA

float readInternalTempC() {
  return temperatureRead();
}

WebServer server(80);
WebSocketsClient webSocket;
Preferences prefs;

// ---------- IO (pinos configuráveis via /config, persistidos na NVS) ----------
int ledPin   = 8;   // LED azul integrado do Super Mini — ATIVO LOW (fixo)
int relayPin = 3;   // Convencional: relé ON/OFF
int startPin = 5;   // Industrial: pulso START IN (Speed Queen H3-7), ativo HIGH
int availPin = 6;   // Industrial: leitura AVAIL OUT (Speed Queen H3-4), INPUT_PULLUP

// ---------- Machine Mode ----------
enum MachineMode : uint8_t { MODE_CONVENTIONAL = 0, MODE_INDUSTRIAL = 1 };
MachineMode machineMode = MODE_INDUSTRIAL;
const uint16_t PULSE_MS = 100;
bool     pulseActive = false;
uint32_t pulseEndMs  = 0;

// ---------- Relay (só usado no modo Convencional) ----------
int  relayType      = 0;   // 0=NA, 1=NF
bool relayLogicalOn = false;
int  relayOnLevel   = HIGH;
int  relayOffLevel  = LOW;

// ---------- AVAIL OUT (leitura com debounce) ----------
// LOW = máquina LIVRE | HIGH = máquina OCUPADA  (schematic 807300: "ON=AVAILABLE")
const uint32_t AVAIL_DEBOUNCE_MS = 50;
int      availStable    = -1;   // estado estável (pós-debounce)
int      availReading   = -1;   // última leitura crua
uint32_t availLastMs    = 0;    // marca da última troca crua
uint32_t availStableAtMs = 0;   // quando o estado estável mudou

// ---------- AVAIL fail-safe (Industrial) ----------
// Após o pulso, confirma que a máquina foi LIVRE→OCUPADA. Se não foi, repulsa.
bool availEnabled = false;                 // usar fail-safe? (modelos SEM AVAIL: deixar OFF)
const uint8_t  CREDIT_MAX_ATTEMPTS = 3;    // nº máx. de pulsos antes de desistir
const uint32_t CREDIT_CONFIRM_MS   = 800;  // janela p/ AVAIL ir p/ OCUPADA após o pulso
const uint32_t CREDIT_GAP_MS       = 400;  // espera entre tentativas
enum CreditState : uint8_t { CR_IDLE, CR_PULSE, CR_CONFIRM, CR_GAP };
CreditState creditState   = CR_IDLE;
uint8_t     creditAttempts = 0;
uint32_t    creditTimer    = 0;            // deadline da fase atual

// ---------- Identity ----------
String nodeId = "FOREASY";

// ---------- Credenciais cacheadas (carregadas no boot) ----------
String sSsid, sPass, sSsid2, sPass2;
uint8_t wifiSlot = 0;  // 0=rede1, 1=rede2

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

// ---------- WebSocket (servidor configurável via /config, NVS) ----------
String   wsHost = "frst-back-02b607761078.herokuapp.com";
uint16_t wsPort = 80;
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

// ---------- OTA (requisição enfileirada; executa em otaTick, fora do callback WS) ----------
bool   otaRequested = false;
bool   otaInProgress = false;
String otaPendingUrl;
String otaPendingSha;

// ---------- Restart remoto (enfileirado; executa em restartTick, fora do callback WS) ----------
bool     restartRequested = false;
uint32_t restartAtMs      = 0;

// ================= RELAY HELPERS =================
void updateRelayLevels() {
  if (relayType == 0) { relayOnLevel = HIGH; relayOffLevel = LOW; }
  else                { relayOnLevel = LOW;  relayOffLevel = HIGH; }
}

// Estado lógico "ligado": Industrial = pulso ativo | Convencional = relé ligado
bool isRelayEffectiveOn() {
  if (machineMode == MODE_INDUSTRIAL) return pulseActive;
  return relayLogicalOn;
}

// Relé físico: só atua no Convencional; no Industrial fica sempre OFF
void applyRelayOutput() {
  bool on = (machineMode == MODE_CONVENTIONAL) && relayLogicalOn;
  digitalWrite(relayPin, on ? relayOnLevel : relayOffLevel);
}

// Industrial: dispara pulso no startPin (START IN), ativo HIGH
void startPulse() {
  if (pulseActive) return;
  pulseActive = true;
  pulseEndMs  = millis() + PULSE_MS;
  digitalWrite(startPin, HIGH);
}

// ================= RELAY TICK (fim do pulso) =================
void handleRelayTick() {
  if (pulseActive && (int32_t)(millis() - pulseEndMs) >= 0) {
    pulseActive = false;
    digitalWrite(startPin, LOW);
    if (creditState == CR_IDLE && isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
  }
}

// ================= AVAIL OUT (leitura com debounce) =================
void readAvailTick() {
  int reading = digitalRead(availPin);
  if (reading != availReading) {
    availReading = reading;
    availLastMs  = millis();
  }
  if (reading != availStable && (millis() - availLastMs) >= AVAIL_DEBOUNCE_MS) {
    availStable     = reading;
    availStableAtMs = millis();
  }
}

bool availLivre() { return availStable == LOW; }  // LOW = livre

// ================= CREDIT FAIL-SAFE (Industrial + AVAIL) =================
void creditFirePulse() {
  creditAttempts++;
  pulseActive = true;
  pulseEndMs  = millis() + PULSE_MS;
  digitalWrite(startPin, HIGH);
  creditState = CR_PULSE;
  Serial.printf("CREDIT: pulso %u/%u\n", creditAttempts, CREDIT_MAX_ATTEMPTS);
}

void creditStart() {
  if (creditState != CR_IDLE) return;
  if (!availLivre()) {
    Serial.println("CREDIT: maquina OCUPADA, comando ignorado.");
    if (isWebSocketConnected) webSocket.sendTXT("CreditBusy");
    return;
  }
  creditAttempts = 0;
  creditFirePulse();
}

void creditTick() {
  switch (creditState) {
    case CR_IDLE:
      return;
    case CR_PULSE:
      if (!pulseActive) {
        creditState = CR_CONFIRM;
        creditTimer = millis() + CREDIT_CONFIRM_MS;
      }
      break;
    case CR_CONFIRM:
      if (!availLivre()) {
        creditState = CR_IDLE;
        Serial.println("CREDIT: OK (maquina ocupada).");
        if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
      } else if ((int32_t)(millis() - creditTimer) >= 0) {
        if (creditAttempts < CREDIT_MAX_ATTEMPTS) {
          creditState = CR_GAP;
          creditTimer = millis() + CREDIT_GAP_MS;
        } else {
          creditState = CR_IDLE;
          Serial.println("CREDIT: FALHOU (maquina nao respondeu).");
          if (isWebSocketConnected) webSocket.sendTXT("CreditFail");
        }
      }
      break;
    case CR_GAP:
      if ((int32_t)(millis() - creditTimer) >= 0) creditFirePulse();
      break;
  }
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
  nodeId           = prefs.getString("nodeid",    "FOREASY");
  sSsid            = prefs.getString("ssid",      "");
  sPass            = prefs.getString("pass",      "");
  sSsid2           = prefs.getString("ssid2",     "");
  sPass2           = prefs.getString("pass2",     "");
  machineMode      = (MachineMode)prefs.getInt("machineMode", 1);
  relayType        = prefs.getInt("relayType",    0);
  wsHost           = prefs.getString("wsHost", wsHost);
  wsPort           = (uint16_t)prefs.getInt("wsPort", wsPort);
  relayPin         = prefs.getInt("relayPin", relayPin);
  startPin         = prefs.getInt("startPin", startPin);
  availPin         = prefs.getInt("availPin", availPin);
  availEnabled     = (prefs.getInt("availEn", 0) != 0);
  wsRestartEnabled = (prefs.getInt("wsrestart",   0) != 0);
  bootCount        = prefs.getUInt("bootCount",   0);
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
  webSocket.begin(wsHost.c_str(), wsPort, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);
  lastPingMs = millis();
  lastAppPingMs = 0;
  lastWSConnectAttemptMs = millis();
  Serial.printf("WS begin: %s:%u\n", wsHost.c_str(), wsPort);
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
      lastPingMs  = millis();
      wsLastOkMs  = millis();
      wsDownSinceMs = 0;
      resetWsBackoff();
      String msg = isRelayEffectiveOn() ? ("ID:" + nodeId) : ("NID:" + nodeId);
      webSocket.sendTXT(msg);
      Serial.printf("WS conectado. Sent: %s | mode=%s\n", msg.c_str(),
                    machineMode == MODE_INDUSTRIAL ? "INDUSTRIAL" : "CONVENCIONAL");
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
          char buf[340];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%u,\"block\":%u,\"cpu\":%u,"
            "\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u,\"temp\":%.1f,"
            "\"machineMode\":%u,\"pulse\":%s,\"chip\":\"%s\",\"fw\":\"%s\"}",
            staOk ? WiFi.RSSI() : 0,
            staOk ? (int)WiFi.channel() : 0,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getCpuFreqMHz(),
            (unsigned long)(millis() / 1000UL),
            (unsigned long)bootCount,
            (unsigned)wifiSlot,
            readInternalTempC(),
            (unsigned)machineMode,
            pulseActive ? "true" : "false",
            FW_CHIP,
            FW_VERSION
          );
          webSocket.sendTXT(buf);
          break;
        }

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

        if (b == 0x06) {
          webSocket.sendTXT("Restarting");
          restartRequested = true;
          restartAtMs = millis() + 300;
          break;
        }

        if (b == 0x05) {
          char buf[160];
          snprintf(buf, sizeof(buf),
            "{\"type\":\"avail\",\"livre\":%s,\"raw\":%d,\"sinceMs\":%lu,"
            "\"availEn\":%u,\"machineMode\":%u}",
            availLivre() ? "true" : "false",
            availStable,
            (unsigned long)(millis() - availStableAtMs),
            (unsigned)(availEnabled ? 1 : 0),
            (unsigned)machineMode
          );
          webSocket.sendTXT(buf);
          break;
        }

        bool reply = true;
        if (machineMode == MODE_INDUSTRIAL) {
          if (b == 0x01) {
            if (availEnabled) { creditStart(); reply = false; }
            else              startPulse();
          }
        } else {
          if      (b == 0x01) { relayLogicalOn = true;  applyRelayOutput(); }
          else if (b == 0x02) { relayLogicalOn = false; applyRelayOutput(); }
        }
        if (reply) webSocket.sendTXT(isRelayEffectiveOn() ? "RelayStatus:ON" : "RelayStatus:OFF");
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
  digitalWrite(ledPin, HIGH);  // HIGH = LED apagado (ativo LOW no Super Mini)
  pinMode(relayPin, OUTPUT);
  pinMode(startPin, OUTPUT);
  digitalWrite(startPin, LOW);
  pinMode(availPin, INPUT_PULLUP);
  availReading    = digitalRead(availPin);
  availStable     = availReading;
  availLastMs     = millis();
  availStableAtMs = millis();
  relayLogicalOn = false;
  pulseActive    = false;
  applyRelayOutput();

  bootTimeMs            = millis();
  apEnabled             = true;
  lastConnectivityOkMs  = millis();
  wsLastOkMs            = millis();
  resetWsBackoff();

  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  setupAPSTA();
  startWebServer();
  server.begin();
  Serial.printf("HTTP server iniciado. machineMode=%s chip=%s fw=%s\n",
                machineMode == MODE_INDUSTRIAL ? "INDUSTRIAL" : "CONVENCIONAL",
                FW_CHIP, FW_VERSION);

  WiFi.scanNetworks(true);

  if (hasSavedWiFi()) connectToWiFi_begin();
  else Serial.println("Sem WiFi salvo. Configure pelo AP.");
}

// ================= LOOP =================
void loop() {
  server.handleClient();
  wifiTick();
  wsTick();
  handleRelayTick();
  readAvailTick();
  creditTick();
  watchdogTick();
  apLifetimeTick();
  wsRestartTick();
  otaTick();
  restartTick();

  // GPIO8 LED integrado: ativo LOW (inverso do ESP32-S3)
  digitalWrite(ledPin, isWebSocketConnected ? LOW : HIGH);
}

// ================= TICKS =================
void wifiTick() {
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
  server.on("/",            handleRoot);
  server.on("/config",      handleConfigPage);
  server.on("/config-data", HTTP_GET,  handleConfigData);
  server.on("/info",        handleInfoPage);
  server.on("/scan",        HTTP_GET,  handleScan);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/status",      HTTP_GET,  handleStatusJson);
  server.on("/resetwifi",   HTTP_GET,  handleResetWiFi);
  server.on("/restart",     HTTP_GET,  handleRestart);
  server.onNotFound(handleNotFound);
}

void handleRoot() {
  server.sendHeader("Location", "/config", true);
  server.send(302, "text/plain", "");
}

// ========== /config (wizard step-by-step) ==========
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
.pill{flex:1;display:flex;flex-direction:column;align-items:center;gap:4px;color:var(--mu);font-size:9px;letter-spacing:1px;text-transform:uppercase;transition:color .2s}
.pill b{display:flex;align-items:center;justify-content:center;width:26px;height:26px;border-radius:50%;border:1px solid var(--bd);font-size:12px;background:var(--cd);transition:all .2s}
.pill.cur{color:var(--ac)}.pill.cur b{border-color:var(--ac);color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.12)}
.pill.done{color:var(--lb)}.pill.done b{border-color:var(--ac2);background:#04261447;color:var(--ac)}
.track{height:2px;background:var(--bd);border-radius:2px;overflow:hidden;margin-bottom:18px}
.fill{height:100%;width:0;background:linear-gradient(90deg,var(--ac2),var(--ac));transition:width .35s ease}
.card-wrap{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px 16px;min-height:230px;position:relative;overflow:hidden}
.step{display:none;animation:fade .35s ease}
.step.on{display:block}
@keyframes fade{from{opacity:0;transform:translateX(14px)}to{opacity:1;transform:none}}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:16px 0 8px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
.sec:first-child{margin-top:0}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:11px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none;transition:border-color .15s}
input:focus,select:focus{border-color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.1)}
select option{background:var(--cd)}
.cards{display:flex;gap:8px;margin-top:8px}
.card{flex:1;border:1px solid var(--bd);border-radius:6px;padding:14px 12px;cursor:pointer;background:var(--cd2);transition:all .18s}
.card:hover{border-color:var(--ac2)}
.card.active{border-color:var(--ac);background:#04261433;box-shadow:0 0 0 3px rgba(0,230,118,.1)}
.card .ct{color:var(--tx);font-size:14px;font-weight:700;letter-spacing:1px}
.card.active .ct{color:var(--ac)}
.card .cd{color:var(--mu);font-size:10px;line-height:1.5;margin-top:6px}
.chk{display:flex;align-items:center;gap:10px;margin-top:14px;padding:10px 12px;border:1px solid var(--bd);border-radius:4px;background:var(--cd2)}
.chk input{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}
.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0;cursor:pointer}
.hint{color:var(--mu);font-size:10px;line-height:1.5;margin-top:10px;padding:8px 10px;border-left:2px solid var(--bd)}
.nav{display:flex;gap:8px;margin-top:16px}
.btn{flex:1;padding:13px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000;transition:all .15s}
.btn:hover{background:var(--ac2);color:var(--tx)}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu);flex:0 0 110px}
.btn.ghost:hover{border-color:var(--ac);color:var(--ac)}
.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}
.foot{margin-top:16px;text-align:center;font-size:11px}
.foot a{color:var(--mu);text-decoration:none}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">esp32-c3 super mini</div></header>
<main>
  <div class="steps">
    <div class="pill cur" id="pill0"><b>1</b><span>Rede</span></div>
    <div class="pill" id="pill1"><b>2</b><span>Servidor</span></div>
    <div class="pill" id="pill2"><b>3</b><span>Modo</span></div>
    <div class="pill" id="pill3"><b>4</b><span>Pinos</span></div>
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
      <div class="sec">Rede 2 — failover (opcional)</div>
      <select id="ssid2_scan"></select>
      <label>Ou SSID manual</label>
      <input id="ssid2" placeholder="opcional">
      <label>Senha</label>
      <input id="pass2" type="text" placeholder="opcional">
      <div class="sec">Identificação</div>
      <label>Node ID</label>
      <input id="nodeid" placeholder="ex: C00045">
    </div>

    <div class="step" id="step1">
      <div class="sec">Servidor — WebSocket</div>
      <label>Host</label>
      <input id="host" placeholder="ex: frst-back-....herokuapp.com">
      <label>Porta</label>
      <input id="port" type="number" min="1" max="65535" placeholder="80">
      <div class="chk"><input id="wsrestart" type="checkbox"><label for="wsrestart">Reiniciar sozinho se ficar 1h sem WebSocket</label></div>
      <div class="hint">O servidor é salvo na memória (NVS) e usado para a conexão WebSocket após reiniciar.</div>
    </div>

    <div class="step" id="step2">
      <div class="sec">Modo da máquina</div>
      <div class="cards" style="flex-direction:column">
        <div class="card" id="mCon"><div class="ct">Convencional</div><div class="cd">Controle de energia pelo RELÉ (liga/desliga). Funciona com qualquer lavadora.</div></div>
        <div class="card active" id="mInd"><div class="ct">Industrial</div><div class="cd">Pulso elétrico no START IN (Speed Queen) + leitura do AVAIL OUT. Não usa o relé.</div></div>
      </div>
    </div>

    <div class="step" id="step3">
      <div id="pinsCon">
        <div class="sec">Pinos — Convencional</div>
        <label>GPIO do relé</label>
        <input id="relayPin" type="number" min="0" max="21">
        <div class="sec">Tipo do relé</div>
        <div class="cards">
          <div class="card active" id="tNA"><div class="ct">NA</div><div class="cd">Normalmente Aberto · ON = HIGH</div></div>
          <div class="card" id="tNF"><div class="ct">NF</div><div class="cd">Normalmente Fechado · ON = LOW</div></div>
        </div>
      </div>
      <div id="pinsInd">
        <div class="sec">Pinos — Industrial</div>
        <label>GPIO START IN (pulso, ativo HIGH)</label>
        <input id="startPin" type="number" min="0" max="21">
        <label>GPIO AVAIL OUT (leitura status)</label>
        <input id="availPin" type="number" min="0" max="21">
        <div class="chk"><input id="availEn" type="checkbox"><label for="availEn">Usar AVAIL como fail-safe (confirma o ciclo e repulsa se a máquina não ligar)</label></div>
        <div class="hint">Evite strapping pins do C3: GPIO2, 8, 9. AVAIL usa pull-up interno (sem resistor externo). <b>Modelos sem AVAIL: deixe o fail-safe desmarcado.</b></div>
      </div>
    </div>
  </div>

  <div class="nav">
    <button class="btn ghost" id="back">Voltar</button>
    <button class="btn" id="next">Avançar</button>
  </div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/info">→ status do dispositivo (/info)</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
const N=4; let cur=0, mm=1, rt=0;

function paint(){
  for(let i=0;i<N;i++){
    qs('step'+i).classList.toggle('on', i===cur);
    qs('pill'+i).classList.toggle('done', i<cur);
    qs('pill'+i).classList.toggle('cur', i===cur);
  }
  qs('bar').style.width=(cur/(N-1)*100)+'%';
  qs('back').style.visibility=cur?'visible':'hidden';
  qs('next').textContent=cur===N-1?'Salvar e Reiniciar':'Avançar';
  if(cur===3) paintPins();
}
function setMode(v){mm=v;qs('mCon').classList.toggle('active',v===0);qs('mInd').classList.toggle('active',v===1);}
function setType(v){rt=v;qs('tNA').classList.toggle('active',v===0);qs('tNF').classList.toggle('active',v===1);}
function paintPins(){qs('pinsCon').style.display=mm===0?'block':'none';qs('pinsInd').style.display=mm===1?'block':'none';}
function msg(t){qs('msg').textContent=t;}
function valid(){
  if(cur===0){
    let ss=qs('manual_ssid').value.trim()||qs('ssid').value;
    if(!ss){msg('Selecione ou digite a rede 1');return false;}
    if(!qs('nodeid').value.trim()){msg('Preencha o Node ID');return false;}
  }
  if(cur===1 && !qs('host').value.trim()){msg('Preencha o servidor');return false;}
  msg('');return true;
}
function next(){ if(cur===N-1){return save();} if(!valid())return; cur++; paint(); }
function back(){ if(cur>0){cur--; paint();} }
function save(){
  let ss=qs('manual_ssid').value.trim()||qs('ssid').value;
  let ss2=qs('ssid2').value.trim()||qs('ssid2_scan').value;
  msg('Salvando...');
  let b='ssid='+encodeURIComponent(ss)+
    '&pass='+encodeURIComponent(qs('pass').value)+
    '&ssid2='+encodeURIComponent(ss2)+
    '&pass2='+encodeURIComponent(qs('pass2').value)+
    '&nodeid='+encodeURIComponent(qs('nodeid').value.trim())+
    '&host='+encodeURIComponent(qs('host').value.trim())+
    '&port='+encodeURIComponent(qs('port').value||80)+
    '&machineMode='+mm+'&relayType='+rt+
    '&relayPin='+(qs('relayPin').value||3)+
    '&startPin='+(qs('startPin').value||5)+
    '&availPin='+(qs('availPin').value||6)+
    '&availEn='+(qs('availEn').checked?1:0)+
    '&wsrestart='+(qs('wsrestart').checked?1:0);
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
    .then(r=>r.text()).then(t=>{msg(t+' Reconecte ao Wi-Fi em ~5s.');})
    .catch(()=>msg('Falha ao salvar.'));
}
function encText(e){return['Open','WEP','WPA-PSK','WPA2-PSK','WPA/WPA2'][e]||'?';}
function scan(retry){
  retry=retry||0;
  fetch('/scan').then(r=>r.json()).then(list=>{
    if(list.length===0&&retry<6){setTimeout(()=>scan(retry+1),2500);return;}
    let s=qs('ssid'); s.innerHTML='';
    let s2=qs('ssid2_scan'); s2.innerHTML='<option value="">— nenhuma —</option>';
    list.forEach(i=>{
      let o=document.createElement('option'); o.value=i.ssid;
      o.textContent=i.ssid+' · '+i.rssi+'dBm · '+encText(i.enc);
      s.appendChild(o); s2.appendChild(o.cloneNode(true));
    });
  }).catch(()=>{if(retry<6)setTimeout(()=>scan(retry+1),2500);});
}
window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('manual_ssid').value=d.ssid||''; qs('pass').value=d.pass||'';
    qs('ssid2').value=d.ssid2||''; qs('pass2').value=d.pass2||'';
    qs('nodeid').value=d.nodeid||'';
    qs('host').value=d.host||''; qs('port').value=d.port||80;
    qs('relayPin').value=d.relayPin!=null?d.relayPin:3;
    qs('startPin').value=d.startPin!=null?d.startPin:5;
    qs('availPin').value=d.availPin!=null?d.availPin:6;
    qs('availEn').checked=(d.availEn===1);
    qs('wsrestart').checked=(d.wsrestart===1);
    setMode(d.machineMode!=null?d.machineMode:1);
    setType(d.relayType||0);
    paintPins();
  }).catch(()=>{});
  scan();
  qs('mCon').onclick=()=>{setMode(0);paintPins();};
  qs('mInd').onclick=()=>{setMode(1);paintPins();};
  qs('tNA').onclick=()=>setType(0);
  qs('tNF').onclick=()=>setType(1);
  qs('next').onclick=next; qs('back').onclick=back;
  paint();
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /config-data ==========
void handleConfigData() {
  String json = "{";
  json += "\"ssid\":"        + (sSsid.length()  ? "\""+sSsid+"\""  : "\"\"") + ",";
  json += "\"pass\":"        + (sPass.length()  ? "\""+sPass+"\""  : "\"\"") + ",";
  json += "\"ssid2\":"       + (sSsid2.length() ? "\""+sSsid2+"\"" : "\"\"") + ",";
  json += "\"pass2\":"       + (sPass2.length() ? "\""+sPass2+"\"" : "\"\"") + ",";
  json += "\"nodeid\":\""    + nodeId + "\",";
  json += "\"machineMode\":" + String((int)machineMode) + ",";
  json += "\"relayType\":"   + String(relayType) + ",";
  json += "\"host\":\""      + wsHost + "\",";
  json += "\"port\":"        + String(wsPort) + ",";
  json += "\"relayPin\":"    + String(relayPin) + ",";
  json += "\"startPin\":"    + String(startPin) + ",";
  json += "\"availPin\":"    + String(availPin) + ",";
  json += "\"availEn\":"     + String(availEnabled ? 1 : 0) + ",";
  json += "\"wsrestart\":"   + String(wsRestartEnabled ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

// ========== /save ==========
void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("nodeid")) {
    server.send(400, "text/plain", "ssid e nodeid obrigatórios");
    return;
  }

  sSsid  = server.arg("ssid");
  sPass  = server.arg("pass");
  sSsid2 = server.arg("ssid2");
  sPass2 = server.arg("pass2");
  nodeId = server.arg("nodeid");

  machineMode = server.hasArg("machineMode") ? (MachineMode)constrain(server.arg("machineMode").toInt(), 0, 1) : MODE_INDUSTRIAL;
  relayType   = server.hasArg("relayType") ? constrain(server.arg("relayType").toInt(), 0, 1) : 0;
  if (server.hasArg("host")) wsHost = server.arg("host");
  if (server.hasArg("port")) wsPort = (uint16_t)constrain(server.arg("port").toInt(), 1, 65535);
  if (server.hasArg("relayPin")) relayPin = constrain(server.arg("relayPin").toInt(), 0, 21);
  if (server.hasArg("startPin")) startPin = constrain(server.arg("startPin").toInt(), 0, 21);
  if (server.hasArg("availPin")) availPin = constrain(server.arg("availPin").toInt(), 0, 21);
  availEnabled = server.hasArg("availEn") && server.arg("availEn").toInt() == 1;
  wsRestartEnabled = server.hasArg("wsrestart") && server.arg("wsrestart").toInt() == 1;
  wifiSlot = 0;

  prefs.putString("ssid",      sSsid);
  prefs.putString("pass",      sPass);
  prefs.putString("ssid2",     sSsid2);
  prefs.putString("pass2",     sPass2);
  prefs.putString("nodeid",    nodeId);
  prefs.putInt("machineMode",  (int)machineMode);
  prefs.putInt("relayType",    relayType);
  prefs.putString("wsHost",    wsHost);
  prefs.putInt("wsPort",       wsPort);
  prefs.putInt("relayPin",     relayPin);
  prefs.putInt("startPin",     startPin);
  prefs.putInt("availPin",     availPin);
  prefs.putInt("availEn",      availEnabled ? 1 : 0);
  prefs.putInt("wsrestart",    wsRestartEnabled ? 1 : 0);
  prefs.putUInt("bootCount",   bootCount);

  updateRelayLevels();
  applyRelayOutput();

  server.send(200, "text/plain", "Configurado. Reiniciando...");
  delay(400);
  ESP.restart();
}

// ========== /status ==========
void handleStatusJson() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  String json = "{";
  json += "\"nodeId\":\""     + nodeId + "\",";
  json += "\"ssid\":\""       + (staOk ? WiFi.SSID() : String("")) + "\",";
  json += "\"rssi\":"         + String(staOk ? WiFi.RSSI() : 0) + ",";
  json += "\"ip_sta\":\""     + (staOk ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"ip_ap\":\""      + WiFi.softAPIP().toString() + "\",";
  json += "\"wsConnected\":"  + String(isWebSocketConnected ? "true" : "false") + ",";
  json += "\"host\":\""       + wsHost + "\",";
  json += "\"port\":"         + String(wsPort) + ",";
  json += "\"temp\":"         + String(readInternalTempC(), 1) + ",";
  json += "\"relayOn\":"      + String(isRelayEffectiveOn() ? "true" : "false") + ",";
  json += "\"machineMode\":"  + String((int)machineMode) + ",";
  json += "\"pulseActive\":"  + String(pulseActive ? "true" : "false") + ",";
  json += "\"relayType\":"    + String(relayType) + ",";
  json += "\"relayPin\":"     + String(relayPin) + ",";
  json += "\"startPin\":"     + String(startPin) + ",";
  json += "\"availPin\":"     + String(availPin) + ",";
  json += "\"availRaw\":"     + String(availStable) + ",";
  json += "\"availLivre\":"   + String(availLivre() ? "true" : "false") + ",";
  json += "\"availSinceMs\":" + String(millis() - availStableAtMs) + ",";
  json += "\"availEn\":"      + String(availEnabled ? 1 : 0) + ",";
  json += "\"creditState\":"  + String((int)creditState) + ",";
  json += "\"wifiSlot\":"     + String(wifiSlot) + ",";
  json += "\"boots\":"        + String(bootCount);
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
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px;min-height:100vh}
header{padding:18px 18px 6px;text-align:center}
.logo{color:var(--ac);font-size:20px;font-weight:700;letter-spacing:4px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px;text-transform:uppercase}
main{max-width:900px;margin:0 auto;padding:10px 16px 28px}
.big{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px;text-align:center;margin:12px 0}
.big .l{color:var(--mu);font-size:10px;letter-spacing:2px;text-transform:uppercase}
.big .v{font-size:30px;font-weight:700;margin-top:6px;color:var(--ac)}
.big .s{color:var(--mu);font-size:11px;margin-top:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:8px}
.it{background:var(--cd);border:1px solid var(--bd);border-radius:6px;padding:11px 13px}
.lb{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}
.vl{color:var(--tx);font-size:14px;font-weight:600;word-break:break-all}
pre{font-size:11px;background:#060a07;color:#4ade80;padding:10px;border-radius:6px;max-height:200px;overflow:auto;margin-top:14px;border:1px solid var(--bd);white-space:pre-wrap}
.nav{margin-top:16px;font-size:11px;display:flex;gap:18px;flex-wrap:wrap}
.nav a{color:var(--mu);text-decoration:none;cursor:pointer}.nav a:hover{color:var(--ac)}
.nav a.danger:hover{color:var(--red)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">esp32-c3 · status — atualiza a cada 2s</div></header>
<main>
  <div class="big">
    <div class="l">AVAIL OUT — status da máquina</div>
    <div class="v" id="avail">…</div>
    <div class="s" id="availRaw">…</div>
  </div>
  <div class="grid">
    <div class="it"><div class="lb">Node ID</div><div class="vl" id="node">…</div></div>
    <div class="it"><div class="lb">Modo</div><div class="vl" id="mm">…</div></div>
    <div class="it"><div class="lb">WebSocket</div><div class="vl" id="ws">…</div></div>
    <div class="it"><div class="lb">Servidor</div><div class="vl" id="srv">…</div></div>
    <div class="it"><div class="lb">Wi-Fi (STA)</div><div class="vl" id="wifi">…</div></div>
    <div class="it"><div class="lb">RSSI</div><div class="vl" id="rssi">…</div></div>
    <div class="it"><div class="lb">IP (STA)</div><div class="vl" id="ipsta">…</div></div>
    <div class="it"><div class="lb">IP (AP)</div><div class="vl" id="ipap">…</div></div>
    <div class="it"><div class="lb">Relé / Pulso</div><div class="vl" id="relay">…</div></div>
    <div class="it"><div class="lb">Pinos (R/S/A)</div><div class="vl" id="pins">…</div></div>
    <div class="it"><div class="lb">Temp</div><div class="vl" id="temp">…</div></div>
    <div class="it"><div class="lb">Rede ativa</div><div class="vl" id="slot">…</div></div>
    <div class="it"><div class="lb">Boots</div><div class="vl" id="boots">…</div></div>
  </div>
  <div class="nav">
    <a href="/config">← configuração</a>
    <a id="rst">reiniciar</a>
    <a class="danger" id="clr">apagar config</a>
  </div>
  <pre id="raw"></pre>
</main>
<script>
const mmLabel=['Convencional','Industrial'];
function g(id){return document.getElementById(id);}
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    g('avail').textContent = j.availLivre?'LIVRE':'OCUPADA';
    g('avail').style.color = j.availLivre?'#00e676':'#f87171';
    let raw = j.availRaw===0?'LOW':(j.availRaw===1?'HIGH':'—');
    let fs = j.availEn ? (['idle','pulsando','confirmando','retry'][j.creditState]||'?') : 'desligado';
    g('availRaw').textContent = raw+' · há '+(j.availSinceMs/1000).toFixed(1)+'s · fail-safe: '+fs;
    g('node').textContent=j.nodeId||'—';
    g('mm').textContent=mmLabel[j.machineMode]||j.machineMode;
    g('ws').textContent=j.wsConnected?'Conectado':'Desconectado';
    g('srv').textContent=(j.host||'—')+':'+j.port;
    g('wifi').textContent=j.ssid||'—';
    g('rssi').textContent=(j.rssi||0)+' dBm';
    g('ipsta').textContent=j.ip_sta||'—';
    g('ipap').textContent=j.ip_ap||'—';
    g('relay').textContent=(j.relayOn?'ON':'OFF')+(j.pulseActive?' · pulso':'');
    g('pins').textContent='R'+j.relayPin+' · S'+j.startPin+' · A'+j.availPin;
    g('temp').textContent=j.temp+'°C';
    g('slot').textContent='Rede '+(j.wifiSlot+1);
    g('boots').textContent=j.boots;
    g('raw').textContent=JSON.stringify(j,null,2);
  }).catch(()=>{});
}
g('rst').onclick=()=>{if(confirm('Reiniciar o dispositivo?'))fetch('/restart');};
g('clr').onclick=()=>{if(confirm('Apagar TODA a configuração e reiniciar?'))fetch('/resetwifi');};
setInterval(upd,2000); upd();
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /scan ==========
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
            ",\"enc\":"     + (int)WiFi.encryptionType(i) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  server.send(200, "application/json", json);
}

// ========== /resetwifi ==========
void handleResetWiFi() {
  prefs.clear();
  server.send(200, "text/plain", "Configuração apagada. Reiniciando...");
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
