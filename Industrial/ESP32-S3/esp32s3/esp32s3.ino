// ============================================================================
// Foreasy ESP32-S3 — Industrial
// Hardware: ESP32-S3 + saída START IN (Industrial)
//
// PÁGINAS WEB: somente /config (wizard) e /info (status). "/" redireciona p/ /config.
//
// SISTEMA: Industrial — pulso de START IN + leitura do AVAIL OUT (Speed Queen).
//   Pulso de START IN (Speed Queen, conector H3-7) via GPIO direto (startPin).
//   GPIO sobe por PULSE_MS (100ms) e desce — igual esp32_avail.
//   WS 0x01 => dispara pulso | WS 0x02 => ignorado
//
//   FAIL-SAFE AVAIL (availEnabled — opcional; modelos sem AVAIL deixam OFF):
//   máquina LIVRE (AVAIL LOW) → pulso → confirma LIVRE→OCUPADA (AVAIL HIGH) em até
//   CREDIT_CONFIRM_MS; se não confirmar, repulsa após CREDIT_GAP_MS, até
//   CREDIT_MAX_ATTEMPTS tentativas. Resposta assíncrona ao backend:
//   "RelayStatus:ON" (sucesso) | "CreditFail" (esgotou) | "CreditBusy" (já ocupada).
//   Sem availEnabled: comportamento antigo (um pulso, sem confirmação).
//
// PROTOCOLO WEBSOCKET (binário):
// - 0x01 => pulso START IN 100ms
// - 0x02 => ignorado
// - 0x03 => JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw
// - 0x04 => OTA: payload = 0x04 + "url|sha256" (sha256 opcional, 64 hex). Baixa o .bin,
//           grava com Update, valida SHA256 e reinicia. Respostas async:
//           "OTA:QUEUED" → "OTA:START" → "OTA:OK:restart" | "OTA:FAIL:<motivo>"
// - 0x05 => JSON status do AVAIL: {"type":"avail","livre":bool,"raw":-1/0/1,
//           "sinceMs":ms,"availEn":0/1,"machineMode":1}
// - 0x06 => Restart remoto. Responde "Restarting" e reinicia após ~300ms.
//
// DETECÇÃO DE FICHA (token) — só com availEnabled e creditState IDLE (§11):
// - "TokenInserted" : AVAIL LOW→HIGH externo (ficha) ou snapshot ocupado no connect
// - "TokenFinished" : AVAIL HIGH→LOW externo ou snapshot livre no connect
//   O backend cruza com is_in_use antes de marcar a ficha (não toca is_in_use).
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
// - wsRestartEnabled  : reinicia ESP32-S3 após 1h sem WS (opcional, configurável)
//
// AP: ativo 10 min após boot (lean mode após expirar)
//     SSID: <nodeId>-AP | Senha: 12345678
//
// ARMAZENAMENTO: Preferences (NVS) — ssid, pass, ssid2, pass2, nodeid,
//                wsHost, wsPort, startPin, availPin, availEn, wsrestart, bootCount
// bootCount incrementado em RAM; salvo apenas no /save (evita desgaste da flash)
//
// SCAN WIFI: assíncrono (não bloqueia o loop)
// TEMPERATURA: sensor interno do ESP32-S3 via temperatureRead()
//
// PINOS (configuráveis via /config; strapping ESP32-S3: GPIO0, GPIO3, GPIO45, GPIO46):
// - ledPin   : GPIO4 (fixo)
// - startPin : GPIO5 (START IN, ativo HIGH)   - availPin : GPIO4 (AVAIL OUT, INPUT_PULLUP)
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#define FW_VERSION "1.0.0"   // reportado no 0x03 para auditoria da frota (ver PLANO-NOVA-VERSAO.md §1.6)
#define FW_CHIP    "esp32s3" // identifica o chip na telemetria / seleção de OTA

float readInternalTempC() {
  return temperatureRead();
}

WebServer server(80);
WebSocketsClient webSocket;
Preferences prefs;

// ---------- IO (pinos configuráveis via /config, persistidos na NVS) ----------
int ledPin   = 4;   // LED status WS (fixo)
int startPin = 5;   // pulso START IN (Speed Queen H3-7), ativo HIGH
int availPin = 4;   // leitura AVAIL OUT (Speed Queen H3-4), INPUT_PULLUP

// ---------- Pulso START IN ----------
const uint16_t PULSE_MS = 100;
bool     pulseActive = false;
uint32_t pulseEndMs  = 0;

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
const uint32_t CREDIT_CONFIRM_MS   = 800;  // janela p/ AVAIL ir p/ OCUPADA após o pulso (ALPM ~500ms)
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

// ---------- Wizard: teste ao vivo de WiFi/WS (não-bloqueante) ----------
// Durante um teste, suspendemos o WiFi/WS de produção (wifiTestActive) para o STA
// poder testar credenciais candidatas sem comprometer a conexão salva.
enum TestState : uint8_t { TST_IDLE, TST_RUN, TST_OK, TST_FAIL };
TestState wifiTestState   = TST_IDLE;
bool      wifiTestActive  = false;
uint32_t  wifiTestStartMs = 0;
const uint32_t WIFI_TEST_TIMEOUT_MS = 12000;
String    wifiTestSsid, wifiTestPass;
String    lastGoodSsid, lastGoodPass;   // última rede testada com sucesso (p/ o teste de WS)

// ================= PULSO START IN =================
// dispara pulso no startPin (START IN), ativo HIGH — igual esp32_avail
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
    // Pulso simples (sem fail-safe) reporta aqui; com fail-safe, quem reporta é creditTick().
    if (creditState == CR_IDLE && isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
  }
}

// ================= AVAIL OUT (leitura com debounce) =================
// Lê o status da máquina e notifica transições externas (ficha) ao backend (§11.4a).
void readAvailTick() {
  int reading = digitalRead(availPin);
  if (reading != availReading) {
    availReading = reading;
    availLastMs  = millis();
  }
  if (reading != availStable && (millis() - availLastMs) >= AVAIL_DEBOUNCE_MS) {
    int prevStable  = availStable;          // salva antes de atualizar
    availStable     = reading;
    availStableAtMs = millis();

    // Detecção de ficha (§11): creditState == CR_IDLE garante que a transição NÃO foi do
    // nosso creditTick. O backend ainda cruza com is_in_use antes de marcar ficha (§11.1).
    if (availEnabled && isWebSocketConnected && creditState == CR_IDLE) {
      if      (prevStable == LOW  && availStable == HIGH) webSocket.sendTXT("TokenInserted");
      else if (prevStable == HIGH && availStable == LOW)  webSocket.sendTXT("TokenFinished");
    }
  }
}

bool availLivre() { return availStable == LOW; }  // LOW = livre

// ================= CREDIT FAIL-SAFE (Industrial + AVAIL) =================
// Máquina de estados não-bloqueante: pulsa e confirma que ficou OCUPADA; senão repulsa.
void creditFirePulse() {
  creditAttempts++;
  pulseActive = true;
  pulseEndMs  = millis() + PULSE_MS;
  digitalWrite(startPin, HIGH);
  creditState = CR_PULSE;
  Serial.printf("CREDIT: pulso %u/%u\n", creditAttempts, CREDIT_MAX_ATTEMPTS);
}

// Chamado ao receber WS 0x01 no Industrial com availEnabled.
void creditStart() {
  if (creditState != CR_IDLE) return;        // sequência já em andamento → ignora
  if (!availLivre()) {                        // máquina ocupada → não envia crédito
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
      if (!pulseActive) {                     // pulso terminou (handleRelayTick baixou o pino)
        creditState = CR_CONFIRM;
        creditTimer = millis() + CREDIT_CONFIRM_MS;
      }
      break;
    case CR_CONFIRM:
      if (!availLivre()) {                     // AVAIL = OCUPADA → sucesso
        creditState = CR_IDLE;
        Serial.println("CREDIT: OK (maquina ocupada).");
        if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
      } else if ((int32_t)(millis() - creditTimer) >= 0) {  // janela expirou, ainda livre
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
  wsHost           = prefs.getString("wsHost", wsHost);
  wsPort           = (uint16_t)prefs.getInt("wsPort", wsPort);
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

// ================= TESTE AO VIVO (wizard) =================
// Conecta o STA na rede candidata enquanto o AP segue de pé. Para reduzir a queda do
// celular (rádio único), casa o canal do AP com o da rede alvo antes de conectar.
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

// Lê exatamente `len` bytes do cliente até o deadline. Retorna true se completou.
static bool wsReadN(WiFiClient* c, uint8_t* buf, size_t len, uint32_t deadline) {
  size_t got = 0;
  while (got < len && (int32_t)(deadline - millis()) > 0) {
    if (c->available()) buf[got++] = (uint8_t)c->read();
    else delay(2);
  }
  return got == len;
}

// Extrai o valor de uma chave "key":"valor" de um JSON simples (sem lib).
static String jsonStr(const String& src, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int i = src.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int e = src.indexOf('"', i);
  return (e < 0) ? "" : src.substring(i, e);
}

// Teste de WebSocket (síncrono): faz o handshake, envia "WhoAmI:<nodeId>" e lê a
// resposta do servidor com o prédio/máquina (o backend responde "WhoAmI:{json}").
// Garante WiFi reconectando na última rede testada, se necessário. Retorna true se
// conectou (handshake 101); `found` indica se o servidor reconheceu o nodeId.
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
  if (resp.indexOf("101") < 0) { c->stop(); return false; }   // não é endpoint WS

  // Envia "WhoAmI:<nodeId>" como frame de texto mascarado (cliente DEVE mascarar).
  String payload = "WhoAmI:" + nodeId;
  size_t n = payload.length();
  if (n < 126) {
    uint8_t mk[4]  = { 0x21, 0x53, 0xAE, 0x42 };
    uint8_t hdr[2] = { 0x81, (uint8_t)(0x80 | n) };
    c->write(hdr, 2); c->write(mk, 4);
    for (size_t i = 0; i < n; i++) { uint8_t b = (uint8_t)payload[i] ^ mk[i & 3]; c->write(&b, 1); }
    c->flush();

    // Lê o frame de resposta (texto, NÃO mascarado pelo servidor).
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
// Esboço do PLANO-NOVA-VERSAO.md §1.5: baixa o .bin, grava com Update, valida SHA256, reinicia.
// Bloqueante por natureza — por isso roda em otaTick() (loop), nunca dentro do callback WS.
void otaReport(String s) {
  Serial.println(s);
  if (isWebSocketConnected) webSocket.sendTXT(s);
}

bool doOTA(const String& url, const String& expectedSha) {
  if (WiFi.status() != WL_CONNECTED) { otaReport("OTA:FAIL:nowifi"); return false; }
  if (url.length() == 0)             { otaReport("OTA:FAIL:nourl");  return false; }

  otaInProgress = true;
  otaReport("OTA:START");

  // HTTP (plain) ou HTTPS conforme a URL. setInsecure: sem pinning (ver §1.6 p/ endurecer).
  bool https = url.startsWith("https");
  WiFiClient       plainClient;
  WiFiClientSecure secureClient;
  if (https) secureClient.setInsecure();
  WiFiClient* netClient = https ? (WiFiClient*)&secureClient : &plainClient;

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // GitHub/R2 redirecionam p/ outro host
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
      if ((millis() - lastDataMs) > 15000) {   // stream travou
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
  return true;   // não retorna (reiniciou)
}

// Executado no loop: dispara o OTA pendente fora do contexto do callback WS.
void otaTick() {
  if (!otaRequested) return;
  otaRequested = false;
  doOTA(otaPendingUrl, otaPendingSha);
  otaInProgress = false;   // só chega aqui se o OTA falhou (sucesso reinicia)
}

// Executado no loop: reinicia após o WS 0x06, dando tempo da resposta "Restarting" sair.
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
      String msg = pulseActive ? ("ID:" + nodeId) : ("NID:" + nodeId);
      webSocket.sendTXT(msg);
      // Snapshot do AVAIL na reconexão (§11.4b): resolve ficha iniciada/encerrada com o WS
      // caído. O backend cruza com is_in_use, então em uso nosso legítimo vira no-op.
      if (availEnabled && creditState == CR_IDLE) {
        webSocket.sendTXT(availLivre() ? "TokenFinished" : "TokenInserted");
      }
      Serial.printf("WS conectado. Sent: %s\n", msg.c_str());
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
            "\"machineMode\":1,\"pulse\":%s,\"chip\":\"%s\",\"fw\":\"%s\"}",
            staOk ? WiFi.RSSI() : 0,
            staOk ? (int)WiFi.channel() : 0,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getCpuFreqMHz(),
            (unsigned long)(millis() / 1000UL),
            (unsigned long)bootCount,
            (unsigned)wifiSlot,
            readInternalTempC(),
            pulseActive ? "true" : "false",
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

        // 0x05 => status do AVAIL OUT
        if (b == 0x05) {
          char buf[160];
          snprintf(buf, sizeof(buf),
            "{\"type\":\"avail\",\"livre\":%s,\"raw\":%d,\"sinceMs\":%lu,"
            "\"availEn\":%u,\"machineMode\":1}",
            availLivre() ? "true" : "false",
            availStable,
            (unsigned long)(millis() - availStableAtMs),
            (unsigned)(availEnabled ? 1 : 0)
          );
          webSocket.sendTXT(buf);
          break;
        }

        bool reply = true;
        if (b == 0x01) {
          if (availEnabled) { creditStart(); reply = false; }  // resposta vem async (creditTick)
          else              startPulse();
        }
        // 0x02 ignorado
        if (reply) webSocket.sendTXT(pulseActive ? "RelayStatus:ON" : "RelayStatus:OFF");
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

  bootCount++;  // incrementa em RAM; salvo apenas no /save

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(startPin, OUTPUT);
  digitalWrite(startPin, LOW);
  pinMode(availPin, INPUT_PULLUP);
  availReading    = digitalRead(availPin);
  availStable     = availReading;
  availLastMs     = millis();
  availStableAtMs = millis();
  pulseActive    = false;

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
  Serial.printf("HTTP server iniciado. modo=INDUSTRIAL\n");

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
  handleRelayTick();
  readAvailTick();
  creditTick();
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
    // Se um teste do wizard ficou pendente (config abandonada), retoma a produção.
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
  if (pulseActive)           { wsLastOkMs = millis(); return; }  // não reinicia durante o pulso
  if ((millis() - wsLastOkMs) > WS_RESTART_TIMEOUT_MS) {
    Serial.println("WS_RESTART: sem WS por 1h. Reiniciando.");
    delay(200);
    ESP.restart();
  }
}

// ================= HTTP ROUTES =================
void startWebServer() {
  server.on("/",            handleRoot);            // landing: Wizard / Administrador
  server.on("/wizard",      handleConfigPage);      // assistente passo a passo
  server.on("/config",      handleConfigPage);      // alias (compat)
  server.on("/admin",       handleAdminPage);       // edições pontuais
  server.on("/config-data", HTTP_GET,  handleConfigData);
  server.on("/info",        handleInfoPage);
  server.on("/scan",        HTTP_GET,  handleScan);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/status",      HTTP_GET,  handleStatusJson);
  server.on("/test-wifi",        HTTP_GET, handleTestWifi);
  server.on("/test-wifi-status", HTTP_GET, handleTestWifiStatus);
  server.on("/test-ws",          HTTP_GET, handleTestWs);
  server.on("/resetwifi",   HTTP_GET,  handleResetWiFi);
  server.on("/restart",     HTTP_GET,  handleRestart);
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
.foot{margin-top:6px;text-align:center;font-size:11px}
.foot a{color:var(--mu);text-decoration:none}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">esp32-s3 · configuração</div></header>
<main>
  <a class="card" href="/wizard"><div class="ct">▶ Assistente (Wizard)</div><div class="cd">Configuração guiada passo a passo: testa o Wi-Fi e o servidor antes de salvar. Recomendado na primeira instalação.</div></a>
  <a class="card" href="/admin"><div class="ct">⚙ Administrador</div><div class="cd">Editar configurações pontuais (Node ID, servidor, redes, pinos) sem refazer tudo.</div></a>
  <div class="foot"><a href="/info">→ status do dispositivo (/info)</a></div>
</main>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
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
.tbtn{margin-top:14px}
.ts{margin-top:9px;font-size:11px;line-height:1.5;min-height:14px;color:var(--mu)}
.ts.ok{color:var(--ac)}.ts.err{color:var(--red)}.ts.run{color:var(--lb)}
.advx{margin-top:12px}.advx a{color:var(--mu);font-size:11px;text-decoration:none;cursor:pointer}.advx a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">assistente de configuração</div></header>
<main>
  <div class="steps">
    <div class="pill cur" id="pill0"><b>1</b><span>Rede 1</span></div>
    <div class="pill" id="pill1"><b>2</b><span>Rede 2</span></div>
    <div class="pill" id="pill2"><b>3</b><span>Servidor</span></div>
    <div class="pill" id="pill3"><b>4</b><span>Pinos</span></div>
  </div>
  <div class="track"><div class="fill" id="bar"></div></div>

  <div class="card-wrap">
    <!-- Passo 1: Rede 1 (obrigatória, testada) -->
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

    <!-- Passo 2: Rede 2 (opcional, com Pular) -->
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

    <!-- Passo 3: Node ID + servidor (escondido) + teste WS -->
    <div class="step" id="step2">
      <div class="sec">Identificação</div>
      <label>Node ID</label>
      <input id="nodeid" placeholder="ex: C00045">
      <div class="advx"><a id="srvtoggle">⚙ Alterar servidor (avançado)</a></div>
      <div id="srvbox" style="display:none">
        <label>Host</label>
        <input id="host" placeholder="frst-back-....herokuapp.com">
        <label>Porta</label>
        <input id="port" type="number" min="1" max="65535" placeholder="80">
      </div>
      <button class="btn tbtn" id="t2">Testar WebSocket</button>
      <div class="ts" id="ts2">Testa a conexão com o servidor.</div>
    </div>

    <!-- Passo 4: Pinos -->
    <div class="step" id="step3">
      <div class="sec">Pinos — Industrial</div>
      <label>GPIO START IN (pulso, ativo HIGH)</label>
      <input id="startPin" type="number" min="0" max="48">
      <label>GPIO AVAIL OUT (leitura status)</label>
      <input id="availPin" type="number" min="0" max="48">
      <div class="chk"><input id="availEn" type="checkbox"><label for="availEn">Usar AVAIL como fail-safe (confirma o ciclo e repulsa se a máquina não ligar)</label></div>
      <div class="hint">Evite strapping pins: GPIO0, 3, 45, 46. AVAIL usa pull-up interno (sem resistor externo). <b>Modelos sem AVAIL: deixe o fail-safe desmarcado.</b></div>
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
let scanList=[], net1ok=false, wsdone=false;

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

// Teste de WiFi ao vivo: o ESP conecta de verdade; o celular pode cair do AP por
// alguns segundos (rádio único) — usamos polling tolerante a falhas de fetch.
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
  let host=qs('host').value.trim(), port=qs('port').value||80, nid=qs('nodeid').value.trim();
  if(!nid){setTs('ts2','err','Preencha o Node ID antes de testar.');return;}
  setTs('ts2','run','Testando servidor…');
  fetch('/test-ws?host='+encodeURIComponent(host)+'&port='+port+'&nodeid='+encodeURIComponent(nid)).then(r=>r.json()).then(j=>{
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
    '&host='+encodeURIComponent(qs('host').value.trim())+
    '&port='+encodeURIComponent(qs('port').value||80)+
    '&startPin='+(qs('startPin').value||5)+
    '&availPin='+(qs('availPin').value||6)+
    '&availEn='+(qs('availEn').checked?1:0);
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
    qs('host').value=d.host||''; qs('port').value=d.port||80;
    qs('startPin').value=d.startPin!=null?d.startPin:5;
    qs('availPin').value=d.availPin!=null?d.availPin:6;
    qs('availEn').checked=(d.availEn===1);
  }).catch(()=>{});
  scan();
  qs('t0').onclick=()=>{net1ok=false;testWifi(qs('manual_ssid').value.trim()||qs('ssid').value,qs('pass').value,'ts0',(ok)=>{net1ok=ok;});};
  qs('t1').onclick=()=>{testWifi(qs('ssid2').value.trim()||qs('ssid2_scan').value,qs('pass2').value,'ts1',()=>{});};
  qs('t2').onclick=()=>{wsdone=false;testWs();};
  qs('srvtoggle').onclick=()=>{let b=qs('srvbox');b.style.display=(b.style.display==='none')?'block':'none';};
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
.foot a{color:var(--mu);text-decoration:none}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">administrador</div></header>
<main>
  <div class="box">
    <div class="sec">Node ID</div>
    <input id="nodeid">
    <button class="btn" id="bNode">Salvar Node ID</button>
  </div>
  <div class="box">
    <div class="sec">Servidor (WebSocket)</div>
    <label>Host</label><input id="host">
    <label>Porta</label><input id="port" type="number" min="1" max="65535">
    <button class="btn" id="bSrv">Salvar servidor</button>
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
    <div class="sec">Pinos — Industrial</div>
    <label>GPIO START IN</label><input id="startPin" type="number" min="0" max="48">
    <label>GPIO AVAIL OUT</label><input id="availPin" type="number" min="0" max="48">
    <div class="chk"><input id="availEn" type="checkbox"><label for="availEn">Fail-safe AVAIL</label></div>
    <button class="btn" id="bPin">Salvar pinos</button>
  </div>
  <div class="box">
    <div class="sec">Avançado</div>
    <div class="chk"><input id="wsrestart" type="checkbox"><label for="wsrestart">Auto-restart se 1h sem WebSocket</label></div>
    <button class="btn" id="bAdv">Salvar avançado</button>
    <div class="row"><button class="btn ghost" id="bRst">Reiniciar</button><button class="btn danger" id="bClr">Apagar tudo</button></div>
  </div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/">← início</a> · <a href="/wizard">assistente</a> · <a href="/info">/info</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
function val(i){return qs(i).value.trim();}
function msg(t){qs('msg').textContent=t;}
function post(b){msg('Salvando…');fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.text()).then(t=>msg(t)).catch(()=>msg('Falha.'));}
function save(o){post(Object.keys(o).map(k=>k+'='+encodeURIComponent(o[k])).join('&'));}
function scan(){fetch('/scan').then(r=>r.json()).then(l=>{['ssid','ssid2'].forEach(id=>{let s=qs(id);s.innerHTML='<option value="">— escolher —</option>';l.forEach(i=>{let o=document.createElement('option');o.value=i.ssid;o.textContent=i.ssid+' · '+i.rssi+'dBm';s.appendChild(o);});});}).catch(()=>{});}
window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('nodeid').value=d.nodeid||''; qs('host').value=d.host||''; qs('port').value=d.port||80;
    qs('startPin').value=d.startPin!=null?d.startPin:5; qs('availPin').value=d.availPin!=null?d.availPin:6;
    qs('availEn').checked=(d.availEn===1); qs('wsrestart').checked=(d.wsrestart===1);
  }).catch(()=>{});
  scan();
  qs('bNode').onclick=()=>{if(!val('nodeid')){msg('Preencha o Node ID');return;}save({nodeid:val('nodeid')});};
  qs('bSrv').onclick=()=>{if(!val('host')){msg('Preencha o host');return;}save({host:val('host'),port:val('port')||80});};
  qs('bN1').onclick=()=>{let s=val('m1')||val('ssid');if(!s){msg('Escolha a rede 1');return;}save({ssid:s,pass:qs('p1').value});};
  qs('bN2').onclick=()=>{save({ssid2:(val('m2')||val('ssid2')),pass2:qs('p2').value});};
  qs('bPin').onclick=()=>{save({startPin:val('startPin')||5,availPin:val('availPin')||6,availEn:(qs('availEn').checked?1:0)});};
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
  String   host = server.hasArg("host") ? server.arg("host") : wsHost;
  uint16_t port = server.hasArg("port") ? (uint16_t)server.arg("port").toInt() : wsPort;
  String   nid  = server.hasArg("nodeid") ? server.arg("nodeid") : nodeId;
  String building, machine; bool found = false;
  bool ok = testWsSync(host, port, nid, building, machine, found);
  // escapa para JSON (nomes podem ter aspas/contrabarra)
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
  json += "\"host\":\""      + wsHost + "\",";
  json += "\"port\":"        + String(wsPort) + ",";
  json += "\"startPin\":"    + String(startPin) + ",";
  json += "\"availPin\":"    + String(availPin) + ",";
  json += "\"availEn\":"     + String(availEnabled ? 1 : 0) + ",";
  json += "\"wsrestart\":"   + String(wsRestartEnabled ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

// ========== /save (parcial: grava só os campos enviados) ==========
// Usado pelo wizard (envia tudo) e pelo painel admin (envia subconjuntos).
void handleSave() {
  bool any = false;
  if (server.hasArg("ssid"))     { sSsid  = server.arg("ssid");  prefs.putString("ssid",   sSsid);  any = true; }
  if (server.hasArg("pass"))     { sPass  = server.arg("pass");  prefs.putString("pass",   sPass);  any = true; }
  if (server.hasArg("ssid2"))    { sSsid2 = server.arg("ssid2"); prefs.putString("ssid2",  sSsid2); any = true; }
  if (server.hasArg("pass2"))    { sPass2 = server.arg("pass2"); prefs.putString("pass2",  sPass2); any = true; }
  if (server.hasArg("nodeid"))   { nodeId = server.arg("nodeid"); prefs.putString("nodeid", nodeId); any = true; }
  if (server.hasArg("host"))     { wsHost = server.arg("host");  prefs.putString("wsHost", wsHost); any = true; }
  if (server.hasArg("port"))     { wsPort = (uint16_t)constrain(server.arg("port").toInt(), 1, 65535); prefs.putInt("wsPort", wsPort); any = true; }
  if (server.hasArg("startPin")) { startPin = constrain(server.arg("startPin").toInt(), 0, 48); prefs.putInt("startPin", startPin); any = true; }
  if (server.hasArg("availPin")) { availPin = constrain(server.arg("availPin").toInt(), 0, 48); prefs.putInt("availPin", availPin); any = true; }
  if (server.hasArg("availEn"))  { availEnabled = server.arg("availEn").toInt() == 1; prefs.putInt("availEn", availEnabled ? 1 : 0); any = true; }
  if (server.hasArg("wsrestart")){ wsRestartEnabled = server.arg("wsrestart").toInt() == 1; prefs.putInt("wsrestart", wsRestartEnabled ? 1 : 0); any = true; }

  if (server.hasArg("ssid") || server.hasArg("ssid2")) wifiSlot = 0;  // volta p/ a rede 1 ao trocar WiFi
  prefs.putUInt("bootCount", bootCount);

  server.send(200, "text/plain", any ? "Configurado. Reiniciando..." : "Nada para salvar.");
  if (any) { delay(400); ESP.restart(); }
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
  json += "\"relayOn\":"      + String(pulseActive ? "true" : "false") + ",";
  json += "\"machineMode\":1,";
  json += "\"pulseActive\":"  + String(pulseActive ? "true" : "false") + ",";
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
<header><div class="logo">FOREASY</div><div class="sub">status — atualiza a cada 2s</div></header>
<main>
  <div class="big">
    <div class="l">AVAIL OUT — status da máquina</div>
    <div class="v" id="avail">…</div>
    <div class="s" id="availRaw">…</div>
  </div>
  <div class="grid">
    <div class="it"><div class="lb">Node ID</div><div class="vl" id="node">…</div></div>
    <div class="it"><div class="lb">WebSocket</div><div class="vl" id="ws">…</div></div>
    <div class="it"><div class="lb">Servidor</div><div class="vl" id="srv">…</div></div>
    <div class="it"><div class="lb">Wi-Fi (STA)</div><div class="vl" id="wifi">…</div></div>
    <div class="it"><div class="lb">RSSI</div><div class="vl" id="rssi">…</div></div>
    <div class="it"><div class="lb">IP (STA)</div><div class="vl" id="ipsta">…</div></div>
    <div class="it"><div class="lb">IP (AP)</div><div class="vl" id="ipap">…</div></div>
    <div class="it"><div class="lb">Pulso</div><div class="vl" id="relay">…</div></div>
    <div class="it"><div class="lb">Pinos (S/A)</div><div class="vl" id="pins">…</div></div>
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
function g(id){return document.getElementById(id);}
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    g('avail').textContent = j.availLivre?'LIVRE':'OCUPADA';
    g('avail').style.color = j.availLivre?'#00e676':'#f87171';
    let raw = j.availRaw===0?'LOW':(j.availRaw===1?'HIGH':'—');
    let fs = j.availEn ? (['idle','pulsando','confirmando','retry'][j.creditState]||'?') : 'desligado';
    g('availRaw').textContent = raw+' · há '+(j.availSinceMs/1000).toFixed(1)+'s · fail-safe: '+fs;
    g('node').textContent=j.nodeId||'—';
    g('ws').textContent=j.wsConnected?'Conectado':'Desconectado';
    g('srv').textContent=(j.host||'—')+':'+j.port;
    g('wifi').textContent=j.ssid||'—';
    g('rssi').textContent=(j.rssi||0)+' dBm';
    g('ipsta').textContent=j.ip_sta||'—';
    g('ipap').textContent=j.ip_ap||'—';
    g('relay').textContent=j.pulseActive?'ATIVO':'inativo';
    g('pins').textContent='S'+j.startPin+' · A'+j.availPin;
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

// ========== /resetwifi ==========
void handleResetWiFi() {
  prefs.clear();   // apaga todo o namespace "wifi" da NVS
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
