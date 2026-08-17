// ============================================================================
// Foreasy ESP8266 — Modelo 6 — Convencional
// Hardware: ESP8266 ESP-01S + shield relay GPIO V1 (relé 10A no GPIO0).
//   O relé 10A do shield aciona um MÓDULO DE RELÉ 30A externo, que por sua vez
//   controla a alimentação da máquina. O firmware enxerga apenas o GPIO0.
//
// SISTEMA: Convencional — controle de energia pelo RELÉ (liga/desliga).
//   WS 0x01 => RELAY ON  (fica ligado até receber OFF)
//   WS 0x02 => RELAY OFF
//
// RELAY INVERT (relayInvert) — útil para relay NF (Normalmente Fechado):
//   0 => normal    (ON = HIGH | OFF = LOW)
//   1 => invertido (ON = LOW  | OFF = HIGH)
//
// RELAY MODE (persistente):
//   relayMode 0 = Normal (obedece WS)
//   relayMode 1 = Sempre ON   (ignora WS)
//   relayMode 2 = Sempre OFF  (ignora WS)
//
// ATENÇÃO — GPIO0 é strapping pin do ESP8266:
//   Durante o boot o GPIO0 precisa estar HIGH; o shield tem pull-up, e o
//   firmware força LOW (relé desligado na lógica NA) logo no início do setup.
//   Os watchdogs NUNCA reiniciam sozinhos — só fazem failover — para não
//   arriscar um boot em modo flash com a carga ligada.
//   Com relayInvert=1 (NF) o nível de repouso passa a ser HIGH: só use invert
//   se o módulo de 30A estiver ligado no contato NC do relé do shield.
//
// PROTOCOLO WEBSOCKET (binário):
//   0x01 => Relay ON  (se relayMode=0)
//   0x02 => Relay OFF (se relayMode=0)
//   0x03 => JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot,
//                 machineMode(0), pulse(false), chip, fw  — sem `temp`
//                 (o ESP8266 não tem sensor de temperatura interno)
//   0x06 => Restart remoto. Responde "Restarting" e reinicia após ~200ms.
//   (0x04 OTA não existe no ESP-01S: 1MB de flash, sem espaço p/ imagem dupla)
//
// WIFI: Dual WiFi com failover automático (sem restart)
//   Conexão não-bloqueante, credenciais nunca apagadas por falha
//
// WEBSOCKET:
//   Backoff exponencial: 10s base → 120s máx
//   Watchdog WS down  : sem WS >5min  → failover
//   Watchdog global   : sem WiFi+WS >8min → failover
//   Zumbi             : sem ping/pong >5min → reconecta
//   wsRestartEnabled  : reinicia após 30min sem WS (configurável; nunca reinicia
//                       com o relé ligado)
//
// AP: ativo 10 min após boot | SSID: <nodeId>-AP | Senha: 12345678
//     Depois disso: AP desliga, logs apagados e o HTTP para (lean mode).
//
// EEPROM v1 (magic 0xF0EA5E0C — layout próprio do convencional):
//   ssid[32], pass[64], ssid2[32], pass2[64], nodeId[24],
//   relayMode, relayInvert, bootCount, lastResetReason, wsRestartEnabled,
//   wsHost[64], wsPort (servidor alterável apenas pelo /admin).
//   Apaga setor antes de salvar + até 3 tentativas de commit com verificação.
//   bootCount incrementado em RAM; salvo apenas no /save.
//   Magic diferente do firmware industrial: gravar este .ino por cima de uma
//   placa industrial cai nos defaults (é preciso reconfigurar pelo AP).
//
// DICA: se EEPROM.commit() falhar → selecionar Flash Mode = DOUT na IDE Arduino
// ============================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsClient.h>
#include <EEPROM.h>
#include <stdarg.h>

// ======================== CONFIG DEBUG =========================
static const bool DEBUG_FLASH_INFO        = true;
static const bool DEBUG_EEPROM_DUMP       = true;
static const bool DEBUG_EEPROM_SECTORINFO = true;

// ATENÇÃO: /save apaga o setor antes de gravar. Útil para diagnóstico.
static const bool ERASE_SECTOR_BEFORE_SAVE = true;

// Quantas tentativas de commit
static const uint8_t COMMIT_TRIES = 3;

// ======================== WS =========================
// Defaults de fábrica; o host/porta efetivos ficam em P.wsHost/P.wsPort (alteráveis só no /admin)
static const char*    WS_HOST = "frst-back-02b607761078.herokuapp.com";
static const uint16_t WS_PORT = 80;

// Identidade do firmware (reportada no WS 0x03 — auditoria da frota)
#define FW_VERSION "1.4.0"
#define FW_CHIP    "esp8266"   // ESP-01S (Modelo 6 — convencional)

// ======================== IO (ESP-01/ESP-01S / Generic ESP8266) =========================
static const int  relayPin = 0;  // GPIO0 — relé 10A do shield (aciona o módulo 30A)
static const int  ledPin   = 2;  // GPIO2 (LED onboard, ativo LOW)
static const bool LED_ACTIVE_LOW = true;

// ======================== AP =========================
static IPAddress apIP(192, 168, 4, 1);
static const uint32_t AP_LIFETIME_MS = 10UL * 60UL * 1000UL; // 10 min
static bool     apEnabled  = true;
static uint32_t bootTimeMs = 0;

// ======================== WiFi STA =========================
static bool     wifiConnecting      = false;
static uint32_t wifiConnectStartMs  = 0;
static const uint32_t WIFI_MAX_WAIT_MS = 40000;
static uint32_t lastWiFiAttemptMs   = 0;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;

// ======================== WebSocket =========================
WebSocketsClient webSocket;
static bool isWebSocketConnected = false;

// ======================== WiFi Failover =========================
static uint8_t wifiSlot = 0; // 0=rede1, 1=rede2

static uint32_t lastWSConnectAttemptMs = 0;
static uint8_t  wsRetryStreak = 0;
static const uint32_t WS_RETRY_BASE_MS = 10000;
static const uint32_t WS_RETRY_MAX_MS  = 120000;
static uint32_t wsNextRetryMs = WS_RETRY_BASE_MS;

// ping/pong
static uint32_t lastPingMs = 0;
static const uint32_t PING_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// app ping
static uint32_t lastAppPingMs = 0;
static const uint32_t APP_PING_INTERVAL_MS = 30UL * 1000UL;

// watchdogs (modo lean)
static uint32_t wsDownSinceMs = 0;
static const uint32_t WS_DOWN_RESET_MS = 5UL * 60UL * 1000UL;

static uint32_t lastConnectivityOkMs = 0;
static const uint32_t GLOBAL_DOWN_RESET_MS = 8UL * 60UL * 1000UL;

// LED only-on-change
static bool lastLedState = false;

// WS auto-restart
static uint32_t wsLastOkMs = 0;
static const uint32_t WS_RESTART_TIMEOUT_MS = 30UL * 60UL * 1000UL; // 30 minutos

// Restart remoto (WS 0x06) — adiado p/ a resposta "Restarting" sair antes do reboot
static bool     pendingRestart   = false;
static uint32_t pendingRestartMs = 0;

// ======================== RELAY (liga/desliga) =========================
static bool relayInvert    = false;
static bool relayLogicalOn = false;   // estado pedido pelo WS / painel (só RAM)
static int  relayOnLevel   = HIGH;
static int  relayOffLevel  = LOW;

// ======================== Wizard: teste ao vivo de WiFi/WS ========================
enum TestState : uint8_t { TST_IDLE, TST_RUN, TST_OK, TST_FAIL };
static TestState wifiTestState   = TST_IDLE;
static bool      wifiTestActive  = false;
static uint32_t  wifiTestStartMs = 0;
static const uint32_t WIFI_TEST_TIMEOUT_MS = 12000;
static String    wifiTestSsid, wifiTestPass;
static String    lastGoodSsid, lastGoodPass;

// ======================== HTTP =========================
ESP8266WebServer server(80);

// ======================== LOG =========================
static const size_t LOG_MAX_LEN = 2500;
static char logBuffer[LOG_MAX_LEN + 1];

// Buffer compartilhado pelas páginas montadas em runtime (/wifistatus, /wsstatus).
// Uma só cópia na RAM: nunca são servidas ao mesmo tempo.
static char htmlBuf[2400];

// ======================== EEPROM =========================
#define EEPROM_SIZE 512
static const uint32_t EEPROM_MAGIC = 0xF0EA5E0C; // 0C = convencional
static const uint16_t EEPROM_VER   = 1;

struct __attribute__((packed)) Persist {
  uint32_t magic;
  uint16_t ver;

  char ssid[32];
  char pass[64];
  char ssid2[32];         // rede 2 (failover), opcional
  char pass2[64];         // senha rede 2
  char nodeId[24];

  uint8_t  relayMode;     // 0=Normal, 1=Sempre ON, 2=Sempre OFF
  uint8_t  relayInvert;   // 0 normal (NA), 1 invertido (NF)

  uint32_t bootCount;     // incrementa em RAM no boot (commit só no /save)
  uint8_t  lastResetReason;

  uint8_t  wsRestartEnabled; // 0=off, 1=reinicia após 30min sem WS
  uint8_t  reserved[6];

  char     wsHost[64];       // servidor WS (alterável só no /admin)
  uint16_t wsPort;
};

Persist P;

// ======================== Forward declarations =========================
static void connectToWiFi_begin();
static void connectToWebSocket();
static void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length);
static bool isRelayEffectiveOn();
static void applyRelayOutput();

// ======================== Helpers (LED/Relay) =========================
static inline void setLed(bool on) {
  if (LED_ACTIVE_LOW) digitalWrite(ledPin, on ? LOW : HIGH);
  else                digitalWrite(ledPin, on ? HIGH : LOW);
}

static void updateRelayLevels() {
  if (!relayInvert) { relayOnLevel = HIGH; relayOffLevel = LOW; }
  else              { relayOnLevel = LOW;  relayOffLevel = HIGH; }
}

// relayMode fixo ignora o WS e os botões do painel.
static bool isRelayEffectiveOn() {
  if (P.relayMode == 1) return true;
  if (P.relayMode == 2) return false;
  return relayLogicalOn;
}

static void applyRelayOutput() {
  digitalWrite(relayPin, isRelayEffectiveOn() ? relayOnLevel : relayOffLevel);
}

// ======================== Helpers (Log) =========================
static void log_append_line(const char* line) {
  if (!apEnabled) {
    Serial.print("[CRIT] ");
    Serial.println(line);
    return;
  }

  char tmp[300];
  unsigned long secs = millis() / 1000UL;
  int n = snprintf(tmp, sizeof(tmp), "%lus - %s\n", secs, line);
  if (n <= 0) return;

  size_t cur = strnlen(logBuffer, LOG_MAX_LEN);
  size_t add = (size_t)n;
  if (add > LOG_MAX_LEN) return;

  if (cur + add > LOG_MAX_LEN) {
    size_t excess = (cur + add) - LOG_MAX_LEN;
    memmove(logBuffer, logBuffer + excess, cur - excess);
    cur -= excess;
    logBuffer[cur] = '\0';
  }

  memcpy(logBuffer + cur, tmp, add);
  logBuffer[cur + add] = '\0';

  Serial.print(tmp);
}

static void logf(const char* fmt, ...) {
  char msg[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  log_append_line(msg);
}

static size_t json_escape_into(const char* in, char* out, size_t outSz) {
  if (outSz == 0) return 0;
  size_t w = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    char c = in[i];
    const char* rep = nullptr;

    if (c == '\"') rep = "\\\"";
    else if (c == '\\') rep = "\\\\";
    else if (c == '\n') rep = "\\n";
    else if (c == '\r') rep = "\\r";
    else if (c == '\t') rep = "\\t";

    if (rep) {
      for (size_t k = 0; rep[k] != '\0'; k++) {
        if (w + 1 >= outSz) { out[w] = '\0'; return w; }
        out[w++] = rep[k];
      }
    } else {
      if (w + 1 >= outSz) { out[w] = '\0'; return w; }
      out[w++] = c;
    }
  }
  out[w] = '\0';
  return w;
}

static size_t html_escape_into(const char* in, char* out, size_t outSz) {
  if (outSz == 0) return 0;
  size_t w = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    char c = in[i];
    const char* rep = nullptr;
    if (c == '&') rep = "&amp;";
    else if (c == '<') rep = "&lt;";
    else if (c == '>') rep = "&gt;";
    else if (c == '"') rep = "&quot;";
    else if (c == '\'') rep = "&#39;";

    if (rep) {
      for (size_t k = 0; rep[k] != '\0'; k++) {
        if (w + 1 >= outSz) { out[w] = '\0'; return w; }
        out[w++] = rep[k];
      }
    } else {
      if (w + 1 >= outSz) { out[w] = '\0'; return w; }
      out[w++] = c;
    }
  }
  out[w] = '\0';
  return w;
}

// ======================== Reset reason =========================
static uint8_t getResetReasonByte() {
  const rst_info* info = ESP.getResetInfoPtr();
  if (!info) return 0;
  return (uint8_t)info->reason;
}

static const char* resetReasonToStr(uint8_t r) {
  switch (r) {
    case REASON_DEFAULT_RST:      return "POWERON/DEFAULT";
    case REASON_WDT_RST:          return "WDT";
    case REASON_EXCEPTION_RST:    return "EXCEPTION";
    case REASON_SOFT_WDT_RST:     return "SOFT_WDT";
    case REASON_SOFT_RESTART:     return "SOFTWARE";
    case REASON_DEEP_SLEEP_AWAKE: return "DEEPSLEEP";
    case REASON_EXT_SYS_RST:      return "EXT_SYS_RST";
    default:                      return "UNKNOWN";
  }
}

// O enum de criptografia do ESP8266 não bate com o do ESP32: traduz aqui,
// para o wizard não precisar conhecer os números da plataforma.
static const char* encToStr(uint8_t e) {
  switch (e) {
    case ENC_TYPE_NONE: return "Aberta";
    case ENC_TYPE_WEP:  return "WEP";
    case ENC_TYPE_TKIP: return "WPA";
    case ENC_TYPE_CCMP: return "WPA2";
    case ENC_TYPE_AUTO: return "WPA/WPA2";
    default:            return "?";
  }
}

// ======================== FLASH/EEPROM DEBUG =========================
static uint32_t eepromSectorIndexFromFlashSize(uint32_t flashSizeBytes) {
  return (flashSizeBytes / 4096UL) - 1UL; // último setor
}

static void debugFlashInfoOnce() {
  if (!DEBUG_FLASH_INFO) return;

  uint32_t realSz = ESP.getFlashChipRealSize();
  uint32_t ideSz  = ESP.getFlashChipSize();
  uint32_t spd    = ESP.getFlashChipSpeed();
  uint8_t  mode   = ESP.getFlashChipMode();

  Serial.println();
  Serial.println("========== FLASH INFO ==========");
  Serial.printf("[FLASH] Real=%u bytes (%.2f MB)\n", realSz, realSz / 1048576.0);
  Serial.printf("[FLASH] IDE =%u bytes (%.2f MB)\n", ideSz,  ideSz  / 1048576.0);
  Serial.printf("[FLASH] Mode=%u (0=QIO,1=QOUT,2=DIO,3=DOUT)\n", mode);
  Serial.printf("[FLASH] Speed=%u Hz\n", spd);
  Serial.println("================================");
  Serial.println();

  if (realSz != ideSz) {
    Serial.println("[FLASH][WARN] Real != IDE. Isso é causa MUITO comum de EEPROM.commit() falhar.");
  }
}

static void debugEepromSectorInfo() {
  if (!DEBUG_EEPROM_SECTORINFO) return;

  uint32_t ideSz  = ESP.getFlashChipSize();
  uint32_t realSz = ESP.getFlashChipRealSize();

  uint32_t sectorIde  = eepromSectorIndexFromFlashSize(ideSz);
  uint32_t sectorReal = eepromSectorIndexFromFlashSize(realSz);

  Serial.println("========== EEPROM SECTOR INFO ==========");
  Serial.printf("[EEPROM] EEPROM_SIZE=%u bytes | Persist=%u bytes\n",
                (unsigned)EEPROM_SIZE, (unsigned)sizeof(Persist));
  Serial.printf("[EEPROM] Sector(IDE )=%lu\n", (unsigned long)sectorIde);
  Serial.printf("[EEPROM] Sector(REAL)=%lu\n", (unsigned long)sectorReal);
  Serial.println("========================================");
}

static void debugEepromDump(uint16_t from = 0, uint16_t len = 64) {
  if (!DEBUG_EEPROM_DUMP) return;

  Serial.printf("[EEPROM] DUMP %u..%u:\n", from, (unsigned)(from + len - 1));
  for (uint16_t i = 0; i < len; i++) {
    if ((i % 16) == 0) Serial.printf("0x%04X: ", (unsigned)(from + i));
    Serial.printf("%02X ", EEPROM.read(from + i));
    if ((i % 16) == 15) Serial.println();
  }
  Serial.println();
}

static bool diagEraseEepromSector() {
  uint32_t ideSz  = ESP.getFlashChipSize();
  uint32_t realSz = ESP.getFlashChipRealSize();
  uint32_t sector = eepromSectorIndexFromFlashSize(ideSz);

  Serial.println("[DIAG] Attempting flashEraseSector(last sector used for EEPROM)...");
  Serial.printf("[DIAG] Flash Real=%u IDE=%u | sector=%lu\n",
                realSz, ideSz, (unsigned long)sector);

  bool ok = ESP.flashEraseSector(sector);
  Serial.printf("[DIAG] flashEraseSector result=%s\n", ok ? "true" : "false");
  return ok;
}

// ======================== EEPROM persist =========================
static void persistDefaults() {
  memset(&P, 0, sizeof(P));
  P.magic = EEPROM_MAGIC;
  P.ver   = EEPROM_VER;

  strncpy(P.nodeId, "ForeasyInativa01", sizeof(P.nodeId) - 1);

  P.relayMode   = 0;  // Normal (segue o WS)
  P.relayInvert = 0;

  strncpy(P.wsHost, WS_HOST, sizeof(P.wsHost) - 1);
  P.wsPort = WS_PORT;
}

static void applyPersistRuntime() {
  if (P.relayMode > 2) P.relayMode = 0;
  relayInvert = (P.relayInvert != 0);
  updateRelayLevels();
  applyRelayOutput();

  // fail-safe: host vazio ou porta 0 na EEPROM → volta ao default de fábrica
  if (P.wsHost[0] == '\0') strncpy(P.wsHost, WS_HOST, sizeof(P.wsHost) - 1);
  if (P.wsPort == 0)       P.wsPort = WS_PORT;
}

static void persistLoad() {
  EEPROM.get(0, P);

  Serial.printf("[EEPROM] boot read: magic=0x%08lX ver=%u ssid='%s' node='%s' mode=%u invert=%u\n",
                (unsigned long)P.magic, (unsigned)P.ver, P.ssid, P.nodeId,
                (unsigned)P.relayMode, (unsigned)P.relayInvert);

  if (DEBUG_EEPROM_DUMP) debugEepromDump(0, 64);

  if (P.magic != EEPROM_MAGIC || P.ver != EEPROM_VER) {
    Serial.println("[EEPROM] mismatch -> defaults");
    persistDefaults();
    EEPROM.put(0, P);
    bool ok = EEPROM.commit();
    Serial.printf("[EEPROM] defaults commit=%s\n", ok ? "true" : "false");
  }

  applyPersistRuntime();
}

static bool persistSaveAndVerifyOnce(uint8_t tryN) {
  EEPROM.put(0, P);
  bool ok = EEPROM.commit();

  Persist T;
  EEPROM.get(0, T);

  Serial.printf("[EEPROM] try=%u save: commit=%s magic=0x%08lX ver=%u ssid='%s' node='%s' mode=%u invert=%u\n",
                (unsigned)tryN,
                ok ? "true" : "false",
                (unsigned long)T.magic, (unsigned)T.ver, T.ssid, T.nodeId,
                (unsigned)T.relayMode, (unsigned)T.relayInvert);

  if (DEBUG_EEPROM_DUMP) debugEepromDump(0, 64);

  return ok && (T.magic == EEPROM_MAGIC) && (T.ver == EEPROM_VER);
}

static bool persistSaveAndVerifyRetry(uint8_t tries) {
  for (uint8_t t = 1; t <= tries; t++) {
    if (persistSaveAndVerifyOnce(t)) return true;
    delay(150);
    yield();
  }
  return false;
}

static const char* activeSSID() { return (wifiSlot == 0) ? P.ssid : P.ssid2; }
static const char* activePass() { return (wifiSlot == 0) ? P.pass : P.pass2; }
static bool hasSavedWiFi() { return (activeSSID()[0] != '\0'); }

// ======================== WS backoff =========================
static uint32_t computeWsBackoffMs() {
  uint32_t v = WS_RETRY_BASE_MS;
  uint8_t s = wsRetryStreak;
  while (s > 0 && v < WS_RETRY_MAX_MS) { v <<= 1; s--; }
  if (v > WS_RETRY_MAX_MS) v = WS_RETRY_MAX_MS;
  if (v < WS_RETRY_BASE_MS) v = WS_RETRY_BASE_MS;
  return v;
}
static void resetWsBackoff() { wsRetryStreak = 0; wsNextRetryMs = WS_RETRY_BASE_MS; }
static void bumpWsBackoff()  { if (wsRetryStreak < 10) wsRetryStreak++; wsNextRetryMs = computeWsBackoffMs(); }

// ======================== Reconexão completa WiFi+WS (sem restart) =========================
static void fullReconnectWiFiWS() {
  Serial.println("[RECOVERY] Reconexão completa WiFi+WS (sem restart).");
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

static void switchWiFiSlot() {
  wifiSlot = (wifiSlot == 0) ? 1 : 0;
  Serial.printf("[FAILOVER] Alternando para rede %u: SSID=%s\n", wifiSlot + 1, activeSSID());
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

static void failoverReconnect() {
  if (P.ssid2[0] != '\0') {
    switchWiFiSlot();
  } else {
    fullReconnectWiFiWS();
  }
}

// ======================== AP / WiFi / WS =========================
// Sufixo do chip ID de fábrica (2 bytes, 4 dígitos hex) — diferencia o AP de
// cada peça quando várias sobem juntas em bancada com o nodeId ainda no
// padrão (mesmo nodeId = SSID igual em todas, dá erro pra conectar na peça
// certa). ESP.getChipId() já é derivado do MAC, único por placa e estável
// entre reinícios — não precisa de RNG (o ESP8266 nem tem gerador de
// hardware confiável antes do rádio subir).
static String apSuffix() {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", (unsigned)(ESP.getChipId() & 0xFFFF));
  return String(buf);
}

static void setupAPSTA() {
  char apName[40];
  snprintf(apName, sizeof(apName), "%s-%s-AP", P.nodeId[0] ? P.nodeId : "FOREASY", apSuffix().c_str());
  const char* apPass = "12345678";
  bool hidden = false;

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  logf("Iniciando AP: %s (hidden=%s)", apName, hidden ? "true" : "false");
  WiFi.softAP(apName, apPass, 1, hidden);
  delay(120);

  IPAddress ap = WiFi.softAPIP();
  logf("AP IP: %u.%u.%u.%u", ap[0], ap[1], ap[2], ap[3]);

  WiFi.mode(WIFI_AP_STA);
}

static void connectToWiFi_begin() {
  if (!hasSavedWiFi()) {
    log_append_line("Nenhuma credencial Wi-Fi salva.");
    return;
  }
  // Só volta para AP_STA se o AP ainda estiver de direito ligado (apEnabled).
  // Usar AP_STA incondicionalmente aqui religava o rádio do AP em toda
  // reconexão pós-expiração (softAP mantém a config antiga na memória do
  // driver), inclusive depois do apLifetimeTick já ter desligado.
  WiFi.mode(apEnabled ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(activeSSID(), activePass());
  wifiConnecting = true;
  wifiConnectStartMs = millis();
  logf("WiFi begin: SSID=%s (slot %u)", activeSSID(), wifiSlot + 1);
}

static void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      isWebSocketConnected = true;
      lastPingMs = millis();
      wsLastOkMs = millis();
      wsDownSinceMs = 0;
      resetWsBackoff();

      // "ID:<nodeId>" se o relé está ligado, "NID:<nodeId>" se desligado.
      char hello[52];
      snprintf(hello, sizeof(hello), "%s%s",
               isRelayEffectiveOn() ? "ID:" : "NID:",
               (P.nodeId[0] ? P.nodeId : "FOREASY"));
      webSocket.sendTXT(hello);

      if (apEnabled) {
        logf("WS CONNECTED. Ident: %s", hello);
        logf("Modo=CONVENCIONAL | RelayMode=%u | RelayInvert=%s | Levels ON=%s OFF=%s",
             (unsigned)P.relayMode,
             relayInvert ? "true" : "false",
             (relayOnLevel == HIGH) ? "HIGH" : "LOW",
             (relayOffLevel == HIGH) ? "HIGH" : "LOW");
      }
      break;
    }

    case WStype_DISCONNECTED:
      isWebSocketConnected = false;
      bumpWsBackoff();
      if (apEnabled) logf("WS DISCONNECTED. Backoff=%lums (streak=%u)",
                          (unsigned long)wsNextRetryMs, (unsigned)wsRetryStreak);
      break;

    case WStype_PING:
      lastPingMs = millis();
      webSocket.sendTXT("Pong");
      if (apEnabled) log_append_line("WS PING recebido. Enviando 'Pong'.");
      break;

    case WStype_PONG:
      lastPingMs = millis();
      if (apEnabled) log_append_line("WS PONG recebido.");
      break;

    case WStype_BIN:
      if (length > 0) {
        uint8_t b = payload[0];

        if (b == 0x03) {
          // Telemetria. Sem "temp": o ESP8266 não tem sensor interno.
          bool     staOk = (WiFi.status() == WL_CONNECTED);
          int      rssi  = staOk ? WiFi.RSSI() : 0;
          int      ch    = staOk ? (int)WiFi.channel() : 0;
          uint32_t heap  = ESP.getFreeHeap();
          uint32_t block = ESP.getMaxFreeBlockSize();
          uint8_t  cpu   = ESP.getCpuFreqMHz();
          uint32_t up    = millis() / 1000UL;

          char buf[320];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%lu,\"block\":%lu,\"cpu\":%u,\"uptime\":%lu,\"boots\":%lu,"
            "\"wifiSlot\":%u,\"machineMode\":0,\"pulse\":false,\"chip\":\"%s\",\"fw\":\"%s\"}",
            rssi, ch,
            (unsigned long)heap,
            (unsigned long)block,
            (unsigned)cpu,
            (unsigned long)up,
            (unsigned long)P.bootCount,
            (unsigned)wifiSlot,
            FW_CHIP,
            FW_VERSION
          );
          webSocket.sendTXT(buf);
          if (apEnabled) logf("WS BIN 0x03: %s", buf);
          break;
        }

        // 0x06 => restart remoto (adiado p/ a resposta sair antes do reboot)
        if (b == 0x06) {
          webSocket.sendTXT("Restarting");
          if (apEnabled) log_append_line("WS BIN 0x06: restart remoto agendado.");
          pendingRestart   = true;
          pendingRestartMs = millis() + 200;
          break;
        }

        // 0x01 liga / 0x02 desliga — só quando relayMode = Normal
        if (P.relayMode == 0) {
          if      (b == 0x01) { relayLogicalOn = true;  applyRelayOutput(); }
          else if (b == 0x02) { relayLogicalOn = false; applyRelayOutput(); }
        }
        webSocket.sendTXT(isRelayEffectiveOn() ? "RelayStatus:ON" : "RelayStatus:OFF");
        if (apEnabled) logf("WS BIN 0x%02X: rele=%s (mode=%u)",
                            (unsigned)b,
                            isRelayEffectiveOn() ? "ON" : "OFF",
                            (unsigned)P.relayMode);
      }
      break;

    default:
      break;
  }
}

static void connectToWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    if (apEnabled) log_append_line("connectToWebSocket: WiFi não conectado. Abortando.");
    return;
  }

  if (apEnabled) logf("Iniciando WebSocket: %s:%u path:/", P.wsHost, (unsigned)P.wsPort);

  webSocket.disconnect();
  delay(20);

  webSocket.begin(P.wsHost, P.wsPort, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);

  lastPingMs = millis();
  lastAppPingMs = 0;
  lastWSConnectAttemptMs = millis();

  if (apEnabled) log_append_line("WS iniciado (aguardando CONNECTED).");
}

// ======================== TESTE AO VIVO (wizard) ========================
// Conecta o STA na rede candidata enquanto o AP segue de pé. Casa o canal do AP com
// o da rede alvo para reduzir a queda do celular (rádio único do ESP8266).
static void startWifiTest(const String& ssid, const String& pass, int ch) {
  wifiTestActive  = true;
  wifiTestState   = TST_RUN;
  wifiTestStartMs = millis();
  wifiTestSsid = ssid; wifiTestPass = pass;
  webSocket.disconnect();
  isWebSocketConnected = false;
  WiFi.disconnect(false);
  delay(40);
  if (ch >= 1 && ch <= 13) {
    char apName[40];
    snprintf(apName, sizeof(apName), "%s-%s-AP", P.nodeId[0] ? P.nodeId : "FOREASY", apSuffix().c_str());
    WiFi.softAP(apName, "12345678", ch, false);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
    delay(60);
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
}

static void wifiTestTick() {
  if (wifiTestState != TST_RUN) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiTestState = TST_OK;
    lastGoodSsid = wifiTestSsid; lastGoodPass = wifiTestPass;
  } else if ((millis() - wifiTestStartMs) > WIFI_TEST_TIMEOUT_MS) {
    wifiTestState = TST_FAIL;
    WiFi.disconnect(false);
  }
}

static bool wsReadN(WiFiClient& c, uint8_t* buf, size_t len, uint32_t deadline) {
  size_t got = 0;
  while (got < len && (int32_t)(deadline - millis()) > 0) {
    if (c.available()) buf[got++] = (uint8_t)c.read();
    else delay(2);
    yield();
  }
  return got == len;
}

static String jsonStrVal(const String& src, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int i = src.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  int e = src.indexOf('"', i);
  return (e < 0) ? "" : src.substring(i, e);
}

// Teste de WebSocket (síncrono): handshake → "WhoAmI:<nodeId>" → resposta com prédio/máquina.
// Usa o servidor salvo (P.wsHost/P.wsPort), sempre em texto claro — sem TLS, porque o
// ESP-01S não tem RAM sobrando para BearSSL. Retorna true se conectou (101);
// `found` indica se o nodeId existe no backend.
static bool testWsSync(const String& nodeId, String& building, String& machine, bool& found) {
  building = ""; machine = ""; found = false;
  if (WiFi.status() != WL_CONNECTED && lastGoodSsid.length()) {
    WiFi.begin(lastGoodSsid.c_str(), lastGoodPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 8000) { delay(50); yield(); }
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient c;
  c.setTimeout(5000);
  if (!c.connect(P.wsHost, P.wsPort)) return false;

  c.printf("GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", P.wsHost);
  String resp; uint32_t t0 = millis();
  while (c.connected() && (millis() - t0) < 5000 && resp.length() < 512) {
    while (c.available()) resp += (char)c.read();
    if (resp.indexOf("\r\n\r\n") >= 0) break;
    delay(5); yield();
  }
  if (resp.indexOf("101") < 0) { c.stop(); return false; }

  String payload = "WhoAmI:" + nodeId;
  size_t n = payload.length();
  if (n < 126) {
    uint8_t mk[4]  = { 0x21, 0x53, 0xAE, 0x42 };
    uint8_t hdr[2] = { 0x81, (uint8_t)(0x80 | n) };
    c.write(hdr, 2); c.write(mk, 4);
    for (size_t i = 0; i < n; i++) { uint8_t b = (uint8_t)payload[i] ^ mk[i & 3]; c.write(&b, 1); }
    c.flush();
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
        building = jsonStrVal(body, "building");
        machine  = jsonStrVal(body, "machine");
      }
    }
  }
  c.stop();
  return true;
}

// ======================== HTTP pages (PROGMEM, servidas com send_P) =========================
static const char LANDING_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--mu:#557060;--tx:#d4f5e0}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;min-height:100vh}
header{padding:34px 20px 8px;text-align:center}.logo{color:var(--ac);font-size:26px;font-weight:700;letter-spacing:6px}.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:4px;text-transform:uppercase}
main{max-width:460px;margin:0 auto;padding:18px;display:flex;flex-direction:column;gap:14px}
a.card{display:block;text-decoration:none;background:var(--cd);border:1px solid var(--bd);border-radius:10px;padding:22px 20px}
a.card:hover{border-color:var(--ac)}.ct{color:var(--ac);font-size:17px;font-weight:700;letter-spacing:1px}.cd{color:var(--mu);font-size:11px;line-height:1.6;margin-top:8px}
.foot{margin-top:6px;text-align:center;font-size:11px;line-height:2}.foot a{color:var(--mu);text-decoration:none;margin:0 6px}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">esp8266 convencional · configuração</div></header>
<main>
<a class="card" href="/wizard"><div class="ct">▶ Assistente (Wizard)</div><div class="cd">Configuração guiada passo a passo: testa o Wi-Fi e o servidor antes de salvar. Recomendado na primeira instalação.</div></a>
<a class="card" href="/admin"><div class="ct">⚙ Administrador</div><div class="cd">Editar configurações pontuais (Node ID, redes, relé) sem refazer tudo.</div></a>
<div class="foot"><a href="/info">status (/info)</a><a href="/relay">controle do relé</a><a href="/diagflash" target="_blank">/diagflash</a></div>
</main></body></html>
)rawliteral";

static const char WIZARD_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy — Configuração</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--cd2:#0b110d;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0b130e;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px;min-height:100vh}
header{padding:20px 20px 8px;text-align:center}.logo{color:var(--ac);font-size:22px;font-weight:700;letter-spacing:5px}.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:3px;text-transform:uppercase}
main{max-width:560px;margin:0 auto;padding:6px 16px 36px}
.steps{display:flex;gap:6px;margin:14px 0 6px}.pill{flex:1;display:flex;flex-direction:column;align-items:center;gap:4px;color:var(--mu);font-size:9px;letter-spacing:1px;text-transform:uppercase}
.pill b{display:flex;align-items:center;justify-content:center;width:26px;height:26px;border-radius:50%;border:1px solid var(--bd);font-size:12px;background:var(--cd)}
.pill.cur{color:var(--ac)}.pill.cur b{border-color:var(--ac);color:var(--ac)}.pill.done{color:var(--lb)}.pill.done b{border-color:var(--ac2);background:#04261447;color:var(--ac)}
.track{height:2px;background:var(--bd);border-radius:2px;overflow:hidden;margin-bottom:18px}.fill{height:100%;width:0;background:linear-gradient(90deg,var(--ac2),var(--ac));transition:width .35s}
.card-wrap{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px 16px;min-height:230px}.step{display:none}.step.on{display:block}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:16px 0 8px;padding-bottom:5px;border-bottom:1px solid var(--bd)}.sec:first-child{margin-top:0}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:11px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none}
input:focus,select:focus{border-color:var(--ac)}select option{background:var(--cd)}
.trow{display:flex;gap:8px;margin-top:8px}
.tgl{flex:1;padding:11px 8px;border:1px solid var(--bd);border-radius:6px;cursor:pointer;text-align:center;background:var(--cd2);color:var(--mu);font-size:12px;line-height:1.4}
.tgl.active{border-color:var(--ac);background:#04261433;color:var(--ac);font-weight:700}.tgl small{display:block;font-size:10px;opacity:.7;margin-top:2px}
.chk{display:flex;align-items:center;gap:10px;margin-top:14px;padding:10px 12px;border:1px solid var(--bd);border-radius:4px;background:var(--cd2)}.chk input{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0}
.hint{color:var(--mu);font-size:10px;line-height:1.5;margin-top:10px;padding:8px 10px;border-left:2px solid var(--bd)}
.nav{display:flex;gap:8px;margin-top:16px}.btn{flex:1;padding:13px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}.btn:hover{background:var(--ac2);color:var(--tx)}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu);flex:0 0 110px}.btn.ghost:hover{border-color:var(--ac);color:var(--ac)}.tbtn{margin-top:14px;flex:1}
.ts{margin-top:9px;font-size:11px;line-height:1.5;min-height:14px;color:var(--mu)}.ts.ok{color:var(--ac)}.ts.err{color:var(--red)}.ts.run{color:var(--lb)}
.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}.foot{margin-top:16px;text-align:center;font-size:11px}.foot a{color:var(--mu);text-decoration:none}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">assistente de configuração · convencional</div></header>
<main>
  <div class="steps"><div class="pill cur" id="pill0"><b>1</b><span>Rede 1</span></div><div class="pill" id="pill1"><b>2</b><span>Rede 2</span></div><div class="pill" id="pill2"><b>3</b><span>Servidor</span></div><div class="pill" id="pill3"><b>4</b><span>Relé</span></div></div>
  <div class="track"><div class="fill" id="bar"></div></div>
  <div class="card-wrap">
    <div class="step on" id="step0">
      <div class="sec">Rede 1 — primária</div>
      <label>Redes encontradas</label><select id="ssid"></select>
      <label>Ou SSID manual</label><input id="manual_ssid" placeholder="nome da rede">
      <label>Senha</label><input id="pass" type="text" placeholder="vazio se aberta">
      <button class="btn tbtn" id="t0">Testar conexão</button>
      <div class="ts" id="ts0">Teste a rede para poder avançar.</div>
    </div>
    <div class="step" id="step1">
      <div class="sec">Rede 2 — failover (opcional)</div>
      <label>Redes encontradas</label><select id="ssid2_scan"></select>
      <label>Ou SSID manual</label><input id="ssid2" placeholder="opcional">
      <label>Senha</label><input id="pass2" type="text" placeholder="opcional">
      <button class="btn tbtn" id="t1">Testar conexão</button>
      <div class="ts" id="ts1">Opcional — toque em <b>Pular</b> se não houver rede 2.</div>
    </div>
    <div class="step" id="step2">
      <div class="sec">Identificação</div>
      <label>Node ID</label><input id="nodeid" placeholder="ex: C00045">
      <div class="hint">O servidor vem configurado de fábrica (alterável só no Administrador). O teste confirma o prédio/máquina deste Node ID.</div>
      <button class="btn tbtn" id="t2">Testar WebSocket</button>
      <div class="ts" id="ts2">Testa a conexão com o servidor.</div>
    </div>
    <div class="step" id="step3">
      <div class="sec">Configuração do relé</div>
      <label>Modo do relé</label>
      <div class="trow">
        <div id="modeNormal" class="tgl active">Normal<small>segue WS</small></div>
        <div id="modeOn" class="tgl">Sempre ON</div>
        <div id="modeOff" class="tgl">Sempre OFF</div>
      </div>
      <div class="chk"><input id="invert" type="checkbox"><label for="invert">Inverter lógica do relé — ON=LOW (NF, Normalmente Fechado)</label></div>
      <div class="hint">Em <b>Sempre ON/OFF</b> o relé ignora o WebSocket. Use <b>Normal</b> para o backend ligar/desligar.<br>O relé de 10A da placa aciona o módulo de 30A — não ligue a carga direto no shield.</div>
    </div>
  </div>
  <div class="nav"><button class="btn ghost" id="back">Voltar</button><button class="btn ghost" id="skip" style="display:none">Pular</button><button class="btn" id="next">Avançar</button></div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/">← início</a> · <a href="/info">status (/info)</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
var N=4,cur=0,scanList=[],net1ok=false,wsdone=false,relayModeVal=0;
function paint(){for(var i=0;i<N;i++){qs('step'+i).classList.toggle('on',i===cur);qs('pill'+i).classList.toggle('done',i<cur);qs('pill'+i).classList.toggle('cur',i===cur);}qs('bar').style.width=(cur/(N-1)*100)+'%';qs('back').style.visibility=cur?'visible':'hidden';qs('skip').style.display=(cur===1)?'block':'none';qs('next').textContent=cur===N-1?'Salvar e Reiniciar':'Avançar';}
function msg(t){qs('msg').textContent=t||'';}
function setTs(id,c,t){var e=qs(id);e.className='ts '+c;e.textContent=t;}
function chFor(s){for(var i=0;i<scanList.length;i++)if(scanList[i].ssid===s)return scanList[i].ch;return 0;}
function setRelayMode(v){relayModeVal=v;['modeNormal','modeOn','modeOff'].forEach(function(id,i){qs(id).classList.toggle('active',i===v);});}
function testWifi(ssidVal,passVal,tsId,cb){
  if(!ssidVal){setTs(tsId,'err','Selecione ou digite a rede.');return;}
  setTs(tsId,'run','Testando… o Wi-Fi do ESP pode cair alguns segundos — reconecte se precisar.');
  fetch('/test-wifi?ssid='+encodeURIComponent(ssidVal)+'&pass='+encodeURIComponent(passVal)+'&ch='+chFor(ssidVal)).catch(function(){});
  var tries=0;var poll=function(){fetch('/test-wifi-status').then(function(r){return r.json();}).then(function(j){
    if(j.state==='ok'){setTs(tsId,'ok','✓ Conectou! (RSSI '+j.rssi+' dBm)');cb(true);}
    else if(j.state==='fail'){setTs(tsId,'err','✗ Não conectou. Confira a senha e tente de novo.');cb(false);}
    else if(tries++<30){setTimeout(poll,1500);} else{setTs(tsId,'err','✗ Tempo esgotado. Tente de novo.');cb(false);}
  }).catch(function(){if(tries++<30)setTimeout(poll,1800);else{setTs(tsId,'err','✗ Sem resposta — reconecte ao Wi-Fi do ESP e tente de novo.');cb(false);}});};
  setTimeout(poll,1800);
}
function testWs(){var nid=qs('nodeid').value.trim();if(!nid){setTs('ts2','err','Preencha o Node ID antes de testar.');return;}
  setTs('ts2','run','Testando servidor…');
  fetch('/test-ws?nodeid='+encodeURIComponent(nid)).then(function(r){return r.json();}).then(function(j){wsdone=true;
    if(j.ok&&j.found)setTs('ts2','ok','✓ Conectado! Prédio: '+(j.building||'?')+' · Máquina: '+(j.machine||'?'));
    else if(j.ok)setTs('ts2','err','⚠ Servidor OK, mas o Node ID "'+nid+'" não foi encontrado. Confira o Node ID.');
    else setTs('ts2','err','✗ Não conectou ao servidor. Você pode avançar e revisar depois.');
  }).catch(function(){wsdone=true;setTs('ts2','err','✗ Falha no teste. Você pode avançar mesmo assim.');});}
function next(){msg('');if(cur===N-1)return save();cur++;paint();}
function back(){if(cur>0){cur--;paint();msg('');}}
function save(){var ss=qs('manual_ssid').value.trim()||qs('ssid').value;var ss2=qs('ssid2').value.trim()||qs('ssid2_scan').value;
  if(!qs('nodeid').value.trim()){msg('Preencha o Node ID');return;}msg('Salvando…');
  var b='ssid='+encodeURIComponent(ss)+'&pass='+encodeURIComponent(qs('pass').value)+'&ssid2='+encodeURIComponent(ss2)+'&pass2='+encodeURIComponent(qs('pass2').value)+'&nodeid='+encodeURIComponent(qs('nodeid').value.trim())+'&relayMode='+relayModeVal+'&relayInvert='+(qs('invert').checked?1:0)+'&wizard=1';
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(function(r){return r.text();}).then(function(t){msg(t+' Reconecte ao Wi-Fi em ~5s.');}).catch(function(){msg('Falha ao salvar.');});}
function scan(retry){retry=retry||0;fetch('/scan').then(function(r){return r.json();}).then(function(list){if(list.length===0&&retry<6){setTimeout(function(){scan(retry+1);},2500);return;}scanList=list;
  var s=qs('ssid');s.innerHTML='';var s2=qs('ssid2_scan');s2.innerHTML='<option value="">— nenhuma —</option>';
  list.forEach(function(i){var o=document.createElement('option');o.value=i.ssid;o.textContent=i.ssid+' · '+i.rssi+'dBm · ch'+i.ch+' · '+i.enc;s.appendChild(o);s2.appendChild(o.cloneNode(true));});
  }).catch(function(){if(retry<6)setTimeout(function(){scan(retry+1);},2500);});}
window.onload=function(){
  fetch('/config-data').then(function(r){return r.json();}).then(function(d){qs('manual_ssid').value=d.ssid||'';qs('pass').value=d.pass||'';qs('ssid2').value=d.ssid2||'';qs('pass2').value=d.pass2||'';qs('nodeid').value=d.nodeid||'';setRelayMode(d.relayMode||0);qs('invert').checked=(d.relayInvert===1);}).catch(function(){});
  scan();
  qs('t0').onclick=function(){net1ok=false;testWifi(qs('manual_ssid').value.trim()||qs('ssid').value,qs('pass').value,'ts0',function(ok){net1ok=ok;});};
  qs('t1').onclick=function(){testWifi(qs('ssid2').value.trim()||qs('ssid2_scan').value,qs('pass2').value,'ts1',function(){});};
  qs('t2').onclick=function(){wsdone=false;testWs();};
  qs('modeNormal').onclick=function(){setRelayMode(0);};
  qs('modeOn').onclick=function(){setRelayMode(1);};
  qs('modeOff').onclick=function(){setRelayMode(2);};
  qs('next').onclick=next;qs('back').onclick=back;qs('skip').onclick=function(){if(cur===1){cur++;paint();msg('');}};
  paint();
};
</script></body></html>
)rawliteral";

static const char ADMIN_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy — Admin</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0b130e;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px;min-height:100vh}
header{padding:20px 20px 6px;text-align:center}.logo{color:var(--ac);font-size:22px;font-weight:700;letter-spacing:5px}.sub{color:var(--mu);font-size:10px;letter-spacing:2px;margin-top:3px;text-transform:uppercase}
main{max-width:520px;margin:0 auto;padding:8px 16px 36px}.box{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:14px;margin-top:12px}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:9px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:9px 11px;font-family:inherit;font-size:13px;outline:none}input:focus,select:focus{border-color:var(--ac)}select option{background:var(--cd)}
.trow{display:flex;gap:8px;margin-top:8px}.tgl{flex:1;padding:10px 6px;border:1px solid var(--bd);border-radius:5px;cursor:pointer;text-align:center;background:var(--cd);color:var(--mu);font-size:12px}.tgl.active{border-color:var(--ac);background:#04261433;color:var(--ac);font-weight:700}
.chk{display:flex;align-items:center;gap:10px;margin-top:10px}.chk input{width:15px;height:15px;accent-color:var(--ac)}.chk label{margin:0;font-size:12px;color:var(--tx);text-transform:none;letter-spacing:0}
.btn{width:100%;margin-top:12px;padding:11px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:1px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}.btn:hover{background:var(--ac2);color:var(--tx)}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu)}.btn.ghost:hover{border-color:var(--ac);color:var(--ac)}.btn.danger{background:transparent;border:1px solid var(--red);color:var(--red)}.btn.danger:hover{background:var(--red);color:#000}
.row{display:flex;gap:8px}.row .btn{flex:1}.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}.foot{margin-top:16px;text-align:center;font-size:11px}.foot a{color:var(--mu);text-decoration:none;margin:0 5px}.foot a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">administrador · convencional</div></header>
<main>
  <div class="box"><div class="sec">Node ID</div><input id="nodeid"><button class="btn" id="bNode">Salvar Node ID</button></div>
  <div class="box"><div class="sec">Servidor (WebSocket)</div><label>Host</label><input id="host"><label>Porta</label><input id="port" type="number" min="1" max="65535"><button class="btn" id="bSrv">Salvar servidor</button></div>
  <div class="box"><div class="sec">Rede 1</div><label>Redes</label><select id="ssid"></select><label>Ou SSID manual</label><input id="m1"><label>Senha</label><input id="p1" type="text"><button class="btn" id="bN1">Salvar rede 1</button></div>
  <div class="box"><div class="sec">Rede 2 (failover)</div><label>Redes</label><select id="ssid2"></select><label>Ou SSID manual</label><input id="m2"><label>Senha</label><input id="p2" type="text"><button class="btn" id="bN2">Salvar rede 2</button></div>
  <div class="box"><div class="sec">Relé</div><label>Modo do relé</label>
    <div class="trow"><div id="modeNormal" class="tgl active">Normal</div><div id="modeOn" class="tgl">Sempre ON</div><div id="modeOff" class="tgl">Sempre OFF</div></div>
    <div class="chk"><input id="invert" type="checkbox"><label for="invert">Inverter lógica (ON=LOW, NF)</label></div>
    <button class="btn" id="bRelay">Salvar relé</button></div>
  <div class="box"><div class="sec">Avançado</div><div class="chk"><input id="wsrestart" type="checkbox"><label for="wsrestart">Auto-restart se 30min sem WebSocket</label></div>
    <button class="btn" id="bAdv">Salvar avançado</button>
    <div class="row"><button class="btn ghost" id="bRst">Reiniciar</button><button class="btn danger" id="bClr">Apagar tudo</button></div></div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/">← início</a><a href="/wizard">assistente</a><a href="/relay">relé</a><a href="/info">/info</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
function val(i){return qs(i).value.trim();}
function msg(t){qs('msg').textContent=t;}
var relayModeVal=0;
function setRelayMode(v){relayModeVal=v;['modeNormal','modeOn','modeOff'].forEach(function(id,i){qs(id).classList.toggle('active',i===v);});}
function post(b){msg('Salvando…');fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(function(r){return r.text();}).then(function(t){msg(t);}).catch(function(){msg('Falha.');});}
function save(o){post(Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k]);}).join('&'));}
function scan(){fetch('/scan').then(function(r){return r.json();}).then(function(l){['ssid','ssid2'].forEach(function(id){var s=qs(id);s.innerHTML='<option value="">— escolher —</option>';l.forEach(function(i){var o=document.createElement('option');o.value=i.ssid;o.textContent=i.ssid+' · '+i.rssi+'dBm';s.appendChild(o);});});}).catch(function(){});}
window.onload=function(){
  fetch('/config-data').then(function(r){return r.json();}).then(function(d){qs('nodeid').value=d.nodeid||'';qs('host').value=d.host||'';qs('port').value=d.port||80;setRelayMode(d.relayMode||0);qs('invert').checked=(d.relayInvert===1);qs('wsrestart').checked=(d.wsrestart===1);}).catch(function(){});
  scan();
  qs('modeNormal').onclick=function(){setRelayMode(0);};
  qs('modeOn').onclick=function(){setRelayMode(1);};
  qs('modeOff').onclick=function(){setRelayMode(2);};
  qs('bNode').onclick=function(){if(!val('nodeid')){msg('Preencha o Node ID');return;}save({nodeid:val('nodeid')});};
  qs('bSrv').onclick=function(){if(!val('host')){msg('Preencha o host');return;}save({host:val('host'),port:val('port')||80});};
  qs('bN1').onclick=function(){var s=val('m1')||val('ssid');if(!s){msg('Escolha a rede 1');return;}save({ssid:s,pass:qs('p1').value});};
  qs('bN2').onclick=function(){save({ssid2:(val('m2')||val('ssid2')),pass2:qs('p2').value});};
  qs('bRelay').onclick=function(){save({relayMode:relayModeVal,relayInvert:(qs('invert').checked?1:0)});};
  qs('bAdv').onclick=function(){save({wsrestart:(qs('wsrestart').checked?1:0)});};
  qs('bRst').onclick=function(){if(confirm('Reiniciar o dispositivo?')){msg('Reiniciando…');fetch('/restart');}};
  qs('bClr').onclick=function(){if(confirm('Apagar TODA a configuração e reiniciar?')){msg('Apagando…');fetch('/resetwifi');}};
};
</script></body></html>
)rawliteral";

static const char INFO_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy — Info</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}.logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:880px;margin:0 auto;padding:14px 16px 28px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:8px}
.it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:11px 13px}
.lb{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}.vl{color:var(--tx);font-size:14px;font-weight:600}
.sc{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-top:14px;padding-bottom:6px;border-bottom:1px solid var(--bd)}
pre{font-size:11px;background:#060a07;color:#4ade80;padding:10px;border-radius:3px;max-height:220px;overflow:auto;margin-top:10px;border:1px solid var(--bd);white-space:pre-wrap}
.nav{margin-top:14px;font-size:11px;display:flex;gap:16px}.nav a{color:var(--mu);text-decoration:none}.nav a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">info — refresh 3s</div></header>
<main>
  <div class="grid">
    <div class="it"><div class="lb">Node ID</div><div class="vl" id="nodeId">…</div></div>
    <div class="it"><div class="lb">Wi-Fi (STA)</div><div class="vl" id="ssid">…</div></div>
    <div class="it"><div class="lb">RSSI</div><div class="vl" id="rssi">…</div></div>
    <div class="it"><div class="lb">IP (STA)</div><div class="vl" id="ip_sta">…</div></div>
    <div class="it"><div class="lb">IP (AP)</div><div class="vl" id="ip_ap">…</div></div>
    <div class="it"><div class="lb">WebSocket</div><div class="vl" id="ws">…</div></div>
    <div class="it"><div class="lb">Servidor</div><div class="vl" id="srv">…</div></div>
    <div class="it"><div class="lb">Relé</div><div class="vl" id="relay">…</div></div>
    <div class="it"><div class="lb">Relay Mode</div><div class="vl" id="relayMode">…</div></div>
    <div class="it"><div class="lb">Relay Invert</div><div class="vl" id="relayInvert">…</div></div>
    <div class="it"><div class="lb">Heap livre</div><div class="vl" id="heap">…</div></div>
    <div class="it"><div class="lb">WiFi Slot</div><div class="vl" id="wifiSlot">…</div></div>
    <div class="it"><div class="lb">Boots</div><div class="vl" id="boots">…</div></div>
    <div class="it"><div class="lb">Último reset</div><div class="vl" id="reset">…</div></div>
    <div class="it"><div class="lb">AP ativo</div><div class="vl" id="ap">…</div></div>
  </div>
  <div class="nav"><a href="/wizard">← assistente</a><a href="/admin">→ admin</a><a href="/relay">→ relé</a><a href="/">← menu</a></div>
  <div class="sc">Logs</div><pre id="logs"></pre>
  <div class="sc">Raw</div><pre id="raw"></pre>
</main>
<script>
var modeLabel=['Normal','Sempre ON','Sempre OFF'];
function upd(){
  fetch('/status').then(function(r){return r.json();}).then(function(j){
    document.getElementById('nodeId').textContent=j.nodeId||'—';
    document.getElementById('ssid').textContent=j.ssid||'—';
    document.getElementById('rssi').textContent=(j.rssi||0)+' dBm';
    document.getElementById('ip_sta').textContent=j.ip_sta||'—';
    document.getElementById('ip_ap').textContent=j.ip_ap||'—';
    document.getElementById('ws').textContent=j.wsConnected?'Conectado':'Desconectado';
    document.getElementById('srv').textContent=(j.host||'—')+':'+j.port;
    document.getElementById('relay').textContent=j.relayOn?'ON':'OFF';
    document.getElementById('relay').style.color=j.relayOn?'#00e676':'#f87171';
    document.getElementById('relayMode').textContent=modeLabel[j.relayMode]||j.relayMode;
    document.getElementById('relayInvert').textContent=j.relayInvert?'SIM (NF)':'NÃO (NA)';
    document.getElementById('heap').textContent=j.heap+' B';
    document.getElementById('wifiSlot').textContent='Slot '+(j.wifiSlot+1);
    document.getElementById('boots').textContent=j.boots;
    document.getElementById('reset').textContent=j.resetReason||'—';
    document.getElementById('ap').textContent=j.apActive?'SIM (janela 10 min)':'NÃO';
    document.getElementById('raw').textContent=JSON.stringify(j,null,2);
  }).catch(function(){});
  fetch('/logs').then(function(r){return r.text();}).then(function(t){document.getElementById('logs').textContent=t||'(vazio)';}).catch(function(){});
}
setInterval(upd,3000);upd();
</script></body></html>
)rawliteral";

static const char RELAY_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy — Relé</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:480px;margin:0 auto;padding:18px 16px 32px}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:22px 0 10px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
.state{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:14px 16px;display:flex;align-items:center;justify-content:space-between}
.state-lbl{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase}.state-val{font-size:20px;font-weight:700;color:var(--ac)}
.trow{display:flex;gap:8px;margin-top:8px}.tgl{flex:1;padding:11px 8px;border:1px solid var(--bd);border-radius:3px;cursor:pointer;text-align:center;background:var(--cd);color:var(--mu);font-size:12px;line-height:1.4}
.tgl.active{background:#003d1a;border-color:var(--ac);color:var(--ac);font-weight:700}.tgl small{display:block;font-size:10px;opacity:.7;margin-top:2px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:12px 0 4px}
.chk{display:flex;align-items:center;gap:10px;margin-top:10px;padding:10px 12px;border:1px solid var(--bd);border-radius:3px;background:var(--cd)}.chk input[type=checkbox]{width:15px;height:15px;accent-color:var(--ac);flex-shrink:0}.chk span{font-size:12px;color:var(--tx)}
.btn{display:block;width:100%;margin-top:14px;padding:12px;border:none;border-radius:3px;font-family:inherit;font-size:13px;font-weight:700;letter-spacing:1px;cursor:pointer;text-transform:uppercase}
.btn-save{background:var(--ac);color:#000}.btn-save:hover{background:var(--ac2);color:var(--tx)}
.btn-row{display:flex;gap:8px;margin-top:14px}.btn-on{flex:1;background:#003d1a;border:1px solid var(--ac);color:var(--ac)}.btn-on:hover{background:var(--ac);color:#000}
.btn-off{flex:1;background:#3d0000;border:1px solid var(--red);color:var(--red)}.btn-off:hover{background:var(--red);color:#000}
.st{margin-top:10px;font-size:12px;min-height:14px;color:var(--ac)}
.note{margin-top:12px;color:var(--mu);font-size:11px;line-height:1.5;padding:10px 12px;border:1px solid var(--bd);border-radius:3px;background:var(--cd)}
.nav{margin-top:20px;font-size:11px}.nav a{color:var(--mu);text-decoration:none}.nav a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">controle do relé</div></header>
<main>
  <div class="state"><div><div class="state-lbl">Estado atual</div></div><div class="state-val" id="relayState">…</div></div>
  <div class="sec">Configuração do relé</div>
  <label>Modo do relé</label>
  <div class="trow"><div id="modeNormal" class="tgl active">Normal<small>segue WS</small></div><div id="modeOn" class="tgl">Sempre ON</div><div id="modeOff" class="tgl">Sempre OFF</div></div>
  <div class="chk" style="margin-top:12px"><input type="checkbox" id="invert"><span>Inverter lógica — ON=LOW (NF, Normalmente Fechado)</span></div>
  <button class="btn btn-save" id="saveCfg">Salvar Configuração</button>
  <div class="st" id="cfgStatus"></div>
  <div class="sec">Controle manual</div>
  <div class="btn-row"><button class="btn btn-on" id="btnOn">Ligar</button><button class="btn btn-off" id="btnOff">Desligar</button></div>
  <div class="note">Em <b>Sempre ON/OFF</b>: WS e botões ignorados.<br>Em <b>Normal</b>: "Ligar/Desligar" controla o relé e ele segue os comandos do WebSocket.<br>O relé de 10A da placa aciona o módulo de 30A.</div>
  <div class="nav"><a href="/">← menu</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
var relayModeVal=0;
function setRelayMode(v){relayModeVal=v;['modeNormal','modeOn','modeOff'].forEach(function(id,i){qs(id).classList.toggle('active',i===v);});}
function loadStatus(){fetch('/status').then(function(r){return r.json();}).then(function(j){
  qs('relayState').textContent=j.relayOn?'ON':'OFF';
  qs('relayState').style.color=j.relayOn?'#00e676':'#f87171';
  setRelayMode(j.relayMode||0);qs('invert').checked=(j.relayInvert===1);}).catch(function(){});}
window.onload=function(){
  loadStatus();
  qs('modeNormal').onclick=function(){setRelayMode(0);};
  qs('modeOn').onclick=function(){setRelayMode(1);};
  qs('modeOff').onclick=function(){setRelayMode(2);};
  qs('saveCfg').onclick=function(){qs('cfgStatus').textContent='Salvando...';
    fetch('/relay/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+relayModeVal+'&invert='+(qs('invert').checked?1:0)})
    .then(function(r){return r.text();}).then(function(t){qs('cfgStatus').textContent=t;loadStatus();});};
  qs('btnOn').onclick=function(){fetch('/relay/on').then(function(){loadStatus();});};
  qs('btnOff').onclick=function(){fetch('/relay/off').then(function(){loadStatus();});};
};
</script></body></html>
)rawliteral";

static const char NODEID_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy — Node ID</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--ip:#0d1710}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:420px;margin:0 auto;padding:24px 16px 32px}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin-bottom:6px}
input{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:3px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none}input:focus{border-color:var(--ac)}
.btn{display:block;width:100%;margin-top:16px;padding:13px;background:var(--ac);color:#000;border:none;border-radius:3px;font-family:inherit;font-size:13px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase}.btn:hover{background:var(--ac2);color:var(--tx)}
.st{margin-top:10px;font-size:12px;min-height:16px;color:var(--ac)}
.note{margin-top:20px;padding:12px;background:var(--cd);border:1px solid var(--bd);border-radius:3px;color:var(--mu);font-size:11px;line-height:1.6}.note b{color:var(--tx)}
.nav{margin-top:16px;font-size:11px}.nav a{color:var(--mu);text-decoration:none}.nav a:hover{color:var(--ac)}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">configuração de node id</div></header>
<main>
  <label>Node ID</label><input id="nodeid" placeholder="ex: C00045">
  <button class="btn" id="save">Salvar e Reiniciar</button>
  <div class="st" id="status"></div>
  <div class="note">AP: SSID = <b>NodeID-AP</b> | Senha = <b>12345678</b><br>Ativo por 10 min após cada boot.</div>
  <div class="nav"><a href="/">← menu</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
window.onload=function(){
  fetch('/status').then(function(r){return r.json();}).then(function(s){qs('nodeid').value=s.nodeId||'';}).catch(function(){});
  qs('save').onclick=function(){var id=qs('nodeid').value.trim();
    if(!id){qs('status').textContent='Preencha o NodeID!';return;}
    qs('status').textContent='Salvando...';
    fetch('/savenodeid',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'nodeid='+encodeURIComponent(id)}).then(function(r){return r.text();}).then(function(t){qs('status').textContent=t;});};
};
</script></body></html>
)rawliteral";

// ======================== HTTP handlers =========================
static void handleRoot()       { server.send_P(200, "text/html", LANDING_PAGE); }
static void handleConfigPage() { server.send_P(200, "text/html", WIZARD_PAGE); }
static void handleAdminPage()  { server.send_P(200, "text/html", ADMIN_PAGE); }
static void handleInfoPage()   { server.send_P(200, "text/html", INFO_PAGE); }
static void handleRelayPage()  { server.send_P(200, "text/html", RELAY_PAGE); }
static void handleNodeIdPage() { server.send_P(200, "text/html", NODEID_PAGE); }

static void handleConfigData() {
  char ssEsc[70], pwEsc[134], ss2Esc[70], pw2Esc[134], nidEsc[52], hostEsc[134];
  json_escape_into(P.ssid,   ssEsc,   sizeof(ssEsc));
  json_escape_into(P.pass,   pwEsc,   sizeof(pwEsc));
  json_escape_into(P.ssid2,  ss2Esc,  sizeof(ss2Esc));
  json_escape_into(P.pass2,  pw2Esc,  sizeof(pw2Esc));
  json_escape_into(P.nodeId, nidEsc,  sizeof(nidEsc));
  json_escape_into(P.wsHost, hostEsc, sizeof(hostEsc));

  char buf[840];
  snprintf(buf, sizeof(buf),
    "{\"ssid\":\"%s\",\"pass\":\"%s\",\"ssid2\":\"%s\",\"pass2\":\"%s\","
    "\"nodeid\":\"%s\",\"host\":\"%s\",\"port\":%u,"
    "\"relayMode\":%u,\"relayInvert\":%u,\"wsrestart\":%u}",
    ssEsc, pwEsc, ss2Esc, pw2Esc, nidEsc, hostEsc, (unsigned)P.wsPort,
    (unsigned)P.relayMode, (unsigned)P.relayInvert, (unsigned)P.wsRestartEnabled
  );
  server.send(200, "application/json", buf);
}

static void handleStatusJson() {
  bool staOk = (WiFi.status() == WL_CONNECTED);

  char nidEsc[52], hostEsc[134], ssidEsc[70];
  json_escape_into(P.nodeId, nidEsc,  sizeof(nidEsc));
  json_escape_into(P.wsHost, hostEsc, sizeof(hostEsc));
  json_escape_into(staOk ? WiFi.SSID().c_str() : "", ssidEsc, sizeof(ssidEsc));

  IPAddress ipSta = staOk ? WiFi.localIP() : IPAddress(0,0,0,0);
  IPAddress ipAp  = WiFi.softAPIP();
  char ipsta[20], ipap[20];
  if (staOk) snprintf(ipsta, sizeof(ipsta), "%u.%u.%u.%u", ipSta[0], ipSta[1], ipSta[2], ipSta[3]);
  else       ipsta[0] = '\0';
  snprintf(ipap, sizeof(ipap), "%u.%u.%u.%u", ipAp[0], ipAp[1], ipAp[2], ipAp[3]);

  char buf[700];
  snprintf(buf, sizeof(buf),
    "{\"nodeId\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip_sta\":\"%s\",\"ip_ap\":\"%s\","
    "\"wsConnected\":%s,\"host\":\"%s\",\"port\":%u,"
    "\"relayOn\":%s,\"relayMode\":%u,\"relayInvert\":%u,"
    "\"wifiSlot\":%u,\"boots\":%lu,\"heap\":%lu,\"uptime\":%lu,"
    "\"resetReason\":\"%s\",\"apActive\":%s,\"chip\":\"%s\",\"fw\":\"%s\"}",
    nidEsc, ssidEsc,
    staOk ? WiFi.RSSI() : 0,
    ipsta, ipap,
    isWebSocketConnected ? "true" : "false",
    hostEsc, (unsigned)P.wsPort,
    isRelayEffectiveOn() ? "true" : "false",
    (unsigned)P.relayMode, (unsigned)P.relayInvert,
    (unsigned)wifiSlot,
    (unsigned long)P.bootCount,
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)(millis() / 1000UL),
    resetReasonToStr(P.lastResetReason),
    apEnabled ? "true" : "false",
    FW_CHIP, FW_VERSION
  );
  server.send(200, "application/json", buf);
}

static void handleLogs() {
  if (!apEnabled) { server.send(200, "text/plain", "Logs desativados (lean mode)."); return; }

  const size_t SHOW_MAX = 1400;
  size_t logLen = strnlen(logBuffer, LOG_MAX_LEN);
  const char* start = logBuffer;
  if (logLen > SHOW_MAX) start = logBuffer + (logLen - SHOW_MAX);
  server.send(200, "text/plain", start);
}

// ======================== Endpoints de teste do wizard ========================
static void handleTestWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  int ch = server.hasArg("ch") ? server.arg("ch").toInt() : 0;
  if (ssid.length() == 0) { server.send(400, "application/json", "{\"started\":false}"); return; }
  startWifiTest(ssid, pass, ch);
  server.send(200, "application/json", "{\"started\":true}");
}

static void handleTestWifiStatus() {
  const char* s = (wifiTestState == TST_OK) ? "ok" : (wifiTestState == TST_FAIL) ? "fail" : "testing";
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"rssi\":%d}", s, rssi);
  server.send(200, "application/json", buf);
}

static void handleTestWs() {
  String nid = server.hasArg("nodeid") ? server.arg("nodeid") : String(P.nodeId);
  String building, machine; bool found = false;
  bool ok = testWsSync(nid, building, machine, found);
  building.replace("\\", "\\\\"); building.replace("\"", "\\\"");
  machine.replace("\\", "\\\\");  machine.replace("\"", "\\\"");
  String json = String("{\"ok\":") + (ok ? "true" : "false")
              + ",\"found\":" + (found ? "true" : "false")
              + ",\"building\":\"" + building + "\""
              + ",\"machine\":\""  + machine  + "\"}";
  server.send(200, "application/json", json);
}

// ======================== /scan (assíncrono) ========================
static void handleScan() {
  if (!apEnabled) { server.send(200, "application/json", "[]"); return; }

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "[]"); return; }
  if (n < 0) {                              // WIFI_SCAN_FAILED ou nunca iniciado
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "[]");
    return;
  }

  const int MAXN = 25;
  int outN = (n > MAXN) ? MAXN : n;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (int i = 0; i < outN; i++) {
    if (i > 0) server.sendContent(",");

    char ssEsc[160];
    json_escape_into(WiFi.SSID(i).c_str(), ssEsc, sizeof(ssEsc));

    char obj[260];
    snprintf(obj, sizeof(obj),
      "{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"enc\":\"%s\"}",
      ssEsc, WiFi.RSSI(i), WiFi.channel(i), encToStr((uint8_t)WiFi.encryptionType(i))
    );
    server.sendContent(obj);
    yield();
  }
  server.sendContent("]");

  WiFi.scanDelete();
  WiFi.scanNetworks(true);   // já deixa o próximo scan pronto
}

// ======================== /save (parcial) ========================
// Grava só os campos enviados: o wizard manda tudo, o admin manda subconjuntos.
static void handleSave() {
  bool any = false;
  if (server.hasArg("ssid"))        { memset(P.ssid,  0, sizeof(P.ssid));   server.arg("ssid").toCharArray(P.ssid,   sizeof(P.ssid));   any = true; }
  if (server.hasArg("pass"))        { memset(P.pass,  0, sizeof(P.pass));   server.arg("pass").toCharArray(P.pass,   sizeof(P.pass));   any = true; }
  if (server.hasArg("ssid2"))       { memset(P.ssid2, 0, sizeof(P.ssid2));  server.arg("ssid2").toCharArray(P.ssid2, sizeof(P.ssid2));  any = true; }
  if (server.hasArg("pass2"))       { memset(P.pass2, 0, sizeof(P.pass2));  server.arg("pass2").toCharArray(P.pass2, sizeof(P.pass2));  any = true; }
  if (server.hasArg("nodeid"))      { memset(P.nodeId,0, sizeof(P.nodeId)); server.arg("nodeid").toCharArray(P.nodeId,sizeof(P.nodeId)); any = true; }
  if (server.hasArg("host"))        { memset(P.wsHost,0, sizeof(P.wsHost)); server.arg("host").toCharArray(P.wsHost, sizeof(P.wsHost));  any = true; }
  if (server.hasArg("port"))        { long pt = server.arg("port").toInt(); P.wsPort = (uint16_t)((pt < 1 || pt > 65535) ? WS_PORT : pt); any = true; }
  if (server.hasArg("relayMode"))   { long m = server.arg("relayMode").toInt(); P.relayMode = (uint8_t)((m < 0 || m > 2) ? 0 : m); any = true; }
  if (server.hasArg("relayInvert")) { P.relayInvert      = server.arg("relayInvert").toInt() ? 1 : 0; any = true; }
  if (server.hasArg("wsrestart"))   { P.wsRestartEnabled = server.arg("wsrestart").toInt()   ? 1 : 0; any = true; }
  // "wizard=1" só vem do passo final do assistente (não do /admin). Zera o
  // contador ali para a fidelidade da telemetria: reinícios do bench/teste
  // durante a configuração não devem aparecer como reinícios em campo.
  if (server.hasArg("wizard"))      { P.bootCount = 0; any = true; }

  if (!any) { server.send(200, "text/plain", "Nada para salvar."); return; }

  P.magic = EEPROM_MAGIC;
  P.ver   = EEPROM_VER;
  if (server.hasArg("ssid") || server.hasArg("ssid2")) wifiSlot = 0;

  applyPersistRuntime();

  server.send(200, "text/plain", "Salvando... aguarde reinicio.");
  delay(60);
  yield();

  webSocket.disconnect();
  delay(80);
  WiFi.disconnect(false);
  delay(120);
  yield();

  if (DEBUG_EEPROM_SECTORINFO) debugEepromSectorInfo();
  if (ERASE_SECTOR_BEFORE_SAVE) { diagEraseEepromSector(); delay(50); yield(); }

  bool ok = persistSaveAndVerifyRetry(COMMIT_TRIES);
  EEPROM.end();
  delay(250);
  yield();

  Serial.printf("[EEPROM] save ok=%s\n", ok ? "true" : "false");
  delay(200);
  ESP.restart();
}

static void handleSaveNodeId() {
  if (!server.hasArg("nodeid")) { server.send(400, "text/plain", "nodeid obrigatório"); return; }
  memset(P.nodeId, 0, sizeof(P.nodeId));
  server.arg("nodeid").toCharArray(P.nodeId, sizeof(P.nodeId));
  P.magic = EEPROM_MAGIC;
  P.ver   = EEPROM_VER;

  server.send(200, "text/plain", "NodeID salvo. Reiniciando...");
  delay(60);
  yield();

  if (ERASE_SECTOR_BEFORE_SAVE) { diagEraseEepromSector(); delay(50); yield(); }
  bool ok = persistSaveAndVerifyRetry(COMMIT_TRIES);
  EEPROM.end();
  Serial.printf("[EEPROM] savenodeid ok=%s\n", ok ? "true" : "false");
  delay(300);
  ESP.restart();
}

// ======================== /relay (controle manual, sem restart) ========================
static void handleRelayOn() {
  if (P.relayMode != 0) { server.send(200, "text/plain", "Modo fixo ativo, comando ignorado."); return; }
  relayLogicalOn = true;
  applyRelayOutput();
  if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
  log_append_line("Relé ligado pelo painel.");
  server.send(200, "text/plain", "Relay ON");
}

static void handleRelayOff() {
  if (P.relayMode != 0) { server.send(200, "text/plain", "Modo fixo ativo, comando ignorado."); return; }
  relayLogicalOn = false;
  applyRelayOutput();
  if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:OFF");
  log_append_line("Relé desligado pelo painel.");
  server.send(200, "text/plain", "Relay OFF");
}

// Grava modo/invert sem reiniciar — um restart derrubaria a máquina em uso.
static void handleRelayConfigSave() {
  if (server.hasArg("mode")) {
    long m = server.arg("mode").toInt();
    P.relayMode = (uint8_t)((m < 0 || m > 2) ? 0 : m);
  }
  if (server.hasArg("invert")) {
    P.relayInvert = server.arg("invert").toInt() ? 1 : 0;
  }
  P.magic = EEPROM_MAGIC;
  P.ver   = EEPROM_VER;

  applyPersistRuntime();

  bool ok = persistSaveAndVerifyRetry(COMMIT_TRIES);
  server.send(200, "text/plain", ok ? "Configuração do relé salva." : "Falha ao gravar na EEPROM.");
}

// ======================== Sistema ========================
static void handleRestart() {
  server.send(200, "text/plain", "Reiniciando...");
  delay(300);
  ESP.restart();
}

static void handleResetWifi() {
  persistDefaults();
  persistSaveAndVerifyRetry(COMMIT_TRIES);
  server.send(200, "text/plain", "Configuração apagada. Reiniciando...");
  delay(400);
  ESP.restart();
}

static void handleWiFiStatusPage() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  IPAddress ipSta = staOk ? WiFi.localIP() : IPAddress(0,0,0,0);
  IPAddress ipAp  = WiFi.softAPIP();

  char ssidEsc[80];
  html_escape_into(staOk ? WiFi.SSID().c_str() : "—", ssidEsc, sizeof(ssidEsc));

  char rssiBuf[16];
  if (staOk) snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", WiFi.RSSI());
  else       snprintf(rssiBuf, sizeof(rssiBuf), "—");

  snprintf(htmlBuf, sizeof(htmlBuf),
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Foreasy WiFi</title>"
    "<style>:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}"
    "header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}"
    ".logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}"
    ".sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}"
    "main{max-width:480px;margin:0 auto;padding:18px 16px}"
    ".it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}"
    ".lbl{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase}"
    ".vl{color:var(--tx);font-size:13px;font-weight:600}"
    ".nav{margin-top:16px;font-size:11px}.nav a{color:var(--mu);text-decoration:none}.nav a:hover{color:var(--ac)}"
    "</style></head><body>"
    "<header><div class='logo'>FOREASY</div><div class='sub'>status wi-fi</div></header><main>"
    "<div class='it'><span class='lbl'>SSID</span><span class='vl'>%s</span></div>"
    "<div class='it'><span class='lbl'>RSSI</span><span class='vl'>%s</span></div>"
    "<div class='it'><span class='lbl'>IP (STA)</span><span class='vl'>%u.%u.%u.%u</span></div>"
    "<div class='it'><span class='lbl'>IP (AP)</span><span class='vl'>%u.%u.%u.%u</span></div>"
    "<div class='it'><span class='lbl'>Slot ativo</span><span class='vl'>Rede %u</span></div>"
    "<div class='nav'><a href='/'>← menu</a></div></main></body></html>",
    ssidEsc, rssiBuf,
    ipSta[0], ipSta[1], ipSta[2], ipSta[3],
    ipAp[0],  ipAp[1],  ipAp[2],  ipAp[3],
    (unsigned)(wifiSlot + 1)
  );
  server.send(200, "text/html", htmlBuf);
}

static void handleWSStatusPage() {
  char hostEsc[140];
  html_escape_into(P.wsHost, hostEsc, sizeof(hostEsc));

  snprintf(htmlBuf, sizeof(htmlBuf),
    "<!doctype html><html lang='pt-BR'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Foreasy WS</title>"
    "<style>:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}"
    "header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}"
    ".logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}"
    ".sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}"
    "main{max-width:480px;margin:0 auto;padding:18px 16px}"
    ".it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}"
    ".lbl{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase}"
    ".vl{color:var(--tx);font-size:13px;font-weight:600}"
    ".nav{margin-top:16px;font-size:11px}.nav a{color:var(--mu);text-decoration:none}.nav a:hover{color:var(--ac)}"
    "</style></head><body>"
    "<header><div class='logo'>FOREASY</div><div class='sub'>status websocket</div></header><main>"
    "<div class='it'><span class='lbl'>Conectado</span><span class='vl'>%s</span></div>"
    "<div class='it'><span class='lbl'>Servidor</span><span class='vl'>%s:%u</span></div>"
    "<div class='it'><span class='lbl'>Backoff atual</span><span class='vl'>%lu ms</span></div>"
    "<div class='it'><span class='lbl'>Auto-restart</span><span class='vl'>%s</span></div>"
    "<div class='nav'><a href='/'>← menu</a></div></main></body></html>",
    isWebSocketConnected ? "SIM" : "NÃO",
    hostEsc, (unsigned)P.wsPort,
    (unsigned long)wsNextRetryMs,
    P.wsRestartEnabled ? "SIM" : "NÃO"
  );
  server.send(200, "text/html", htmlBuf);
}

static void handleDiagFlash() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");

  server.sendContent("=== DIAG FLASH/EEPROM ===\nVeja o Serial também.\n\n");

  uint32_t realSz = ESP.getFlashChipRealSize();
  uint32_t ideSz  = ESP.getFlashChipSize();
  uint32_t sector = eepromSectorIndexFromFlashSize(ideSz);

  char buf[200];
  snprintf(buf, sizeof(buf), "Flash Real=%lu IDE=%lu | sector(last)=%lu\n",
           (unsigned long)realSz, (unsigned long)ideSz, (unsigned long)sector);
  server.sendContent(buf);

  bool eok = diagEraseEepromSector();
  snprintf(buf, sizeof(buf), "flashEraseSector(last)=%s\n", eok ? "true" : "false");
  server.sendContent(buf);

  for (int i = 0; i < 64; i++) EEPROM.write(i, (uint8_t)(0xA0 + (i & 0x0F)));
  bool cok = EEPROM.commit();
  snprintf(buf, sizeof(buf), "EEPROM.commit() after pattern write=%s\n", cok ? "true" : "false");
  server.sendContent(buf);

  server.sendContent("\nDump(0..63) buffer:\n");
  for (int i = 0; i < 64; i++) {
    char b2[8];
    snprintf(b2, sizeof(b2), "%02X ", EEPROM.read(i));
    server.sendContent(b2);
    if ((i % 16) == 15) server.sendContent("\n");
  }

  server.sendContent("\n\nATENÇÃO: este teste sobrescreve a configuração. Reconfigure pelo /wizard.\n");
}

static void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ================= AUTENTICAÇÃO DO PAINEL =================
// HTTP Basic Auth (popup nativo do navegador) em vez de página HTML própria —
// no ESP8266 o custo de RAM/flash da tela de login + cookie de sessão não
// compensa (o painel roda no ESP32-C3/S3 com HTML bonito; aqui fica o popup
// mesmo). Senha de fábrica fixa no firmware, de propósito: sem tela de
// "trocar senha" — perder a senha numa peça em campo, sem cabo USB para
// recuperar, é pior do que a senha ser sempre a mesma de fábrica.
const char* PANEL_USER = "admin";
const char* PANEL_PASS = "Foreasy@12345678";

bool checkAuth() {
  if (!server.authenticate(PANEL_USER, PANEL_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static void startWebServer() {
  server.on("/",                 HTTP_GET,  [](){ if (checkAuth()) handleRoot(); });            // landing: Wizard / Administrador
  server.on("/wizard",           HTTP_GET,  [](){ if (checkAuth()) handleConfigPage(); });      // assistente passo a passo
  server.on("/config",           HTTP_GET,  [](){ if (checkAuth()) handleConfigPage(); });      // alias (compat)
  server.on("/admin",            HTTP_GET,  [](){ if (checkAuth()) handleAdminPage(); });       // edições pontuais
  server.on("/config-data",      HTTP_GET,  [](){ if (checkAuth()) handleConfigData(); });
  server.on("/info",             HTTP_GET,  [](){ if (checkAuth()) handleInfoPage(); });
  server.on("/status",           HTTP_GET,  [](){ if (checkAuth()) handleStatusJson(); });
  server.on("/logs",             HTTP_GET,  [](){ if (checkAuth()) handleLogs(); });
  server.on("/scan",             HTTP_GET,  [](){ if (checkAuth()) handleScan(); });
  server.on("/save",             HTTP_POST, [](){ if (checkAuth()) handleSave(); });
  server.on("/test-wifi",        HTTP_GET,  [](){ if (checkAuth()) handleTestWifi(); });
  server.on("/test-wifi-status", HTTP_GET,  [](){ if (checkAuth()) handleTestWifiStatus(); });
  server.on("/test-ws",          HTTP_GET,  [](){ if (checkAuth()) handleTestWs(); });
  server.on("/relay",            HTTP_GET,  [](){ if (checkAuth()) handleRelayPage(); });
  server.on("/relay/on",         HTTP_GET,  [](){ if (checkAuth()) handleRelayOn(); });
  server.on("/relay/off",        HTTP_GET,  [](){ if (checkAuth()) handleRelayOff(); });
  server.on("/relay/config",     HTTP_POST, [](){ if (checkAuth()) handleRelayConfigSave(); });
  server.on("/nodeid",           HTTP_GET,  [](){ if (checkAuth()) handleNodeIdPage(); });
  server.on("/savenodeid",       HTTP_POST, [](){ if (checkAuth()) handleSaveNodeId(); });
  server.on("/wifistatus",       HTTP_GET,  [](){ if (checkAuth()) handleWiFiStatusPage(); });
  server.on("/wsstatus",         HTTP_GET,  [](){ if (checkAuth()) handleWSStatusPage(); });
  server.on("/resetwifi",        HTTP_GET,  [](){ if (checkAuth()) handleResetWifi(); });
  server.on("/restart",          HTTP_GET,  [](){ if (checkAuth()) handleRestart(); });
  server.on("/diagflash",        HTTP_GET,  [](){ if (checkAuth()) handleDiagFlash(); });
  server.onNotFound(handleNotFound);
  server.begin();
}

// ======================== Ticks =========================
static void updateLedTick() {
  bool cur = isWebSocketConnected;
  if (cur != lastLedState) {
    lastLedState = cur;
    setLed(cur);
  }
}

static void wifiTick() {
  if (wifiTestActive) return;   // STA é gerido pelo teste do wizard
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;

      IPAddress ip = WiFi.localIP();
      logf("WiFi conectado. IP=%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

      if (!isWebSocketConnected) {
        log_append_line("WiFi OK: iniciando WebSocket.");
        connectToWebSocket();
      }
    } else if ((millis() - wifiConnectStartMs) > WIFI_MAX_WAIT_MS) {
      wifiConnecting = false;
      logf("Timeout WiFi. Status=%d. Encerrando tentativa.", (int)WiFi.status());
      WiFi.disconnect(false);
      delay(10);
    }
  }

  if (WiFi.status() != WL_CONNECTED && !wifiConnecting && hasSavedWiFi()) {
    uint32_t retryMs = apEnabled ? 20000UL : WIFI_RETRY_INTERVAL_MS;
    if ((millis() - lastWiFiAttemptMs) >= retryMs) {
      lastWiFiAttemptMs = millis();
      logf("WiFi retry. Status=%d", (int)WiFi.status());
      connectToWiFi_begin();
    }
  }
}

static void wsTick() {
  if (wifiTestActive) return;   // sem WS de produção durante um teste do wizard
  webSocket.loop();

  if (isWebSocketConnected) {
    wsLastOkMs = millis();

    if ((millis() - lastAppPingMs) > APP_PING_INTERVAL_MS) {
      lastAppPingMs = millis();
      webSocket.sendTXT("Ping");
      if (apEnabled) log_append_line("AppPing enviado: 'Ping'");
    }

    if ((millis() - lastPingMs) > PING_TIMEOUT_MS) {
      if (apEnabled) log_append_line("WS ZUMBI (sem ping/pong). Resetando WS.");
      webSocket.disconnect();
      isWebSocketConnected = false;
      bumpWsBackoff();
      lastWSConnectAttemptMs = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED && !isWebSocketConnected) {
    if ((millis() - lastWSConnectAttemptMs) >= wsNextRetryMs) {
      lastWSConnectAttemptMs = millis();
      if (apEnabled) logf("WS desconectado. Reconectando (backoff=%lums).", (unsigned long)wsNextRetryMs);
      connectToWebSocket();
    }
  }
}

static void watchdogTick() {
  if (WiFi.status() == WL_CONNECTED || isWebSocketConnected) lastConnectivityOkMs = millis();

  if (!apEnabled) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool wsOk   = isWebSocketConnected;

    // WiFi OK mas WS caído por mais de 5 min => reconexão completa
    if (wifiOk && !wsOk) {
      if (wsDownSinceMs == 0) wsDownSinceMs = millis();
      if ((millis() - wsDownSinceMs) > WS_DOWN_RESET_MS) {
        Serial.println("[CRIT] WATCHDOG: WS down > 5min. Failover/reconexão WiFi+WS.");
        failoverReconnect();
      }
    } else {
      wsDownSinceMs = 0;
    }

    // Sem WiFi e sem WS por mais de 8 min => reconexão completa (sem restart)
    if (!wifiOk && !wsOk) {
      if ((millis() - lastConnectivityOkMs) > GLOBAL_DOWN_RESET_MS) {
        Serial.println("[CRIT] GLOBAL WD: sem WiFi e sem WS por muito tempo. Failover/reconexão.");
        lastConnectivityOkMs = millis(); // reseta o timer para não disparar em loop
        failoverReconnect();
      }
    }
  } else {
    wsDownSinceMs = 0;
  }
}

static void apLifetimeTick() {
  if (apEnabled && (millis() - bootTimeMs >= AP_LIFETIME_MS)) {
    log_append_line("AP lifetime expirou. Desligando AP e desabilitando logs detalhados.");
    WiFi.softAPdisconnect(true);
    apEnabled = false;
    logBuffer[0] = '\0';
    lastConnectivityOkMs = millis();
    if (wifiTestActive) {   // config abandonada: retoma a produção
      wifiTestActive = false;
      wifiTestState  = TST_IDLE;
      WiFi.disconnect(false);
      if (hasSavedWiFi()) connectToWiFi_begin();
    }
  }
}

static void wsRestartTick() {
  if (!P.wsRestartEnabled) return;
  if (isWebSocketConnected)  { wsLastOkMs = millis(); return; }
  if (isRelayEffectiveOn())  { wsLastOkMs = millis(); return; }  // nunca corta máquina em uso
  if ((millis() - wsLastOkMs) > WS_RESTART_TIMEOUT_MS) {
    logf("WS_RESTART: sem WS por 30min. Reiniciando.");
    delay(200);
    ESP.restart();
  }
}

// Restart remoto solicitado via WS 0x06 (executado fora do callback)
static void remoteRestartTick() {
  if (!pendingRestart) return;
  if ((int32_t)(millis() - pendingRestartMs) < 0) return;
  ESP.restart();
}

// ======================== SETUP / LOOP =========================
void setup() {
  Serial.begin(115200);
  delay(60);

  // GPIO0 em nível seguro (relé desligado na lógica NA) antes de qualquer outra coisa.
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);
  pinMode(ledPin, OUTPUT);
  setLed(false);
  lastLedState = false;

  debugFlashInfoOnce();

  logBuffer[0] = '\0';

  Serial.println("[EEPROM] begin (setup)");
  EEPROM.begin(EEPROM_SIZE);
  if (DEBUG_EEPROM_SECTORINFO) debugEepromSectorInfo();

  relayLogicalOn = false;   // convencional sempre nasce desligado
  persistLoad();            // aplica relayMode/relayInvert e escreve o nível correto

  // Atualiza apenas em RAM (não salva no boot)
  P.bootCount++;
  P.lastResetReason = getResetReasonByte();

  bootTimeMs = millis();
  apEnabled  = true;

  log_append_line("Boot do dispositivo iniciado (ESP8266 Convencional).");
  logf("BootCount(RAM)=%lu | LastReset=%s (%u)",
       (unsigned long)P.bootCount, resetReasonToStr(P.lastResetReason), (unsigned)P.lastResetReason);
  logf("NodeID=%s", (P.nodeId[0] ? P.nodeId : "—"));
  logf("SSID salvo=%s", (P.ssid[0] ? P.ssid : "—"));
  logf("Modo=CONVENCIONAL | RelayMode=%u | RelayInvert=%s",
       (unsigned)P.relayMode, relayInvert ? "true" : "false");

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(false); // reconexão totalmente manual via wifiTick/failoverReconnect
  WiFi.persistent(false);

  setupAPSTA();
  startWebServer();
  log_append_line("HTTP server iniciado.");

  WiFi.scanNetworks(true);   // scan assíncrono já pronto para o wizard

  lastConnectivityOkMs = millis();
  wsLastOkMs = millis();
  resetWsBackoff();

  if (hasSavedWiFi()) connectToWiFi_begin();
  else log_append_line("Sem WiFi salvo. Use /wizard no AP.");
}

void loop() {
  yield();

  if (apEnabled) server.handleClient();

  wifiTestTick();
  wifiTick();
  wsTick();
  updateLedTick();
  watchdogTick();
  apLifetimeTick();
  wsRestartTick();
  remoteRestartTick();
}
