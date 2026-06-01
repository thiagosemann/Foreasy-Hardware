// ============================================================================
// Foreasy ESP32 — Modelos 3 e 4
// Hardware: ESP32 + shield relé 30A | Modelo 4 usa SSR acionado pelo relé 30A
//
// SISTEMA: Convencional — controle direto de energia (relay ON/OFF)
//   Não há lógica de pulso ou integração com conector Speed Queen H3/H5.
//   O backend liga ou desliga o relay; o relay libera ou corta a energia da máquina.
//
// CONFIGURAÇÃO DO RELAY (persistente via Preferences/NVS):
// - relayMode : 0 = Normal (obedece WS) | 1 = Sempre ON | 2 = Sempre OFF
// - relayType : 0 = NA — Normalmente Aberto (ON=HIGH) | 1 = NF — Normalmente Fechado (ON=LOW)
//   Em modo Sempre ON/OFF os comandos do WS e botões locais são ignorados.
//
// PROTOCOLO WEBSOCKET (binário):
// - 0x01 => Relay ON  (ignorado se relayMode != 0)
// - 0x02 => Relay OFF (ignorado se relayMode != 0)
// - 0x03 => Responde JSON com rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp
//
// WIFI:
// - Dual WiFi com failover automático entre rede 1 e rede 2 (sem restart)
// - Conexão não-bloqueante: wifiTick() com timeout 40s e retry a cada 5s
// - Credenciais NUNCA apagadas por falha de conexão
//
// WEBSOCKET:
// - Backoff exponencial: 10s base → 120s máximo
// - Watchdog WS down  : sem WS por >5min  → failover WiFi+WS
// - Watchdog global   : sem WiFi+WS >8min → failover
// - Detecção de zumbi : sem ping/pong por >5min → reconecta
// - App ping a cada 30s | heartbeat: 15s/3s/2 tentativas
// - wsRestartEnabled  : reinicia ESP32 após 1h sem WS (opcional, configurável)
//
// AP: ativo 10 min após boot (lean mode após expirar)
//     SSID: <nodeId>-AP | Senha: 12345678
//
// ARMAZENAMENTO: Preferences (NVS) — ssid, pass, ssid2, pass2, nodeid,
//                relayMode, relayType, wsrestart, bootCount
// bootCount incrementado em RAM; salvo apenas no /save (evita desgaste da flash)
//
// SCAN WIFI: assíncrono (não bloqueia o loop)
// TEMPERATURA: sensor interno do ESP32 via temprature_sens_read()
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>

// ======== Internal temp (ESP32) ========
extern "C" { uint8_t temprature_sens_read(); }
float readInternalTempC() {
  uint8_t raw = temprature_sens_read();
  return (raw - 32.0f) * 5.0f / 9.0f;
}

WebServer server(80);
WebSocketsClient webSocket;
Preferences prefs;

// ---------- IO ----------
const int ledPin   = 4;
const int relayPin = 2;

// ---------- Relay ----------
int  relayMode      = 0;   // 0=Normal, 1=Sempre ON, 2=Sempre OFF
int  relayType      = 0;   // 0=NA, 1=NF
bool relayLogicalOn = false;
int  relayOnLevel   = HIGH;
int  relayOffLevel  = LOW;

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

// ================= RELAY HELPERS =================
void updateRelayLevels() {
  if (relayType == 0) { relayOnLevel = HIGH; relayOffLevel = LOW; }
  else                { relayOnLevel = LOW;  relayOffLevel = HIGH; }
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
  nodeId           = prefs.getString("nodeid",    "FOREASY");
  sSsid            = prefs.getString("ssid",      "");
  sPass            = prefs.getString("pass",      "");
  sSsid2           = prefs.getString("ssid2",     "");
  sPass2           = prefs.getString("pass2",     "");
  relayMode        = prefs.getInt("relayMode",    0);
  relayType        = prefs.getInt("relayType",    0);
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
  webSocket.begin(wsHost, wsPort, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);
  lastPingMs = millis();
  lastAppPingMs = 0;
  lastWSConnectAttemptMs = millis();
  Serial.printf("WS begin: %s:%u\n", wsHost, wsPort);
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
      String msg = "ID:" + nodeId;
      webSocket.sendTXT(msg);
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
          char buf[260];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%u,\"block\":%u,\"cpu\":%u,\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u,\"temp\":%.1f}",
            staOk ? WiFi.RSSI() : 0,
            staOk ? (int)WiFi.channel() : 0,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getCpuFreqMHz(),
            (unsigned long)(millis() / 1000UL),
            (unsigned long)bootCount,
            (unsigned)wifiSlot,
            readInternalTempC()
          );
          webSocket.sendTXT(buf);
          break;
        }

        if (relayMode == 0) {
          if      (b == 0x01) relayLogicalOn = true;
          else if (b == 0x02) relayLogicalOn = false;
          applyRelayOutput();
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

  bootCount++;  // incrementa em RAM; salvo apenas no /save

  updateRelayLevels();

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
  pinMode(relayPin, OUTPUT);
  relayLogicalOn = false;
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
  Serial.println("HTTP server iniciado.");

  // Inicia scan assíncrono para /config page
  WiFi.scanNetworks(true);

  if (hasSavedWiFi()) connectToWiFi_begin();
  else Serial.println("Sem WiFi salvo. Configure pelo AP.");
}

// ================= LOOP =================
void loop() {
  server.handleClient();
  wifiTick();
  wsTick();
  watchdogTick();
  apLifetimeTick();
  wsRestartTick();

  digitalWrite(ledPin, isWebSocketConnected ? HIGH : LOW);
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
  if (isRelayEffectiveOn())  { wsLastOkMs = millis(); return; }  // não reinicia com relé ativo
  if ((millis() - wsLastOkMs) > WS_RESTART_TIMEOUT_MS) {
    Serial.println("WS_RESTART: sem WS por 1h. Reiniciando.");
    delay(200);
    ESP.restart();
  }
}

// ================= HTTP ROUTES =================
void startWebServer() {
  server.on("/",              handleRoot);
  server.on("/config",        handleConfigPage);
  server.on("/config-data",   HTTP_GET,  handleConfigData);
  server.on("/info",          handleInfoPage);
  server.on("/scan",          HTTP_GET,  handleScan);
  server.on("/save",          HTTP_POST, handleSave);
  server.on("/status",        HTTP_GET,  handleStatusJson);
  server.on("/relay",         HTTP_GET,  handleRelayPage);
  server.on("/relay/on",      HTTP_GET,  handleRelayOn);
  server.on("/relay/off",     HTTP_GET,  handleRelayOff);
  server.on("/relay/config",  HTTP_POST, handleRelayConfigSave);
  server.on("/nodeid",        HTTP_GET,  handleNodeIdPage);
  server.on("/savenodeid",    HTTP_POST, handleSaveNodeId);
  server.on("/wifistatus",    HTTP_GET,  handleWiFiStatusPage);
  server.on("/wsstatus",      HTTP_GET,  handleWSStatusPage);
  server.on("/resetwifi",     HTTP_GET,  handleResetWiFi);
  server.on("/restart",       HTTP_GET,  handleRestart);
  server.onNotFound(handleNotFound);
}

// ========== / ==========
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;min-height:100vh}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:22px;text-align:center}
.logo{color:var(--ac);font-size:24px;font-weight:700;letter-spacing:4px}
.sub{color:var(--mu);font-size:11px;letter-spacing:2px;margin-top:5px}
main{max-width:420px;margin:0 auto;padding:28px 16px}
.menu{display:grid;gap:8px}
.lnk{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;background:var(--cd);border:1px solid var(--bd);border-radius:3px;color:var(--tx);text-decoration:none;font-size:13px;transition:border-color .15s}
.lnk:hover{border-color:var(--ac);color:var(--ac)}
.lnk:hover .arr{color:var(--ac)}
.lnk .desc{font-size:10px;color:var(--mu);margin-top:2px;transition:color .15s}
.lnk:hover .desc{color:var(--ac2)}
.arr{color:var(--mu);font-size:16px;transition:color .15s}
footer{text-align:center;margin-top:28px;color:var(--mu);font-size:10px;letter-spacing:1px;padding-bottom:20px}
</style></head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">smart device · esp32</div>
</header>
<main>
  <div class="menu">
    <a class="lnk" href="/config"><div><div>Configurar Wi-Fi</div><div class="desc">redes e credenciais</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/nodeid"><div><div>Node ID</div><div class="desc">identificação do dispositivo</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/info"><div><div>Informações</div><div class="desc">status em tempo real</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/relay"><div><div>Controle do Relé</div><div class="desc">modo e tipo</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/wifistatus"><div><div>Status Wi-Fi</div><div class="desc">conexão e sinal</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/wsstatus"><div><div>Status WebSocket</div><div class="desc">servidor e backoff</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/resetwifi"><div><div>Reset Credenciais</div><div class="desc">apaga Wi-Fi e reinicia</div></div><span class="arr">→</span></a>
    <a class="lnk" href="/restart"><div><div>Reiniciar</div><div class="desc">reinicia o ESP32</div></div><span class="arr">→</span></a>
  </div>
</main>
<footer>foreasy smart devices</footer>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /config ==========
void handleConfigPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Config</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0d1710}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}
.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:540px;margin:0 auto;padding:18px 16px 32px}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:22px 0 10px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:12px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:3px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none;transition:border-color .15s}
input:focus,select:focus{border-color:var(--ac)}
select option{background:var(--cd)}
.trow{display:flex;gap:8px;margin-top:8px}
.tgl{flex:1;padding:11px 8px;border:1px solid var(--bd);border-radius:3px;cursor:pointer;text-align:center;background:var(--cd);color:var(--mu);font-size:12px;transition:all .15s;line-height:1.4}
.tgl.active{background:#003d1a;border-color:var(--ac);color:var(--ac);font-weight:700}
.tgl small{display:block;font-size:10px;opacity:.7;margin-top:2px}
.chk{display:flex;align-items:center;gap:10px;margin-top:10px;padding:10px 12px;border:1px solid var(--bd);border-radius:3px;background:var(--cd)}
.chk input[type=checkbox]{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}
.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0;cursor:pointer}
.btn{display:block;width:100%;margin-top:22px;padding:13px;background:var(--ac);color:#000;border:none;border-radius:3px;font-family:inherit;font-size:13px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase}
.btn:hover{background:var(--ac2);color:var(--tx)}
.st{margin-top:10px;font-size:12px;min-height:16px;color:var(--ac)}
.lnk{margin-top:20px;font-size:11px}
.lnk a{color:var(--mu);text-decoration:none}
.lnk a:hover{color:var(--ac)}
</style>
</head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">configuração do dispositivo</div>
</header>
<main>
  <div class="sec">Rede 1 — primária</div>
  <label>Scan de redes</label>
  <select id="ssid"></select>
  <label>SSID manual</label>
  <input id="manual_ssid" placeholder="ou digita aqui">
  <label>Senha</label>
  <input id="pass" type="text" placeholder="vazio se rede aberta">

  <div class="sec">Rede 2 — failover</div>
  <small style="color:var(--mu);font-size:11px;display:block;margin-top:6px">Opcional. Alterna automaticamente se a rede 1 falhar.</small>
  <label>Scan de redes</label>
  <select id="ssid2_scan"></select>
  <label>SSID manual</label>
  <input id="ssid2" placeholder="opcional">
  <label>Senha</label>
  <input id="pass2" type="text" placeholder="opcional">

  <div class="sec">Dispositivo</div>
  <label>Node ID</label>
  <input id="nodeid" placeholder="ex: C00045">

  <label>Modo do relé</label>
  <div class="trow">
    <div id="modeNormal" class="tgl active">Normal<small>segue WS</small></div>
    <div id="modeOn"     class="tgl">Sempre ON</div>
    <div id="modeOff"    class="tgl">Sempre OFF</div>
  </div>

  <label style="margin-top:14px">Tipo do relé</label>
  <div class="trow">
    <div id="typeNA" class="tgl active">NA<small>ON = HIGH</small></div>
    <div id="typeNF" class="tgl">NF<small>ON = LOW</small></div>
  </div>

  <div class="chk">
    <input id="wsrestart" type="checkbox">
    <label for="wsrestart">Auto-restart se sem WebSocket por 1 hora</label>
  </div>

  <button class="btn" id="save">Salvar e Reiniciar</button>
  <div class="st" id="status"></div>
  <div class="lnk"><a href="/info">→ /info</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
let relayModeVal=0, relayTypeVal=0;

function setRelayMode(v){
  relayModeVal=v;
  ['modeNormal','modeOn','modeOff'].forEach((id,i)=>qs(id).classList.toggle('active',i===v));
}
function setRelayType(v){
  relayTypeVal=v;
  ['typeNA','typeNF'].forEach((id,i)=>qs(id).classList.toggle('active',i===v));
}

function encText(e){return['Open','WEP','WPA-PSK','WPA2-PSK','WPA/WPA2'][e]||'?';}

function scan(retry){
  retry=retry||0;
  fetch('/scan').then(r=>r.json()).then(list=>{
    if(list.length===0&&retry<6){setTimeout(()=>scan(retry+1),2500);return;}
    let s=qs('ssid'); s.innerHTML='';
    let s2=qs('ssid2_scan'); s2.innerHTML='<option value="">— nenhuma —</option>';
    list.forEach(i=>{
      let o=document.createElement('option');
      o.value=i.ssid;
      o.textContent=i.ssid+' | '+i.rssi+' dBm | '+encText(i.enc);
      s.appendChild(o);
      s2.appendChild(o.cloneNode(true));
    });
  }).catch(()=>{if(retry<6)setTimeout(()=>scan(retry+1),2500);});
}

window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('manual_ssid').value=d.ssid||'';
    qs('pass').value=d.pass||'';
    qs('ssid2').value=d.ssid2||'';
    qs('pass2').value=d.pass2||'';
    qs('nodeid').value=d.nodeid||'';
    setRelayMode(d.relayMode||0);
    setRelayType(d.relayType||0);
    qs('wsrestart').checked=(d.wsrestart===1);
  }).catch(()=>{});

  scan();

  qs('modeNormal').onclick=()=>setRelayMode(0);
  qs('modeOn').onclick    =()=>setRelayMode(1);
  qs('modeOff').onclick   =()=>setRelayMode(2);
  qs('typeNA').onclick    =()=>setRelayType(0);
  qs('typeNF').onclick    =()=>setRelayType(1);

  qs('save').onclick=()=>{
    let ss=qs('manual_ssid').value.trim()||qs('ssid').value;
    let id=qs('nodeid').value.trim();
    if(!ss){qs('status').textContent='Preencha o SSID da rede 1!';return;}
    if(!id){qs('status').textContent='Preencha o NodeID!';return;}
    qs('status').textContent='Salvando...';
    fetch('/save',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ss)+
           '&pass='+encodeURIComponent(qs('pass').value)+
           '&ssid2='+encodeURIComponent(qs('ssid2').value.trim()||qs('ssid2_scan').value)+
           '&pass2='+encodeURIComponent(qs('pass2').value)+
           '&nodeid='+encodeURIComponent(id)+
           '&relayMode='+relayModeVal+
           '&relayType='+relayTypeVal+
           '&wsrestart='+(qs('wsrestart').checked?1:0)
    }).then(r=>r.text()).then(t=>{qs('status').textContent=t;});
  };
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /config-data ==========
void handleConfigData() {
  String json = "{";
  json += "\"ssid\":"     + (sSsid.length()  ? "\""+sSsid+"\""  : "\"\"") + ",";
  json += "\"pass\":"     + (sPass.length()  ? "\""+sPass+"\""  : "\"\"") + ",";
  json += "\"ssid2\":"    + (sSsid2.length() ? "\""+sSsid2+"\"" : "\"\"") + ",";
  json += "\"pass2\":"    + (sPass2.length() ? "\""+sPass2+"\"" : "\"\"") + ",";
  json += "\"nodeid\":\"" + nodeId + "\",";
  json += "\"relayMode\":" + String(relayMode) + ",";
  json += "\"relayType\":" + String(relayType) + ",";
  json += "\"wsrestart\":" + String(wsRestartEnabled ? 1 : 0);
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

  relayMode        = server.hasArg("relayMode") ? constrain(server.arg("relayMode").toInt(), 0, 2) : 0;
  relayType        = server.hasArg("relayType") ? constrain(server.arg("relayType").toInt(), 0, 1) : 0;
  wsRestartEnabled = server.hasArg("wsrestart") && server.arg("wsrestart").toInt() == 1;
  wifiSlot = 0;

  prefs.putString("ssid",      sSsid);
  prefs.putString("pass",      sPass);
  prefs.putString("ssid2",     sSsid2);
  prefs.putString("pass2",     sPass2);
  prefs.putString("nodeid",    nodeId);
  prefs.putInt("relayMode",    relayMode);
  prefs.putInt("relayType",    relayType);
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
  json += "\"nodeId\":\""    + nodeId + "\",";
  json += "\"ssid\":\""      + (staOk ? WiFi.SSID() : String("")) + "\",";
  json += "\"rssi\":"        + String(staOk ? WiFi.RSSI() : 0) + ",";
  json += "\"ip_sta\":\""    + (staOk ? WiFi.localIP().toString() : String("")) + "\",";
  json += "\"ip_ap\":\""     + WiFi.softAPIP().toString() + "\",";
  json += "\"wsConnected\":" + String(isWebSocketConnected ? "true" : "false") + ",";
  json += "\"temp\":"        + String(readInternalTempC(), 1) + ",";
  json += "\"relayOn\":"     + String(isRelayEffectiveOn() ? "true" : "false") + ",";
  json += "\"relayMode\":"   + String(relayMode) + ",";
  json += "\"relayType\":"   + String(relayType) + ",";
  json += "\"wifiSlot\":"    + String(wifiSlot) + ",";
  json += "\"boots\":"       + String(bootCount);
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
    <div class="it"><div class="lb">WiFi Slot</div><div class="vl" id="wifiSlot">…</div></div>
    <div class="it"><div class="lb">Boots</div><div class="vl" id="boots">…</div></div>
    <div class="it"><div class="lb">Relay Mode</div><div class="vl" id="relayMode">…</div></div>
    <div class="it"><div class="lb">Relay Type</div><div class="vl" id="relayType">…</div></div>
  </div>
  <div class="nav">
    <a href="/config">← /config</a>
    <a href="/">← menu</a>
  </div>
  <pre id="raw"></pre>
</main>
<script>
const modeLabel=['Normal','Sempre ON','Sempre OFF'];
const typeLabel=['NA (ON=HIGH)','NF (ON=LOW)'];
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    document.getElementById('nodeId').textContent    = j.nodeId||'—';
    document.getElementById('ssid').textContent      = j.ssid||'—';
    document.getElementById('rssi').textContent      = (j.rssi||0)+' dBm';
    document.getElementById('temp').textContent      = j.temp+'°C';
    document.getElementById('ip_sta').textContent    = j.ip_sta||'—';
    document.getElementById('ip_ap').textContent     = j.ip_ap||'—';
    document.getElementById('ws').textContent        = j.wsConnected ? 'Conectado' : 'Desconectado';
    document.getElementById('relay').textContent     = j.relayOn ? 'ON' : 'OFF';
    document.getElementById('wifiSlot').textContent  = 'Slot '+(j.wifiSlot+1);
    document.getElementById('boots').textContent     = j.boots;
    document.getElementById('relayMode').textContent = modeLabel[j.relayMode]||j.relayMode;
    document.getElementById('relayType').textContent = typeLabel[j.relayType]||j.relayType;
    document.getElementById('raw').textContent       = JSON.stringify(j,null,2);
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
            ",\"enc\":"     + (int)WiFi.encryptionType(i) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  // Inicia novo scan em background para próxima chamada
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
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0d1710;--red:#f87171;--red2:#b91c1c}
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
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:12px 0 4px}
select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:3px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none}
select option{background:var(--cd)}
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

  <div class="sec">Configuração</div>
  <label>Modo do relé</label>
  <select id="relayMode">
    <option value="0">Normal — segue WebSocket</option>
    <option value="1">Sempre ON</option>
    <option value="2">Sempre OFF</option>
  </select>

  <label style="margin-top:14px">Tipo do relé</label>
  <div class="chk"><input type="checkbox" id="relayNA"><span>Normalmente Aberto (NA) — ON=HIGH</span></div>
  <div class="chk"><input type="checkbox" id="relayNF"><span>Normalmente Fechado (NF) — ON=LOW</span></div>

  <button class="btn btn-save" id="saveCfg">Salvar Configuração</button>
  <div class="st" id="cfgStatus"></div>

  <div class="sec">Controle manual</div>
  <div class="btn-row">
    <button class="btn btn-on"  id="btnOn">Ligar</button>
    <button class="btn btn-off" id="btnOff">Desligar</button>
  </div>
  <div class="note">Em modo <b>Sempre ON</b> ou <b>Sempre OFF</b> os comandos do WebSocket e os botões acima são ignorados.</div>

  <div class="nav"><a href="/">← menu</a></div>
</main>
<script>
function qs(id){return document.getElementById(id);}
function loadStatus(){
  fetch('/status').then(r=>r.json()).then(j=>{
    qs('relayState').textContent = j.relayOn ? 'ON' : 'OFF';
    qs('relayState').style.color = j.relayOn ? '#00e676' : '#f87171';
    qs('relayMode').value = j.relayMode!=null ? j.relayMode : 0;
    let t = j.relayType!=null ? j.relayType : 0;
    qs('relayNA').checked=(t===0); qs('relayNF').checked=(t===1);
  });
}
window.onload=()=>{
  loadStatus();
  qs('relayNA').onchange=()=>{if(qs('relayNA').checked)qs('relayNF').checked=false;};
  qs('relayNF').onchange=()=>{if(qs('relayNF').checked)qs('relayNA').checked=false;};
  qs('saveCfg').onclick=()=>{
    let mode=qs('relayMode').value, type=qs('relayNF').checked?1:0;
    qs('cfgStatus').textContent='Salvando...';
    fetch('/relay/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'mode='+mode+'&type='+type})
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
  if (server.hasArg("type")) {
    relayType = constrain(server.arg("type").toInt(), 0, 1);
    prefs.putInt("relayType", relayType);
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
  prefs.remove("ssid");      prefs.remove("pass");
  prefs.remove("ssid2");     prefs.remove("pass2");
  prefs.remove("nodeid");    prefs.remove("relayMode");
  prefs.remove("relayType"); prefs.remove("wsrestart");
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
