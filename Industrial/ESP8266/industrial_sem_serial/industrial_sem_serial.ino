// ============================================================================
// Foreasy ESP8266 — Modelo 2
// Hardware: ESP8266 ESP-01S + shield relay GPIO V1
//   Relay controlado diretamente via GPIO0 (sem MCU intermediário).
//   Serial disponível em 115200 baud para debug (diferente do Modelo 1).
//
// SISTEMA: Industrial — pulso elétrico para liberar um ciclo.
//
//   WS 0x01 => PULSO: relay fecha por PULSE_MS (100ms) depois abre automaticamente
//   WS 0x02 => ignorado
//   Usado para máquinas que aceitam um pulso elétrico para liberar um ciclo.
//
// RELAY INVERT (relayInvert) — útil para relay NF (Normalmente Fechado):
//   0 => normal    (ON=HIGH / OFF=LOW)
//   1 => invertido (ON=LOW  / OFF=HIGH)
//
// ATENÇÃO — GPIO0 é strapping pin do ESP8266:
//   Durante o boot o GPIO0 deve estar HIGH (relay desligado se NA).
//   O firmware força LOW logo no setup para garantir relay desligado.
//   Watchdogs sem restart automático para evitar acionamento indesejado no boot.
//
// MONITORAMENTO:
//   WS 0x03 => responde JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot,
//              machineMode, pulse, chip, fw
//   WS 0x06 => restart remoto: responde "Restarting" e reinicia após ~200ms
//   (0x04 OTA e 0x05 AVAIL não existem no ESP-01S: 1MB de flash / sem GPIO livre)
//
// WIFI:
//   Dual WiFi: failover automático entre rede 1 e rede 2 sem restart.
//   Conexão não-bloqueante (wifiTick). Credenciais nunca apagadas por falha.
//   Scan WiFi síncrono (bloqueia durante o scan — diferença do Modelo 1).
//
// WEBSOCKET:
//   Backoff exponencial 10s → 120s.
//   Watchdog WS  : sem WS >5min  → failover WiFi+WS.
//   Watchdog geral: sem WiFi+WS >8min → failover.
//   wsRestartEnabled: reinicia o ESP após 1h sem WS (configurável via /config).
//
// AP: ativo 10 min após boot — SSID: <nodeId>-AP | Senha: 12345678
//     Após expirar: AP desliga (lean mode). Logs críticos continuam via Serial.
//
// DEBUG (flags habilitadas neste arquivo):
//   DEBUG_FLASH_INFO=true, DEBUG_EEPROM_DUMP=true, DEBUG_EEPROM_SECTORINFO=true
//   Útil para diagnóstico de falhas de EEPROM.commit() em campo.
//
// EEPROM v4 (magic 0xF0EA5E01):
//   ssid[32], pass[64], ssid2[32], pass2[64], nodeId[24],
//   machineMode, relayInvert, bootCount, lastResetReason, wsRestartEnabled.
//   Apaga setor antes de salvar + até 3 tentativas de commit com verificação.
//   bootCount incrementado em RAM; não salvo automaticamente a cada boot.
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

// ATENÇÃO: /save pode apagar setor antes de gravar. Útil para diagnóstico.
// Se quiser mais conservador depois, mude para false.
static const bool ERASE_SECTOR_BEFORE_SAVE = true;

// Quantas tentativas de commit
static const uint8_t COMMIT_TRIES = 3;

// ======================== WS =========================
static const char*    WS_HOST = "frst-back-02b607761078.herokuapp.com";
static const uint16_t WS_PORT = 80;

// Identidade do firmware (reportada no WS 0x03 — auditoria da frota / seleção de OTA)
#define FW_VERSION "1.0.0"
#define FW_CHIP    "esp8266"   // ESP-01S (Modelo 2)

// ======================== IO (ESP-01/ESP-01S / Generic ESP8266) =========================
static const int  relayPin = 0;  // GPIO0
static const int  ledPin   = 2;  // GPIO2 (LED onboard costuma ser ativo LOW)
static const bool LED_ACTIVE_LOW = true;

// Pulso industrial
static const uint16_t PULSE_MS = 100;

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
static const uint32_t WS_RESTART_TIMEOUT_MS = 60UL * 60UL * 1000UL; // 1 hora

// Restart remoto (WS 0x06) — adiado p/ a resposta "Restarting" sair antes do reboot
static bool     pendingRestart   = false;
static uint32_t pendingRestartMs = 0;

// ======================== RELAY (pulso) =========================
// invert levels (útil p/ relay NF, Normalmente Fechado)
static bool relayInvert = false;
static int  relayOnLevel  = HIGH;
static int  relayOffLevel = LOW;

// pulso
static bool     pulseActive = false;
static uint32_t pulseEndMs  = 0;

// ======================== HTTP =========================
ESP8266WebServer server(80);

// ======================== LOG =========================
static const size_t LOG_MAX_LEN = 2500;
static char logBuffer[LOG_MAX_LEN + 1];

// ======================== EEPROM =========================
#define EEPROM_SIZE 512
static const uint32_t EEPROM_MAGIC = 0xF0EA5E01;
static const uint16_t EEPROM_VER   = 4; // v4: wsRestartEnabled (auto-restart sem WS)

struct __attribute__((packed)) Persist {
  uint32_t magic;
  uint16_t ver;

  char ssid[32];
  char pass[64];
  char ssid2[32];         // rede 2 (failover), opcional
  char pass2[64];         // senha rede 2
  char nodeId[24];

  uint8_t  machineMode;   // reservado (sempre industrial; mantido p/ layout da EEPROM)
  uint8_t  relayInvert;   // 0 normal, 1 invert

  uint32_t bootCount;     // incrementa em RAM no boot (não commit automático)
  uint8_t  lastResetReason;

  uint8_t  wsRestartEnabled; // 0=off, 1=reinicia após 1h sem WS
  uint8_t  reserved[5];
};

Persist P;

// ======================== Helpers (LED/Relay) =========================
static inline void setLed(bool on) {
  if (LED_ACTIVE_LOW) digitalWrite(ledPin, on ? LOW : HIGH);
  else                digitalWrite(ledPin, on ? HIGH : LOW);
}

static void updateRelayLevels() {
  if (!relayInvert) { relayOnLevel = HIGH; relayOffLevel = LOW; }
  else              { relayOnLevel = LOW;  relayOffLevel = HIGH; }
}

static void applyRelayPhysical(bool on) {
  digitalWrite(relayPin, on ? relayOnLevel : relayOffLevel);
}

static void relayOffSafe() {
  pulseActive = false;
  applyRelayPhysical(false);
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
  Serial.printf("[EEPROM] EEPROM_SIZE=%u bytes\n", (unsigned)EEPROM_SIZE);
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

  // default NodeID (ajuste se quiser)
  strncpy(P.nodeId, "ForeasyInativa01", sizeof(P.nodeId) - 1);

  // defaults novos
  P.machineMode = 1;  // industrial (fixo)
  P.relayInvert = 0;
}

static void applyPersistRuntime() {
  relayInvert = (P.relayInvert != 0);
  updateRelayLevels();
  relayOffSafe();
}

static void persistLoad() {
  EEPROM.get(0, P);

  Serial.printf("[EEPROM] boot read: magic=0x%08lX ver=%u ssid='%s' node='%s' mode=%u invert=%u\n",
                (unsigned long)P.magic, (unsigned)P.ver, P.ssid, P.nodeId,
                (unsigned)P.machineMode, (unsigned)P.relayInvert);

  if (DEBUG_EEPROM_DUMP) debugEepromDump(0, 64);

  if (P.magic == EEPROM_MAGIC && P.ver == 3) {
    Serial.println("[EEPROM] v3->v4 migration: preservando config, wsRestartEnabled=0");
    P.ver = EEPROM_VER;
    P.wsRestartEnabled = 0;
    memset(P.reserved, 0, sizeof(P.reserved));
    EEPROM.put(0, P);
    bool ok = EEPROM.commit();
    Serial.printf("[EEPROM] migration commit=%s\n", ok ? "true" : "false");
  } else if (P.magic != EEPROM_MAGIC || P.ver != EEPROM_VER) {
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
                (unsigned)T.machineMode, (unsigned)T.relayInvert);

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
static void setupAPSTA() {
  char apName[40];
  snprintf(apName, sizeof(apName), "%s-AP", P.nodeId[0] ? P.nodeId : "FOREASY");
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
  WiFi.mode(WIFI_AP_STA);
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

      // evita String: "ID:<nodeId>"
      char hello[48];
      snprintf(hello, sizeof(hello), "ID:%s", (P.nodeId[0] ? P.nodeId : "FOREASY"));
      webSocket.sendTXT(hello);

      if (apEnabled) {
        logf("WS CONNECTED. Ident: %s", hello);
        logf("Modo=INDUSTRIAL(PULSE) | RelayInvert=%s | Levels ON=%s OFF=%s",
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
      webSocket.sendTXT("Pong"); // mantido
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
          // Responde com informações de monitoramento do dispositivo
          bool   staOk   = (WiFi.status() == WL_CONNECTED);
          int    rssi    = staOk ? WiFi.RSSI() : 0;
          int    ch      = staOk ? (int)WiFi.channel() : 0;
          uint32_t heap  = ESP.getFreeHeap();
          uint32_t block = ESP.getMaxFreeBlockSize();
          uint8_t  cpu   = ESP.getCpuFreqMHz();
          uint32_t up    = millis() / 1000UL;

          char buf[320];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%lu,\"block\":%lu,\"cpu\":%u,\"uptime\":%lu,\"boots\":%lu,"
            "\"wifiSlot\":%u,\"machineMode\":1,\"pulse\":%s,\"chip\":\"%s\",\"fw\":\"%s\"}",
            rssi, ch,
            (unsigned long)heap,
            (unsigned long)block,
            (unsigned)cpu,
            (unsigned long)up,
            (unsigned long)P.bootCount,
            (unsigned)wifiSlot,
            pulseActive ? "true" : "false",
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

        // pulso só em 0x01
        if (b == 0x01) {
          if (!pulseActive) {
            pulseActive = true;
            pulseEndMs = millis() + PULSE_MS;
            applyRelayPhysical(true);
            if (apEnabled) logf("WS BIN 0x01: Pulso iniciado (%ums).", PULSE_MS);
          } else {
            if (apEnabled) log_append_line("WS BIN 0x01: pulso já ativo, ignorando.");
          }
        } else if (b == 0x02) {
          if (apEnabled) log_append_line("WS BIN 0x02 ignorado.");
        } else {
          if (apEnabled) logf("WS BIN desconhecido: %u", (unsigned)b);
        }
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

  if (apEnabled) logf("Iniciando WebSocket: %s:%u path:/", WS_HOST, WS_PORT);

  webSocket.disconnect();
  delay(20);

  webSocket.begin(WS_HOST, WS_PORT, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.enableHeartbeat(15000, 3000, 2);

  lastPingMs = millis();
  lastAppPingMs = 0;
  lastWSConnectAttemptMs = millis();

  if (apEnabled) log_append_line("WS iniciado (aguardando CONNECTED).");
}

// ======================== HTTP pages =========================
static const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
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
.lnk{margin-top:20px;display:flex;gap:16px;font-size:11px}
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
  <label>SSID</label>
  <input id="ssid2" placeholder="opcional">
  <label>Senha</label>
  <input id="pass2" type="text" placeholder="opcional">

  <div class="sec">Dispositivo</div>
  <label>Node ID</label>
  <input id="nodeid" placeholder="ex: I00047">

  <div class="chk">
    <input id="invert" type="checkbox">
    <label for="invert">Inverter lógica do relé (ON=LOW / OFF=HIGH)</label>
  </div>
  <div class="chk">
    <input id="wsrestart" type="checkbox">
    <label for="wsrestart">Auto-restart se sem WebSocket por 1 hora</label>
  </div>

  <button class="btn" id="save">Salvar e Reiniciar</button>
  <div class="st" id="status"></div>
  <div class="lnk">
    <a href="/info">→ /info</a>
    <a href="/diagflash" target="_blank">→ /diagflash</a>
  </div>
</main>
<script>
function qs(id){return document.getElementById(id);}

function encText(enc){
  if(enc===0) return 'Open';
  if(enc===1) return 'WEP';
  if(enc===2) return 'WPA-PSK';
  if(enc===3) return 'WPA2-PSK';
  if(enc===4) return 'WPA/WPA2-PSK';
  return '?';
}

function scan(){
  fetch('/scan').then(r=>r.json()).then(list=>{
    let s=qs('ssid');
    s.innerHTML='';
    list.forEach(i=>{
      let o=document.createElement('option');
      o.value=i.ssid;
      o.textContent = i.ssid + ' | ' + i.rssi + ' dBm | CH ' + i.channel + ' | ' + encText(i.enc);
      s.appendChild(o);
    });
  }).catch(()=>{});
}

window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('manual_ssid').value = d.ssid   || '';
    qs('pass').value        = d.pass   || '';
    qs('ssid2').value       = d.ssid2  || '';
    qs('pass2').value       = d.pass2  || '';
    qs('nodeid').value      = d.nodeid || '';
    qs('invert').checked    = (d.invert    === 1);
    qs('wsrestart').checked = (d.wsrestart === 1);
  }).catch(()=>{});

  scan();

  qs('save').onclick=()=>{
    let ss=qs('manual_ssid').value.trim() || qs('ssid').value;
    let pw=qs('pass').value;
    let ss2=qs('ssid2').value.trim();
    let pw2=qs('pass2').value;
    let id=qs('nodeid').value.trim();
    let inv = qs('invert').checked ? '1' : '0';
    let wsr = qs('wsrestart').checked ? '1' : '0';

    if(!ss){ qs('status').textContent='Preencha o SSID da rede 1!'; return; }
    if(!id){ qs('status').textContent='Preencha o NodeID!'; return; }

    qs('status').textContent='Salvando...';

    fetch('/save',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:
        'ssid='+encodeURIComponent(ss)+
        '&pass='+encodeURIComponent(pw)+
        '&ssid2='+encodeURIComponent(ss2)+
        '&pass2='+encodeURIComponent(pw2)+
        '&nodeid='+encodeURIComponent(id)+
        '&invert='+encodeURIComponent(inv)+
        '&wsrestart='+encodeURIComponent(wsr)
    }).then(r=>r.text()).then(t=>{
      qs('status').textContent=t;
    }).catch(()=>{
      qs('status').textContent='Falha ao salvar.';
    });
  };
};
</script>
</body></html>
)rawliteral";

// ======================== HTTP handlers =========================
static void handleConfigData() {
  char ssEsc[70], pwEsc[134], ss2Esc[70], pw2Esc[134], nidEsc[52];
  json_escape_into(P.ssid,   ssEsc,  sizeof(ssEsc));
  json_escape_into(P.pass,   pwEsc,  sizeof(pwEsc));
  json_escape_into(P.ssid2,  ss2Esc, sizeof(ss2Esc));
  json_escape_into(P.pass2,  pw2Esc, sizeof(pw2Esc));
  json_escape_into(P.nodeId, nidEsc, sizeof(nidEsc));

  char buf[540];
  snprintf(buf, sizeof(buf),
    "{\"ssid\":\"%s\",\"pass\":\"%s\",\"ssid2\":\"%s\",\"pass2\":\"%s\","
    "\"nodeid\":\"%s\",\"mode\":1,\"invert\":%u,\"wsrestart\":%u}",
    ssEsc, pwEsc, ss2Esc, pw2Esc, nidEsc,
    (unsigned)P.relayInvert, (unsigned)P.wsRestartEnabled
  );
  server.send(200, "application/json", buf);
}

static void handleRoot() {
  server.sendHeader("Location", "/config", true);
  server.send(302, "text/plain", "");
}

static void handleConfigPage() {
  server.send_P(200, "text/html", CONFIG_PAGE);
}

static void handleScan() {
  if (!apEnabled) { server.send(200, "application/json", "[]"); return; }

  log_append_line("Scan WiFi (/scan) iniciado...");
  int n = WiFi.scanNetworks();
  logf("Scan (/scan) concluído. n=%d", n);

  const int MAXN = 25;
  int outN = (n > MAXN) ? MAXN : n;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  server.sendContent("[");
  for (int i = 0; i < outN; i++) {
    if (i > 0) server.sendContent(",");

    String ssidS = WiFi.SSID(i);
    char ssEsc[160];
    json_escape_into(ssidS.c_str(), ssEsc, sizeof(ssEsc));

    char obj[240];
    snprintf(obj, sizeof(obj),
      "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"enc\":%d}",
      ssEsc, WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i)
    );
    server.sendContent(obj);
  }
  server.sendContent("]");
  WiFi.scanDelete();
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

  server.sendContent("\n\nReinicie o módulo e veja se permanece.\n");
}

static void handleInfoPage() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  int rssi = staOk ? WiFi.RSSI() : 0;

  IPAddress ipSta = staOk ? WiFi.localIP() : IPAddress(0,0,0,0);
  IPAddress ipAp  = WiFi.softAPIP();

  char rssiBuf[12];
  if (staOk) snprintf(rssiBuf, sizeof(rssiBuf), "%d", rssi);
  else       snprintf(rssiBuf, sizeof(rssiBuf), "—");

  char ipsta[24];
  if (staOk) snprintf(ipsta, sizeof(ipsta), "%u.%u.%u.%u", ipSta[0], ipSta[1], ipSta[2], ipSta[3]);
  else       snprintf(ipsta, sizeof(ipsta), "—");

  char ipap[24];
  snprintf(ipap, sizeof(ipap), "%u.%u.%u.%u", ipAp[0], ipAp[1], ipAp[2], ipAp[3]);

  char bootsBuf[16]; snprintf(bootsBuf, sizeof(bootsBuf), "%lu", (unsigned long)P.bootCount);
  const char* lastResetStr = resetReasonToStr(P.lastResetReason);

  char wifiSavedBuf[64];
  if (wifiSlot == 0)
    snprintf(wifiSavedBuf, sizeof(wifiSavedBuf), "Rede 1 (%s)", P.ssid[0] ? P.ssid : "—");
  else
    snprintf(wifiSavedBuf, sizeof(wifiSavedBuf), "Rede 2 (%s)", P.ssid2[0] ? P.ssid2 : "—");

  const char* srcLog = (apEnabled ? logBuffer : "");
  size_t logLen = strnlen(srcLog, LOG_MAX_LEN);
  const char* logStart = srcLog;
  const size_t SHOW_MAX = 1400;
  if (logLen > SHOW_MAX) logStart = srcLog + (logLen - SHOW_MAX);

  static char logEsc[2200];
  static char page[6800];
  html_escape_into(logStart, logEsc, sizeof(logEsc));

  // Status do relé (físico)
  const char* relayPhys = (digitalRead(relayPin) == relayOnLevel) ? "ON-level" : "OFF-level";

  int n = snprintf(
    page, sizeof(page),
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='2'>"
    "<title>Foreasy Info</title>"
    "<style>"
    ":root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:13px}"
    "header{background:var(--cd);border-bottom:1px solid var(--bd);padding:13px 18px}"
    ".logo{color:var(--ac);font-size:16px;font-weight:700;letter-spacing:3px}"
    ".sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}"
    "main{max-width:880px;margin:0 auto;padding:14px 16px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px}"
    ".it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:11px 13px}"
    ".lb{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}"
    ".vl{color:var(--tx);font-size:14px;font-weight:600}"
    ".bk{display:inline-block;padding:2px 7px;border-radius:2px;font-size:12px;background:#003d1a;border:1px solid var(--bd);color:var(--ac)}"
    ".log{font-size:11px;white-space:pre-wrap;background:#060a07;color:#4ade80;padding:10px;border-radius:3px;max-height:280px;overflow:auto;margin-top:12px;border:1px solid var(--bd)}"
    "a{color:var(--ac);text-decoration:none}"
    "</style></head><body>"
    "<header><div class='logo'>FOREASY</div><div class='sub'>info — refresh 2s</div></header>"
    "<main><div class='grid'>"
    "<div class='it'><div class='lb'>Node ID</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>WiFi salvo</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>WiFi atual</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>RSSI</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>IP (STA)</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>IP (AP)</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>WebSocket</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>Relé invert</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>Relé físico</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>Pulso</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>AP ativo</div><div class='vl'>%s</div></div>"
    "<div class='it'><div class='lb'>Boots</div><div class='vl'><span class='bk'>%s</span></div></div>"
    "<div class='it'><div class='lb'>Último reset</div><div class='vl'>%s (%u)</div></div>"
    "</div>",
    (P.nodeId[0] ? P.nodeId : "—"),
    wifiSavedBuf,
    (staOk ? WiFi.SSID().c_str() : "—"),
    rssiBuf,
    ipsta,
    ipap,
    (isWebSocketConnected ? "Conectado" : "Desconectado"),
    (relayInvert ? "SIM" : "NAO"),
    relayPhys,
    (pulseActive ? "ATIVO" : "inativo"),
    (apEnabled ? "SIM (janela 10 min)" : "NAO"),
    bootsBuf,
    lastResetStr,
    (unsigned)P.lastResetReason
  );

  if (n <= 0) { server.send(500, "text/plain", "error"); return; }

  if (apEnabled) {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent(page);
    server.sendContent("<div style='margin-top:14px;color:#4ade80;font-size:10px;letter-spacing:1px;text-transform:uppercase;padding-bottom:6px;border-bottom:1px solid #1e3028'>Logs</div><div class='log'>");
    server.sendContent(logEsc);
    server.sendContent("</div>");
    server.sendContent("<div style='margin-top:14px;font-size:11px'><a href='/config' style='color:#00e676;text-decoration:none'>← /config</a></div></main></body></html>");
  } else {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent(page);
    server.sendContent("<div style='margin-top:14px;color:#557060;font-size:11px'>Logs desativados (lean mode).</div>");
    server.sendContent("<div style='margin-top:14px;font-size:11px'><a href='/config' style='color:#00e676;text-decoration:none'>← /config</a></div></main></body></html>");
  }
}

static void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("nodeid")) {
    server.send(400, "text/plain", "ssid and nodeid required");
    return;
  }

  String ssidS  = server.arg("ssid");
  String passS  = server.arg("pass");
  String ssid2S = server.arg("ssid2");
  String pass2S = server.arg("pass2");
  String nidS   = server.arg("nodeid");

  uint8_t inv  = (server.hasArg("invert") ? (uint8_t)server.arg("invert").toInt() : 0);
  uint8_t wsr  = (server.hasArg("wsrestart") ? (uint8_t)server.arg("wsrestart").toInt() : 0);

  memset(P.ssid,  0, sizeof(P.ssid));
  memset(P.pass,  0, sizeof(P.pass));
  memset(P.ssid2, 0, sizeof(P.ssid2));
  memset(P.pass2, 0, sizeof(P.pass2));
  memset(P.nodeId, 0, sizeof(P.nodeId));

  ssidS.toCharArray(P.ssid,   sizeof(P.ssid));
  passS.toCharArray(P.pass,   sizeof(P.pass));
  ssid2S.toCharArray(P.ssid2, sizeof(P.ssid2));
  pass2S.toCharArray(P.pass2, sizeof(P.pass2));
  nidS.toCharArray(P.nodeId,  sizeof(P.nodeId));

  P.magic = EEPROM_MAGIC;
  P.ver   = EEPROM_VER;

  P.machineMode      = 1;  // sempre industrial
  P.relayInvert      = (inv  == 0) ? 0 : 1;
  P.wsRestartEnabled = (wsr  == 0) ? 0 : 1;

  wifiSlot = 0; // sempre começa pela rede 1 após salvar

  // aplica em runtime (antes do restart)
  applyPersistRuntime();

  server.send(200, "text/plain", "Salvando... aguarde reinicio.");
  delay(60);
  yield();

  webSocket.disconnect();
  delay(80);

  WiFi.disconnect(false);
  delay(120);
  yield();

  Serial.println("[EEPROM] save requested");
  if (DEBUG_EEPROM_SECTORINFO) debugEepromSectorInfo();

  if (ERASE_SECTOR_BEFORE_SAVE) {
    bool eok = diagEraseEepromSector();
    Serial.printf("[EEPROM] eraseBeforeSave=%s\n", eok ? "true" : "false");
    delay(50);
    yield();
  }

  bool ok = persistSaveAndVerifyRetry(COMMIT_TRIES);

  EEPROM.end();
  delay(250);
  yield();

  Serial.printf("[EEPROM] save ok=%s\n", ok ? "true" : "false");
  delay(200);

  ESP.restart();
}

static void handleNotFound() { server.send(404, "text/plain", "Not found"); }

static void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/config-data", HTTP_GET, handleConfigData);
  server.on("/info", HTTP_GET, handleInfoPage);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/diagflash", HTTP_GET, handleDiagFlash);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ======================== Ticks =========================
static void handleRelayTick() {
  // pulso industrial
  if (pulseActive && (int32_t)(millis() - pulseEndMs) >= 0) {
    pulseActive = false;
    applyRelayPhysical(false);
    if (apEnabled) log_append_line("Pulso finalizado (relay OFF).");
    if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
  }
}

static void updateLedTick() {
  bool cur = isWebSocketConnected;
  if (cur != lastLedState) {
    lastLedState = cur;
    setLed(cur);
  }
}

static void wifiTick() {
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
  webSocket.loop();

  if (isWebSocketConnected && (millis() - lastAppPingMs) > APP_PING_INTERVAL_MS) {
    lastAppPingMs = millis();
    webSocket.sendTXT("Ping");
    if (apEnabled) log_append_line("AppPing enviado: 'Ping'");
  }

  if (isWebSocketConnected && (millis() - lastPingMs) > PING_TIMEOUT_MS) {
    if (apEnabled) log_append_line("WS ZUMBI (sem ping/pong). Resetando WS.");
    webSocket.disconnect();
    isWebSocketConnected = false;
    bumpWsBackoff();
    lastWSConnectAttemptMs = 0;
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
  }
}

static void wsRestartTick() {
  if (!P.wsRestartEnabled) return;
  if (isWebSocketConnected) { wsLastOkMs = millis(); return; }
  if ((millis() - wsLastOkMs) > WS_RESTART_TIMEOUT_MS) {
    logf("WS_RESTART: sem WS por 1h. Reiniciando.");
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

  debugFlashInfoOnce();

  pinMode(ledPin, OUTPUT);
  pinMode(relayPin, OUTPUT);
  digitalWrite(0, LOW); // mantém relé desligado (se for ativo LOW)

  setLed(false);
  lastLedState = false;

  // default relay levels
  //updateRelayLevels();
  relayOffSafe();

  logBuffer[0] = '\0';

  Serial.println("[EEPROM] begin (setup)");
  EEPROM.begin(EEPROM_SIZE);
  if (DEBUG_EEPROM_SECTORINFO) debugEepromSectorInfo();

  persistLoad();

  // Atualiza apenas em RAM (não salva no boot)
  P.bootCount++;
  P.lastResetReason = getResetReasonByte();

  bootTimeMs = millis();
  apEnabled = true;

  log_append_line("Boot do dispositivo iniciado (ESP8266 Unified AP).");
  logf("BootCount(RAM)=%lu | LastReset=%s (%u)",
       (unsigned long)P.bootCount, resetReasonToStr(P.lastResetReason), (unsigned)P.lastResetReason);
  logf("NodeID=%s", (P.nodeId[0] ? P.nodeId : "—"));
  logf("SSID salvo=%s", (P.ssid[0] ? P.ssid : "—"));
  logf("Modo=INDUSTRIAL(PULSE) | RelayInvert=%s",
       relayInvert ? "true" : "false");

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(false); // reconexão totalmente manual via wifiTick/failoverReconnect
  WiFi.persistent(false);

  setupAPSTA();
  startWebServer();
  log_append_line("HTTP server iniciado.");

  lastConnectivityOkMs = millis();
  wsLastOkMs = millis();
  resetWsBackoff();

  if (hasSavedWiFi()) connectToWiFi_begin();
  else log_append_line("Sem WiFi salvo. Use /config no AP.");
}

void loop() {
  yield();

  if (apEnabled) server.handleClient();

  handleRelayTick();
  wifiTick();
  wsTick();
  updateLedTick();
  watchdogTick();
  apLifetimeTick();
  wsRestartTick();
  remoteRestartTick();
}
