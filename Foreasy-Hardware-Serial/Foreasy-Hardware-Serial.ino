// ============================================================================
// Foreasy ESP8266 — Unified AP (Industrial + Conventional) + Serial Relay Control
// Placa alvo: Generic ESP8266 (ESP-01/ESP-01S) + AZ-Delivery Relay Shield (STC15F104W)
// AP+STA + WebSocket + /config /info + EEPROM persistente
//
// RELÉ: Controlado via Serial (9600 baud) para STC15F104W (não GPIO)
// MODO DE MÁQUINA (persistente):
// - Convencional: WS BIN 0x01 => RELÉ ON | 0x02 => RELÉ OFF
// - Industrial  : WS BIN 0x01 => PULSO no relé (PULSE_MS) | ignora 0x02
//
// RELÉ INVERT (persistente):
// - invert=0 => Comandos normais (ON=relayOnCmd / OFF=relayOffCmd)
// - invert=1 => Comandos invertidos (ON=relayOffCmd / OFF=relayOnCmd) — útil para NC
//
// MONITORAMENTO (independe do modo):
// - WS BIN 0x03 => responde JSON: rssi, ch, heap, block, cpu, uptime, boots
//
// AP: fica ativo por 10 minutos após boot, depois desliga (lean mode)
// Logs detalhados só enquanto AP está ativo.
//
// SEM restart automático nos watchdogs — apenas reconexão WiFi/WS
// O único ESP.restart() que permanece é no /save (intencional pelo usuário).
//
// DICA ESP-01/ESP-01S: Flash Mode = DOUT (quando houver commit falhando)
// ============================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsClient.h>
#include <EEPROM.h>
#include <stdarg.h>


// ======================== CONFIG DEBUG =========================
// DESATIVADO: Serial é exclusiva do relé (STC15F104W). Prints corrompem comandos.
static const bool DEBUG_FLASH_INFO        = false;
static const bool DEBUG_EEPROM_DUMP       = false;
static const bool DEBUG_EEPROM_SECTORINFO = false;

// ATENÇÃO: /save pode apagar setor antes de gravar. Útil para diagnóstico.
// Se quiser mais conservador depois, mude para false.
static const bool ERASE_SECTOR_BEFORE_SAVE = true;

// Quantas tentativas de commit
static const uint8_t COMMIT_TRIES = 3;

// ======================== WS =========================
static const char*    WS_HOST = "frst-back-02b607761078.herokuapp.com";
static const uint16_t WS_PORT = 80;

// ======================== IO (ESP-01/ESP-01S / Generic ESP8266) =========================
// NOTA: Relé controlado via Serial (STC15F104W), não via GPIO
static const int  ledPin   = 2;  // GPIO2 (LED onboard costuma ser ativo LOW)
static const bool LED_ACTIVE_LOW = true;

// Pulso industrial
static const uint16_t PULSE_MS = 100;

// ======================== Comandos Serial para Relé =========================
// Envia via Serial 9600 baud para o STC15F104W
static uint8_t relayOnCmd[]  = {0xA0, 0x01, 0x01, 0xA2};  // Relé ON
static uint8_t relayOffCmd[] = {0xA0, 0x01, 0x00, 0xA1};  // Relé OFF

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

// ======================== RELAY MODE =========================
enum MachineMode : uint8_t {
  MODE_CONVENTIONAL = 0,
  MODE_INDUSTRIAL   = 1
};

static MachineMode machineMode = MODE_INDUSTRIAL;

// invert levels
static bool relayInvert = false;
static int  relayOnLevel  = HIGH;
static int  relayOffLevel = LOW;

// estado lógico (convencional)
static bool relayLogicalOn = false;

// pulso (industrial)
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

  uint8_t  machineMode;   // 0 conv, 1 ind
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
  bool physical = relayInvert ? !on : on;
  if (physical) {
    Serial.write(relayOnCmd, 4);
  } else {
    Serial.write(relayOffCmd, 4);
  }
}

static void relayOffSafe() {
  relayLogicalOn = false;
  pulseActive = false;
  applyRelayPhysical(false);
}

// ======================== Helpers (Log) =========================
static void log_append_line(const char* line) {
  if (!apEnabled) return;  // Serial é do relé — sem fallback após AP desligar

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
  // Serial.print removido — Serial é exclusiva do relé (STC15F104W)
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
  uint32_t sector = eepromSectorIndexFromFlashSize(ideSz);
  bool ok = ESP.flashEraseSector(sector);
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
  P.machineMode = (uint8_t)MODE_INDUSTRIAL;
  P.relayInvert = 0;
}

static void applyPersistRuntime() {
  machineMode = (P.machineMode == (uint8_t)MODE_CONVENTIONAL) ? MODE_CONVENTIONAL : MODE_INDUSTRIAL;
  relayInvert = (P.relayInvert != 0);
  updateRelayLevels();
  relayOffSafe();
}

static void persistLoad() {
  EEPROM.get(0, P);

  if (DEBUG_EEPROM_DUMP) debugEepromDump(0, 64);

  if (P.magic == EEPROM_MAGIC && P.ver == 3) {
    // Migração v3 -> v4: preserva config, adiciona wsRestartEnabled=0
    P.ver = EEPROM_VER;
    P.wsRestartEnabled = 0;
    memset(P.reserved, 0, sizeof(P.reserved));
    EEPROM.put(0, P);
    EEPROM.commit();
  } else if (P.magic != EEPROM_MAGIC || P.ver != EEPROM_VER) {
    persistDefaults();
    EEPROM.put(0, P);
    EEPROM.commit();
  }

  applyPersistRuntime();
}

static bool persistSaveAndVerifyOnce(uint8_t tryN) {
  (void)tryN;
  EEPROM.put(0, P);
  bool ok = EEPROM.commit();

  Persist T;
  EEPROM.get(0, T);

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
  logf("RECOVERY: Reconexao completa WiFi+WS.");
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
  logf("FAILOVER: Alternando para rede %u: SSID=%s", wifiSlot + 1, activeSSID());
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
        logf("MachineMode=%s | RelayInvert=%s | Levels ON=%s OFF=%s",
             (machineMode == MODE_INDUSTRIAL) ? "INDUSTRIAL(PULSE)" : "CONVENTIONAL(ON/OFF)",
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

          char buf[260];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%lu,\"block\":%lu,\"cpu\":%u,\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u}",
            rssi, ch,
            (unsigned long)heap,
            (unsigned long)block,
            (unsigned)cpu,
            (unsigned long)up,
            (unsigned long)P.bootCount,
            (unsigned)wifiSlot
          );
          webSocket.sendTXT(buf);
          if (apEnabled) logf("WS BIN 0x03: %s", buf);
          break;
        }

        if (machineMode == MODE_CONVENTIONAL) {
          if (b == 0x01) {
            relayLogicalOn = true;
            applyRelayPhysical(true);
            if (apEnabled) log_append_line("WS BIN 0x01: RELÉ ON (CONV).");
            webSocket.sendTXT("RelayStatus:ON");
          } else if (b == 0x02) {
            relayLogicalOn = false;
            applyRelayPhysical(false);
            if (apEnabled) log_append_line("WS BIN 0x02: RELÉ OFF (CONV).");
            webSocket.sendTXT("RelayStatus:OFF");
          } else {
            if (apEnabled) logf("WS BIN desconhecido (CONV): %u", (unsigned)b);
          }
        } else {
          // MODE_INDUSTRIAL: pulso só em 0x01
          if (b == 0x01) {
            if (!pulseActive) {
              pulseActive = true;
              pulseEndMs = millis() + PULSE_MS;
              applyRelayPhysical(true);
              if (apEnabled) logf("WS BIN 0x01: Pulso iniciado (%ums) (IND).", PULSE_MS);
            } else {
              if (apEnabled) log_append_line("WS BIN 0x01: pulso já ativo (IND), ignorando.");
            }
          } else if (b == 0x02) {
            if (apEnabled) log_append_line("WS BIN 0x02 ignorado (IND).");
          } else {
            if (apEnabled) logf("WS BIN desconhecido (IND): %u", (unsigned)b);
          }
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
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy - Configuração</title>
<style>
body{background:#eef6f0;font-family:Arial;margin:0;color:#222}
.header{background:#198754;color:#fff;padding:20px;text-align:center;font-weight:700}
.card{background:#fff;max-width:520px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 18px rgba(0,0,0,0.12)}
input,select,button{width:100%;padding:12px;margin-top:10px;border-radius:8px;border:1px solid #cfe9d8;box-sizing:border-box}
button{background:#198754;color:#fff;border:none;font-weight:700}
small{color:#444;display:block;margin-top:10px;line-height:1.35}
a{color:#198754}
.toggleRow{display:flex;gap:10px;margin-top:10px}
.toggleBtn{flex:1;border:1px solid #cfe9d8;border-radius:10px;padding:12px;cursor:pointer;background:#f7fff7}
.toggleBtn.active{outline:2px solid #198754;border-color:#198754;font-weight:700}
.chkRow{display:flex;align-items:center;gap:10px;margin-top:10px}
.chkRow input{width:auto;margin:0}
.section{font-weight:700;color:#198754;margin-top:18px;margin-bottom:2px;border-bottom:1px solid #cfe9d8;padding-bottom:4px}
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

  <div class="section">Rede 2 (secundária / failover)</div>
  <small>Opcional. Se preenchida, o dispositivo alterna para esta rede quando a rede 1 falhar.</small>

  <label>SSID</label>
  <input id="ssid2" placeholder="SSID da rede 2 (opcional)">

  <label>Senha</label>
  <input id="pass2" type="text" placeholder="Senha rede 2 (opcional)">

  <div class="section">Dispositivo</div>

  <label>NodeID</label>
  <input id="nodeid" placeholder="ex: I00047">

  <label>Tipo de máquina</label>
  <div class="toggleRow">
    <div id="btnConv" class="toggleBtn">Convencional<br><small>0x01 liga / 0x02 desliga</small></div>
    <div id="btnInd"  class="toggleBtn active">Industrial<br><small>0x01 = pulso</small></div>
  </div>

  <div class="chkRow">
    <input id="invert" type="checkbox">
    <label for="invert">Inverter lógica do relé (ON=LOW / OFF=HIGH)</label>
  </div>

  <div class="chkRow">
    <input id="wsrestart" type="checkbox">
    <label for="wsrestart">Reiniciar automaticamente se sem conexão por 1 hora</label>
  </div>

  <button id="save" style="margin-top:18px">Salvar e Reiniciar</button>
  <small id="status"></small>

  <small style="margin-top:14px;">
    <a href="/info">Abrir /info</a><br>
    <a href="/diagflash" target="_blank">Rodar diagnóstico flash (/diagflash)</a>
  </small>
</div>
</body>
<script>
function qs(id){return document.getElementById(id);}

function encText(enc){
  if(enc===0) return 'Open';
  if(enc===1) return 'WEP';
  if(enc===2) return 'WPA-PSK';
  if(enc===3) return 'WPA2-PSK';
  if(enc===4) return 'WPA/WPA2-PSK';
  return 'Desconhecido';
}

let machineMode = 1;
function setMode(m){
  machineMode = m;
  qs('btnConv').classList.toggle('active', m===0);
  qs('btnInd').classList.toggle('active', m===1);
}

function scan(retries){
  retries=retries||0;
  fetch('/scan').then(r=>r.json()).then(list=>{
    let s=qs('ssid');
    if(list.length===0&&retries<5){setTimeout(()=>scan(retries+1),3000);return;}
    s.innerHTML='';
    list.forEach(i=>{
      let o=document.createElement('option');
      o.value=i.ssid;
      o.textContent=i.ssid+' | '+i.rssi+' dBm | CH '+i.channel+' | '+encText(i.enc);
      s.appendChild(o);
    });
  }).catch(()=>{if(retries<5)setTimeout(()=>scan(retries+1),3000);});
}

window.onload=()=>{
  fetch('/config-data').then(r=>r.json()).then(d=>{
    qs('manual_ssid').value = d.ssid   || '';
    qs('pass').value        = d.pass   || '';
    qs('ssid2').value       = d.ssid2  || '';
    qs('pass2').value       = d.pass2  || '';
    qs('nodeid').value      = d.nodeid || '';
    setMode(d.mode || 1);
    qs('invert').checked    = (d.invert    === 1);
    qs('wsrestart').checked = (d.wsrestart === 1);
  }).catch(()=>{});

  scan();

  qs('btnConv').onclick=()=>setMode(0);
  qs('btnInd').onclick =()=>setMode(1);

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
        '&mode='+encodeURIComponent(String(machineMode))+
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
</html>
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
    "\"nodeid\":\"%s\",\"mode\":%u,\"invert\":%u,\"wsrestart\":%u}",
    ssEsc, pwEsc, ss2Esc, pw2Esc, nidEsc,
    (unsigned)P.machineMode, (unsigned)P.relayInvert, (unsigned)P.wsRestartEnabled
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

  int8_t n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING) {
    // Scan ainda em andamento — retorna vazio, frontend tenta de novo
    server.send(200, "application/json", "[]");
    return;
  }

  if (n == WIFI_SCAN_FAILED || n < 0) {
    // Inicia scan assíncrono (não bloqueia o loop)
    WiFi.scanNetworks(true);
    log_append_line("Scan WiFi iniciado (async).");
    server.send(200, "application/json", "[]");
    return;
  }

  // Scan concluído — monta JSON com os resultados
  const int MAXN = 25;
  int outN = (n > MAXN) ? MAXN : n;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  server.sendContent("[");
  for (int i = 0; i < outN; i++) {
    if (i > 0) server.sendContent(",");

    char ssidRaw[33] = {0};
    WiFi.SSID(i).toCharArray(ssidRaw, sizeof(ssidRaw));
    char ssEsc[160];
    json_escape_into(ssidRaw, ssEsc, sizeof(ssEsc));

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

  const char* modeStr = (machineMode == MODE_INDUSTRIAL) ? "Industrial (Pulso)" : "Convencional (ON/OFF)";

  const char* srcLog = (apEnabled ? logBuffer : "");
  size_t logLen = strnlen(srcLog, LOG_MAX_LEN);
  const char* logStart = srcLog;
  const size_t SHOW_MAX = 1400;
  if (logLen > SHOW_MAX) logStart = srcLog + (logLen - SHOW_MAX);

  // Buffer local evita String temporária de WiFi.SSID() na heap
  char currentSSID[33] = {0};
  if (staOk) WiFi.SSID().toCharArray(currentSSID, sizeof(currentSSID));

  static char logEsc[2200];
  static char page[6800];
  html_escape_into(logStart, logEsc, sizeof(logEsc));

  // Status do relé (via Serial)
  bool physOn = relayInvert ? !relayLogicalOn : relayLogicalOn;
  const char* relayPhys = physOn ? "ON" : "OFF";
  const char* relayLogic = relayLogicalOn ? "ON" : "OFF";

  int n = snprintf(
    page, sizeof(page),
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='2'>"
    "<title>Foreasy - Info</title>"
    "<style>"
    "body{background:#eef6f0;font-family:Arial;margin:0;color:#222}"
    ".header{background:#198754;color:white;padding:18px;text-align:center;font-weight:700}"
    ".card{background:#fff;max-width:860px;margin:18px auto;padding:18px;border-radius:12px;box-shadow:0 6px 20px rgba(0,0,0,0.12)}"
    ".row{display:flex;gap:12px;flex-wrap:wrap}"
    ".item{flex:1;min-width:240px;padding:12px;border-radius:10px;background:#f7fff7;border:1px solid #e6f4ea}"
    "h4{margin:6px 0;color:#198754}"
    ".mono{font-family:monospace;font-size:12px;white-space:pre-wrap;background:#111827;color:#e5e7eb;padding:10px;border-radius:10px;max-height:320px;overflow:auto}"
    "a{color:#198754}"
    ".badge{display:inline-block;padding:3px 8px;border-radius:999px;font-size:12px;background:#e8f5ee;border:1px solid #cfe9d8}"
    "</style></head><body>"
    "<div class='header'>Foreasy - /info (auto refresh 2s)</div>"
    "<div class='card'>"

    "<div class='row'>"
      "<div class='item'><h4>NodeID</h4><div>%s</div></div>"
      "<div class='item'><h4>Wi-Fi salvo</h4><div>%s</div></div>"
      "<div class='item'><h4>Wi-Fi atual (STA)</h4><div>%s</div></div>"
    "</div>"

    "<div style='height:12px'></div>"
    "<div class='row'>"
      "<div class='item'><h4>RSSI</h4><div>%s</div></div>"
      "<div class='item'><h4>IP (STA)</h4><div>%s</div></div>"
      "<div class='item'><h4>IP (AP)</h4><div>%s</div></div>"
    "</div>"

    "<div style='height:12px'></div>"
    "<div class='row'>"
      "<div class='item'><h4>WebSocket</h4><div>%s</div></div>"
      "<div class='item'><h4>Tipo de máquina</h4><div>%s</div></div>"
      "<div class='item'><h4>Relé invert?</h4><div>%s</div></div>"
    "</div>"

    "<div style='height:12px'></div>"
    "<div class='row'>"
      "<div class='item'><h4>Relé físico</h4><div>%s</div></div>"
      "<div class='item'><h4>Relé lógico (conv)</h4><div>%s</div></div>"
      "<div class='item'><h4>Pulso (ind)</h4><div>%s</div></div>"
    "</div>"

    "<div style='height:12px'></div>"
    "<div class='row'>"
      "<div class='item'><h4>AP ativo?</h4><div>%s</div></div>"
      "<div class='item'><h4>Boots (RAM)</h4><div><span class='badge'>%s</span></div></div>"
      "<div class='item'><h4>Último reset</h4><div>%s (%u)</div></div>"
    "</div>",
    (P.nodeId[0] ? P.nodeId : "—"),
    wifiSavedBuf,
    (staOk ? currentSSID : "—"),
    rssiBuf,
    ipsta,
    ipap,
    (isWebSocketConnected ? "Conectado" : "Desconectado"),
    modeStr,
    (relayInvert ? "SIM" : "NAO"),
    relayPhys,
    relayLogic,
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
    server.sendContent("<div style='height:14px'></div><h4>Logs (ultimos ~1400 chars)</h4><div class='mono'>");
    server.sendContent(logEsc);
    server.sendContent("</div>");
    server.sendContent("<div style='margin-top:14px'><a href='/config'>Voltar /config</a></div></div></body></html>");
  } else {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent(page);
    server.sendContent("<div style='height:14px'></div><div style='color:#555'>Logs desativados após o AP desligar (lean mode).</div>");
    server.sendContent("<div style='margin-top:14px'><a href='/config'>Voltar /config</a></div></div></body></html>");
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

  uint8_t mode = (server.hasArg("mode") ? (uint8_t)server.arg("mode").toInt() : 1);
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

  P.machineMode      = (mode == 0) ? 0 : 1;
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

  if (DEBUG_EEPROM_SECTORINFO) debugEepromSectorInfo();

  if (ERASE_SECTOR_BEFORE_SAVE) {
    diagEraseEepromSector();
    delay(50);
    yield();
  }

  persistSaveAndVerifyRetry(COMMIT_TRIES);

  EEPROM.end();
  delay(250);
  yield();

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
  // industrial pulso
  if (machineMode == MODE_INDUSTRIAL) {
    if (pulseActive && (int32_t)(millis() - pulseEndMs) >= 0) {
      pulseActive = false;
      applyRelayPhysical(false);
      if (apEnabled) log_append_line("Pulso finalizado (relay OFF).");
      if (isWebSocketConnected) webSocket.sendTXT("RelayStatus:ON");
    }
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
        logf("WATCHDOG: WS down > 5min. Failover.");
        failoverReconnect();
      }
    } else {
      wsDownSinceMs = 0;
    }

    // Sem WiFi e sem WS por mais de 8 min => reconexão completa (sem restart)
    if (!wifiOk && !wsOk) {
      if ((millis() - lastConnectivityOkMs) > GLOBAL_DOWN_RESET_MS) {
        logf("GLOBAL WD: sem WiFi e WS por muito tempo. Failover.");
        lastConnectivityOkMs = millis();
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

// ======================== SETUP / LOOP =========================
void setup() {
  Serial.begin(9600);  // IMPORTANTE: 9600 baud para comunicar com STC15F104W
  delay(60);

  debugFlashInfoOnce();

  pinMode(ledPin, OUTPUT);
  // Relé controlado via Serial, não precisa pinMode

  setLed(false);
  lastLedState = false;

  logBuffer[0] = '\0';

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
  logf("MachineMode=%s | RelayInvert=%s",
       (machineMode == MODE_INDUSTRIAL) ? "INDUSTRIAL(PULSE)" : "CONVENTIONAL(ON/OFF)",
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
}
