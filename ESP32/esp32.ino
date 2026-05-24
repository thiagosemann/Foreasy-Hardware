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
// - 0x03 => Responde JSON com rssi, heap, uptime, boots, wifiSlot, temp
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
      bool effectiveOn = isRelayEffectiveOn();
      String msg = (effectiveOn ? "ID:" : "NID:") + nodeId;
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
          char buf[200];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"heap\":%u,\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u,\"temp\":%.1f}",
            staOk ? WiFi.RSSI() : 0,
            (unsigned)ESP.getFreeHeap(),
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
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222;text-align:center}
.header{background:#198754;color:white;padding:22px;font-size:24px;font-weight:bold}
.container{margin-top:28px}
.btn{display:block;margin:12px auto;padding:14px 18px;width:85%;max-width:360px;background:#198754;color:white;font-size:18px;border-radius:12px;text-decoration:none;box-shadow:0 4px 12px rgba(0,0,0,0.12)}
.btn:hover{background:#146b45}
.small{color:#666;margin-top:18px}
</style></head><body>
<div class="header">Foreasy - Dispositivo</div>
<div class="container">
  <a class="btn" href="/config">Configurar Wi-Fi</a>
  <a class="btn" href="/nodeid">Configurar NodeID</a>
  <a class="btn" href="/info">Informações</a>
  <a class="btn" href="/relay">Controle do Relé</a>
  <a class="btn" href="/wifistatus">Status Wi-Fi</a>
  <a class="btn" href="/wsstatus">Status WebSocket</a>
  <a class="btn" href="/resetwifi">Reset credenciais</a>
  <a class="btn" href="/restart">Reiniciar</a>
</div>
<div class="small">Foreasy Smart Devices</div>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ========== /config ==========
void handleConfigPage() {
  String html = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy - Configuração</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222}
.header{background:#198754;color:#fff;padding:20px;text-align:center;font-weight:700}
.card{background:#fff;max-width:520px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.12)}
input,select,button{width:100%;padding:12px;margin-top:10px;border-radius:8px;border:1px solid #cfe9d8;box-sizing:border-box}
button{background:#198754;color:#fff;border:none;font-weight:700;cursor:pointer}
small{color:#444;display:block;margin-top:10px;line-height:1.35}
a{color:#198754}
.section{font-weight:700;color:#198754;margin-top:18px;margin-bottom:4px;border-bottom:1px solid #cfe9d8;padding-bottom:4px}
.toggleRow{display:flex;gap:10px;margin-top:10px}
.toggleBtn{flex:1;border:1px solid #cfe9d8;border-radius:10px;padding:10px;cursor:pointer;background:#f7fff7;text-align:center;font-size:14px}
.toggleBtn.active{outline:2px solid #198754;border-color:#198754;font-weight:700}
.chkRow{display:flex;align-items:center;gap:10px;margin-top:12px}
.chkRow input{width:auto;margin:0}
</style>
</head><body>
<div class="header">Foreasy - Configuração</div>
<div class="card">
  <div class="section">Rede 1 (primária)</div>
  <label>Selecione rede (scan)</label>
  <select id="ssid"></select>
  <label>Ou digite SSID manual</label>
  <input id="manual_ssid" placeholder="SSID manual">
  <label>Senha</label>
  <input id="pass" type="text" placeholder="Senha (vazio se rede aberta)">

  <div class="section">Rede 2 (failover)</div>
  <small>Opcional. O dispositivo alterna para esta rede quando a rede 1 falhar.</small>
  <label>SSID</label>
  <input id="ssid2" placeholder="SSID da rede 2 (opcional)">
  <label>Senha</label>
  <input id="pass2" type="text" placeholder="Senha rede 2 (opcional)">

  <div class="section">Dispositivo</div>
  <label>NodeID</label>
  <input id="nodeid" placeholder="ex: C00045">

  <label>Modo do relé</label>
  <div class="toggleRow">
    <div id="modeNormal" class="toggleBtn active">Normal<br><small>segue WS</small></div>
    <div id="modeOn"     class="toggleBtn">Sempre ON</div>
    <div id="modeOff"    class="toggleBtn">Sempre OFF</div>
  </div>

  <label style="margin-top:14px">Tipo do relé</label>
  <div class="toggleRow">
    <div id="typeNA" class="toggleBtn active">NA<br><small>ON = HIGH</small></div>
    <div id="typeNF" class="toggleBtn">NF<br><small>ON = LOW</small></div>
  </div>

  <div class="chkRow">
    <input id="wsrestart" type="checkbox">
    <label for="wsrestart">Reiniciar automaticamente se sem WebSocket por 1 hora</label>
  </div>

  <button id="save" style="margin-top:18px">Salvar e Reiniciar</button>
  <small id="status"></small>
  <small style="margin-top:14px"><a href="/info">Informações do dispositivo</a></small>
</div>
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
    list.forEach(i=>{
      let o=document.createElement('option');
      o.value=i.ssid;
      o.textContent=i.ssid+' | '+i.rssi+' dBm | '+encText(i.enc);
      s.appendChild(o);
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
           '&ssid2='+encodeURIComponent(qs('ssid2').value.trim())+
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
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy - Info</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222}
.header{background:#198754;color:white;padding:18px;text-align:center;font-weight:700}
.card{background:#fff;max-width:760px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 20px rgba(0,0,0,0.12)}
.row{display:flex;gap:12px;flex-wrap:wrap}
.item{flex:1;min-width:180px;padding:12px;border-radius:10px;background:#f7fff7;border:1px solid #e6f4ea}
h4{margin:6px 0;color:#198754}
a{color:#198754}
pre{max-height:200px;overflow:auto;font-size:12px;background:#111827;color:#e5e7eb;padding:10px;border-radius:8px;margin-top:12px}
</style></head><body>
<div class="header">Foreasy - Informações (auto refresh 3s)</div>
<div class="card">
  <div class="row">
    <div class="item"><h4>NodeID</h4><span id="nodeId">...</span></div>
    <div class="item"><h4>Wi-Fi (STA)</h4><span id="ssid">...</span></div>
    <div class="item"><h4>RSSI</h4><span id="rssi">...</span></div>
    <div class="item"><h4>Temp (°C)</h4><span id="temp">...</span></div>
  </div>
  <div style="height:10px"></div>
  <div class="row">
    <div class="item"><h4>IP (STA)</h4><span id="ip_sta">...</span></div>
    <div class="item"><h4>IP (AP)</h4><span id="ip_ap">...</span></div>
    <div class="item"><h4>WebSocket</h4><span id="ws">...</span></div>
    <div class="item"><h4>Relé</h4><span id="relay">...</span></div>
  </div>
  <div style="height:10px"></div>
  <div class="row">
    <div class="item"><h4>WiFi Slot</h4><span id="wifiSlot">...</span></div>
    <div class="item"><h4>Boots</h4><span id="boots">...</span></div>
    <div class="item"><h4>Relay Mode</h4><span id="relayMode">...</span></div>
    <div class="item"><h4>Relay Type</h4><span id="relayType">...</span></div>
  </div>
  <div style="margin-top:14px"><a href="/config">← Config</a> | <a href="/">Menu</a></div>
  <pre id="raw"></pre>
</div>
<script>
const modeLabel=['Normal','Sempre ON','Sempre OFF'];
const typeLabel=['NA (ON=HIGH)','NF (ON=LOW)'];
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    nodeId.textContent   = j.nodeId||'—';
    ssid.textContent     = j.ssid||'—';
    rssi.textContent     = (j.rssi||0)+' dBm';
    temp.textContent     = j.temp+'°C';
    ip_sta.textContent   = j.ip_sta||'—';
    ip_ap.textContent    = j.ip_ap||'—';
    ws.textContent       = j.wsConnected ? 'Conectado' : 'Desconectado';
    relay.textContent    = j.relayOn ? 'ON' : 'OFF';
    wifiSlot.textContent = 'Slot '+(j.wifiSlot+1);
    boots.textContent    = j.boots;
    relayMode.textContent= modeLabel[j.relayMode]||j.relayMode;
    relayType.textContent= typeLabel[j.relayType]||j.relayType;
    raw.textContent      = JSON.stringify(j,null,2);
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
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Controle do Relé</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222}
.header{background:#198754;color:#fff;padding:18px;text-align:center;font-weight:700}
.card{background:#fff;max-width:480px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.12)}
button{padding:10px 12px;margin:6px;border-radius:10px;border:none;background:#198754;color:#fff;font-weight:700;cursor:pointer}
label{display:block;margin-top:12px;font-weight:600}
select{width:100%;padding:10px;border-radius:8px;border:1px solid #cfe9d8;margin-top:6px}
.chkRow{display:flex;align-items:center;gap:8px;margin-top:8px}
.chkRow input{width:auto}
.small{font-size:13px;color:#555;margin-top:8px}
</style></head><body>
<div class="header">Controle do Relé</div>
<div class="card">
  <p><b>Estado atual:</b> <span id="relayState">...</span></p>

  <label>MODO DO RELÉ</label>
  <select id="relayMode">
    <option value="0">Normal (segue WebSocket)</option>
    <option value="1">Sempre ON</option>
    <option value="2">Sempre OFF</option>
  </select>

  <label style="margin-top:14px">TIPO DO RELÉ</label>
  <div class="chkRow"><input type="checkbox" id="relayNA"><span>Normalmente Aberto (NA) — ON=HIGH</span></div>
  <div class="chkRow"><input type="checkbox" id="relayNF"><span>Normalmente Fechado (NF) — ON=LOW</span></div>

  <button id="saveCfg" style="width:100%;margin-top:14px">Salvar Configuração do Relé</button>
  <div class="small" id="cfgStatus"></div>

  <hr style="margin:18px 0">
  <div style="text-align:center">
    <button id="btnOn"  style="width:46%">Ligar</button>
    <button id="btnOff" style="width:46%">Desligar</button>
  </div>
  <p class="small">Em modo <b>Sempre ON</b> ou <b>Sempre OFF</b> os comandos do WebSocket e os botões acima são ignorados.</p>

  <div style="margin-top:12px;text-align:center"><a href="/">Voltar</a></div>
</div>
<script>
function qs(id){return document.getElementById(id);}
function loadStatus(){
  fetch('/status').then(r=>r.json()).then(j=>{
    qs('relayState').textContent = j.relayOn ? 'ON' : 'OFF';
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
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy - NodeID</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222}
.header{background:#198754;color:#fff;padding:20px;text-align:center;font-weight:700}
.card{background:#fff;max-width:420px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.12)}
input,button{width:100%;padding:12px;margin-top:10px;border-radius:8px;border:1px solid #cfe9d8;box-sizing:border-box}
button{background:#198754;color:#fff;border:none;font-weight:700;cursor:pointer}
small{color:#555;font-size:13px;display:block;margin-top:10px}
</style></head><body>
<div class="header">Configuração de NodeID</div>
<div class="card">
  <label>NodeID atual</label>
  <input id="nodeid" placeholder="ex: C00045">
  <button id="save">Salvar NodeID e Reiniciar</button>
  <small id="status"></small>
  <small style="margin-top:14px">AP: SSID = <b>NodeID-AP</b> | Senha = <b>12345678</b> (ativo 10 min após boot)</small>
  <p style="margin-top:14px"><a href="/">Voltar</a></p>
</div>
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
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{background:#eef6f0;font-family:Arial;margin:0}.header{background:#198754;color:#fff;padding:18px;text-align:center;font-weight:700}.card{background:#fff;max-width:560px;margin:18px auto;padding:18px;border-radius:12px}</style>"
    "<body><div class='header'>Status WiFi</div><div class='card'>"
    "<p><b>SSID:</b> "     + (staOk ? WiFi.SSID() : String("—")) + "</p>"
    "<p><b>RSSI:</b> "     + (staOk ? String(WiFi.RSSI()) + " dBm" : String("—")) + "</p>"
    "<p><b>IP (STA):</b> " + (staOk ? WiFi.localIP().toString() : String("—")) + "</p>"
    "<p><b>IP (AP):</b> "  + WiFi.softAPIP().toString() + "</p>"
    "<p><b>Slot ativo:</b> Rede " + String(wifiSlot + 1) + "</p>"
    "<p><a href='/'>Voltar</a></p></div></body></html>";
  server.send(200, "text/html", html);
}

// ========== /wsstatus ==========
void handleWSStatusPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{background:#eef6f0;font-family:Arial;margin:0}.header{background:#198754;color:#fff;padding:18px;text-align:center;font-weight:700}.card{background:#fff;max-width:560px;margin:18px auto;padding:18px;border-radius:12px}</style>"
    "<body><div class='header'>Status WebSocket</div><div class='card'>"
    "<p><b>Conectado:</b> "          + String(isWebSocketConnected ? "SIM" : "NÃO") + "</p>"
    "<p><b>Servidor:</b> "           + String(wsHost) + "</p>"
    "<p><b>Backoff atual:</b> "      + String(wsNextRetryMs) + " ms</p>"
    "<p><b>wsRestartEnabled:</b> "   + String(wsRestartEnabled ? "SIM" : "NÃO") + "</p>"
    "<p><a href='/'>Voltar</a></p></div></body></html>";
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
