// ============================================================================
// Foreasy ESP32-S3 — MIDEA IoT (lavadora/secadora convencional residencial)
// Cada ESP é vinculado a UMA máquina MIDEA Smart (device_id + token + key).
//
// O ESP conecta no WiFi da lavanderia, fala com a máquina MIDEA na rede local
// (TCP 6444, protocolo V3 criptografado) e expõe o status do ciclo.
//
// PÁGINAS WEB: /config (wizard) e /info (status ao vivo). "/" → /config.
//
// FLUXO MIDEA V3 (ver Midea-WiFi/VETORES-TESTE.md):
//   TCP connect → handshake (token→tcp_key) → query cifrada → parse status
//
// O QUE É SALVO NA NVS (namespace "midea"):
//   ssid, pass, ssid2, pass2  — WiFi (dual, failover)
//   nodeid                    — identificação Foreasy do ESP
//   mdId                      — device_id da MIDEA (uint64, como string decimal)
//   mdIp                      — IP da máquina na rede (fallback; descoberta atualiza)
//   mdToken                   — token V3 (128 hex chars / 64 bytes)
//   mdKey                     — key V3 (64 hex chars / 32 bytes)
//   mdType                    — tipo do device (0xDB etc.) — informativo
//   bootCount
//
// REDE AP (config inicial):
//   SSID: <nodeId>-AP   |   Senha: 12345678   |   IP: 192.168.4.1
//   Ativo 10 min após boot; depois desliga (lean mode).
//
// Baseado na estrutura do esp32c3.ino (NVS, AP, wizard, /info, dual WiFi).
// ============================================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md5.h>

#define FW_VERSION "0.3.0"
#define FW_CHIP    "esp32s3"

// ---------------- LED ----------------
int ledPin = 2;   // LED de status (ajuste ao seu shield)

// ---------------- Constantes MIDEA (LocalSecurity) ----------------
static const uint8_t MIDEA_AES_KEY[16] = {
  0x6a,0x92,0xef,0x40,0x6b,0xad,0x2f,0x03,0x59,0xba,0xad,0x99,0x41,0x71,0xea,0x6d
};
// SALT = "xhdiwjnchekd4d512chdjx5d8e4c394D2D7S" (36 bytes)
static const char MIDEA_SALT[] = "xhdiwjnchekd4d512chdjx5d8e4c394D2D7S";
static const int  MIDEA_SALT_LEN = 36;

#define MIDEA_PORT        6444
#define MIDEA_DISCOVER_PORT 6445
#define MIDEA_POLL_MS     10000UL
#define MIDEA_RECON_MS    20000UL
#define MIDEA_TCP_TO_MS   4000
#define MIDEA_DISCOVER_TO_MS 3000

// Pacote de broadcast da descoberta MIDEA (72 bytes fixos — igual à lib midea-local)
static const uint8_t MIDEA_BROADCAST[72] = {
  0x5A,0x5A,0x01,0x11,0x48,0x00,0x92,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7F,0x75,0xBD,0x6B,0x3E,0x4F,0x8B,0x76,
  0x2E,0x84,0x9C,0x6E,0x57,0x8D,0x65,0x90,0x03,0x6E,0x9D,0x43,0x42,0xA5,0x0F,0x1F,
  0x56,0x9E,0xB8,0xEC,0x91,0x8E,0x92,0xE5
};

// ---------------- Identidade / config ----------------
String nodeId   = "MIDEA-001";
String mideaIdS = "";      // device_id decimal
uint64_t mideaId = 0;
String mideaIp  = "";
String mideaTokenHex = ""; // 128 hex
String mideaKeyHex   = ""; // 64 hex
int    mideaType = 0xDB;

uint8_t mdToken[64];  int mdTokenLen = 0;
uint8_t mdKey[32];    int mdKeyLen   = 0;

// ---------------- Estado MIDEA ----------------
WiFiClient mideaClient;
bool     mdConnected = false;     // TCP + handshake OK
bool     mdAuthed    = false;
uint8_t  tcpKey[32];              // chave de sessão derivada no handshake
uint16_t reqCount    = 0;
uint32_t lastPollMs  = 0;
uint32_t lastReconMs = 0;

// Status lido
bool mdPower = false;
int  mdStatus = 0, mdMode = 0, mdProgram = 0, mdProgress = 0, mdTimeLeft = 0;
bool mdValid = false;             // último parse foi válido
String mdLastErr = "—";
String mdRawHex  = "";

// ---------------- WiFi ----------------
String sSsid, sPass, sSsid2, sPass2;
uint8_t wifiSlot = 0;
bool     wifiConnecting = false;
uint32_t wifiConnectStartMs = 0;
uint32_t lastWiFiAttemptMs = 0;
const uint32_t WIFI_MAX_WAIT_MS = 40000;
const uint32_t WIFI_RETRY_MS    = 5000;

const char* activeSSID() { return wifiSlot == 0 ? sSsid.c_str() : sSsid2.c_str(); }
const char* activePass() { return wifiSlot == 0 ? sPass.c_str() : sPass2.c_str(); }
bool hasSavedWiFi()      { return strlen(activeSSID()) > 0; }

// ---------------- AP ----------------
IPAddress apIP(192,168,4,1);
const uint32_t AP_LIFETIME_MS = 10UL*60UL*1000UL;
bool     apEnabled = true;
uint32_t bootTimeMs = 0;

uint32_t bootCount = 0;

WebServer server(80);
Preferences prefs;

// ============================================================================
// CRIPTO — helpers (mbedtls)
// ============================================================================

void sha256_hash(const uint8_t* in, size_t len, uint8_t out[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, in, len);
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
}

void md5_hash(const uint8_t* in, size_t len, uint8_t out[16]) {
  mbedtls_md5_context c;
  mbedtls_md5_init(&c);
  mbedtls_md5_starts(&c);
  mbedtls_md5_update(&c, in, len);
  mbedtls_md5_finish(&c, out);
  mbedtls_md5_free(&c);
}

// AES-128-ECB com a chave fixa. PKCS7. Retorna tamanho cifrado.
int aesEcbEncryptFixed(const uint8_t* in, int len, uint8_t* out) {
  int pad = 16 - (len % 16); if (pad == 0) pad = 16;
  int total = len + pad;
  uint8_t buf[256];
  if (total > (int)sizeof(buf)) return -1;
  memcpy(buf, in, len);
  for (int i = 0; i < pad; i++) buf[len + i] = (uint8_t)pad;
  mbedtls_aes_context a; mbedtls_aes_init(&a);
  mbedtls_aes_setkey_enc(&a, MIDEA_AES_KEY, 128);
  for (int i = 0; i < total; i += 16)
    mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, buf + i, out + i);
  mbedtls_aes_free(&a);
  return total;
}

// AES-128-ECB decrypt com chave fixa + remove PKCS7. Retorna tamanho ou -1.
int aesEcbDecryptFixed(const uint8_t* in, int len, uint8_t* out) {
  if (len <= 0 || len % 16 != 0) return -1;
  mbedtls_aes_context a; mbedtls_aes_init(&a);
  mbedtls_aes_setkey_dec(&a, MIDEA_AES_KEY, 128);
  for (int i = 0; i < len; i += 16)
    mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_DECRYPT, in + i, out + i);
  mbedtls_aes_free(&a);
  int pad = out[len - 1];
  if (pad < 1 || pad > 16) return len;   // sem unpad confiável → devolve cheio
  return len - pad;
}

// AES-256-CBC encrypt (key 32B, IV zero). len múltiplo de 16.
void aesCbcEncrypt(const uint8_t* in, int len, const uint8_t key[32], uint8_t* out) {
  uint8_t iv[16]; memset(iv, 0, 16);
  mbedtls_aes_context a; mbedtls_aes_init(&a);
  mbedtls_aes_setkey_enc(&a, key, 256);
  mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
  mbedtls_aes_free(&a);
}

// AES-256-CBC decrypt (key 32B, IV zero). len múltiplo de 16.
void aesCbcDecrypt(const uint8_t* in, int len, const uint8_t key[32], uint8_t* out) {
  uint8_t iv[16]; memset(iv, 0, 16);
  mbedtls_aes_context a; mbedtls_aes_init(&a);
  mbedtls_aes_setkey_dec(&a, key, 256);
  mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, len, iv, in, out);
  mbedtls_aes_free(&a);
}

int hexToBytes(const String& hex, uint8_t* out, int maxLen) {
  int n = hex.length() / 2;
  if (n > maxLen) n = maxLen;
  for (int i = 0; i < n; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    out[i] = (uint8_t) strtol(b, nullptr, 16);
  }
  return n;
}

String bytesToHex(const uint8_t* in, int len) {
  String s; s.reserve(len * 2);
  const char* h = "0123456789abcdef";
  for (int i = 0; i < len; i++) { s += h[in[i] >> 4]; s += h[in[i] & 0xF]; }
  return s;
}

// ============================================================================
// MIDEA — montagem de pacotes
// ============================================================================

uint8_t midChecksum(const uint8_t* d, int from, int to) {
  uint32_t s = 0;
  for (int i = from; i < to; i++) s += d[i];
  return (uint8_t)((~s + 1) & 0xFF);
}

// Monta mensagem de aplicação 0xAA. Retorna tamanho.
int buildAppMessage(uint8_t msgType, uint8_t bodyType, const uint8_t* extra, int extraLen, uint8_t* out) {
  int bodyLen = 1 + extraLen;                 // body_type + extra
  int len = 10 + bodyLen;                     // header(10) + body
  out[0] = 0xAA;
  out[1] = (uint8_t)len;
  out[2] = (uint8_t)mideaType;                // 0xDB
  out[3] = 0; out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
  out[8] = 0x03;                              // protocol version
  out[9] = msgType;                           // set/query
  out[10] = bodyType;
  for (int i = 0; i < extraLen; i++) out[11 + i] = extra[i];
  int total = 10 + bodyLen;
  out[total] = midChecksum(out, 1, total);    // checksum sobre [1..total-1]
  return total + 1;
}

int buildQuery(uint8_t* out) {
  return buildAppMessage(0x03, 0x03, nullptr, 0, out);     // msgtype=query, body_type=0x03
}

int buildPower(bool on, uint8_t* out) {
  uint8_t extra[21];
  extra[0] = on ? 0x01 : 0x00;
  for (int i = 1; i < 21; i++) extra[i] = 0xFF;
  return buildAppMessage(0x02, 0x02, extra, 21, out);      // msgtype=set, body_type=0x02
}

// Monta pacote 5A5A a partir da mensagem de aplicação. Retorna tamanho.
int buildPacket5A5A(const uint8_t* cmd, int cmdLen, uint8_t* out) {
  uint8_t hdr[40];
  memset(hdr, 0, 40);
  hdr[0] = 0x5A; hdr[1] = 0x5A; hdr[2] = 0x01; hdr[3] = 0x11;
  hdr[6] = 0x20;
  // time[12:20] deixamos zero (não é validado pela máquina)
  // device_id little-endian 8 bytes em [20:28]
  uint64_t id = mideaId;
  for (int i = 0; i < 8; i++) { hdr[20 + i] = (uint8_t)(id & 0xFF); id >>= 8; }

  memcpy(out, hdr, 40);
  int enc = aesEcbEncryptFixed(cmd, cmdLen, out + 40);
  if (enc < 0) return -1;
  int packetLen = 40 + enc;
  // packet[4:6] = (packetLen + 16) LE
  uint16_t total = (uint16_t)(packetLen + 16);
  out[4] = total & 0xFF; out[5] = (total >> 8) & 0xFF;
  // checksum MD5(packet + salt)
  uint8_t md[16];
  uint8_t tmp[300];
  memcpy(tmp, out, packetLen);
  memcpy(tmp + packetLen, MIDEA_SALT, MIDEA_SALT_LEN);
  md5_hash(tmp, packetLen + MIDEA_SALT_LEN, md);
  memcpy(out + packetLen, md, 16);
  return packetLen + 16;
}

// Encapsula no frame 8370. msgType: 0=handshake, 6=encrypted. Retorna tamanho.
int encode8370(const uint8_t* data, int dataLen, uint8_t msgType, uint8_t* out) {
  uint8_t body[400];
  int bodyLen = dataLen;
  int padding = 0;
  bool enc = (msgType == 0x06 || msgType == 0x03);

  memcpy(body, data, dataLen);

  int size = dataLen;
  if (enc && ((size + 2) % 16 != 0)) {
    padding = 16 - ((size + 2) & 0xF);
    size += padding + 32;
    for (int i = 0; i < padding; i++) body[bodyLen + i] = (uint8_t)random(0, 256);
    bodyLen += padding;
  }

  // header
  out[0] = 0x83; out[1] = 0x70;
  out[2] = (size >> 8) & 0xFF; out[3] = size & 0xFF;
  out[4] = 0x20; out[5] = (uint8_t)((padding << 4) | msgType);

  // data = request_count(2 BE) + body
  uint8_t payload[420];
  payload[0] = (reqCount >> 8) & 0xFF; payload[1] = reqCount & 0xFF;
  memcpy(payload + 2, body, bodyLen);
  int payloadLen = 2 + bodyLen;
  reqCount++;

  if (enc) {
    // sign = sha256(header(6) + payload)
    uint8_t signbuf[6 + 420];
    memcpy(signbuf, out, 6);
    memcpy(signbuf + 6, payload, payloadLen);
    uint8_t sign[32];
    sha256_hash(signbuf, 6 + payloadLen, sign);
    // encrypt payload (múltiplo de 16) com tcp_key
    uint8_t encbuf[420];
    aesCbcEncrypt(payload, payloadLen, tcpKey, encbuf);
    memcpy(out + 6, encbuf, payloadLen);
    memcpy(out + 6 + payloadLen, sign, 32);
    return 6 + payloadLen + 32;
  } else {
    memcpy(out + 6, payload, payloadLen);
    return 6 + payloadLen;
  }
}

// Decodifica um frame 8370 → pacote 5A5A em out. Retorna tamanho ou -1.
int decode8370(const uint8_t* frame, int frameLen, uint8_t* out) {
  if (frameLen < 6) return -1;
  if (frame[0] != 0x83 || frame[1] != 0x70) return -1;
  int size = ((frame[2] << 8) | frame[3]) + 8;
  if (frameLen < size) return -1;
  int padding = frame[5] >> 4;
  int msgType = frame[5] & 0xF;
  const uint8_t* body = frame + 6;
  int bodyLen = size - 6;

  uint8_t dec[420];
  int dataLen;
  if (msgType == 0x06 || msgType == 0x03) {
    int encLen = bodyLen - 32;
    if (encLen <= 0 || encLen % 16 != 0) return -1;
    aesCbcDecrypt(body, encLen, tcpKey, dec);
    // (verificação de sign omitida — confiamos no tcp_key)
    dataLen = encLen;
    if (padding) dataLen -= padding;
  } else {
    memcpy(dec, body, bodyLen);
    dataLen = bodyLen;
  }
  // pula response_count (2 bytes)
  if (dataLen < 2) return -1;
  int outLen = dataLen - 2;
  memcpy(out, dec + 2, outLen);
  return outLen;
}

// Faz o parse de um pacote 5A5A (status). Atualiza variáveis globais.
bool parsePacket5A5A(const uint8_t* pkt, int len) {
  if (len <= 56) return false;
  int payloadType = pkt[2] | (pkt[3] << 8);
  if (payloadType == 0x1001 || payloadType == 0x0001) return false;  // controle
  int cLen = len - 56;                    // tira header 40 + checksum 16
  if (cLen % 16 != 0) return false;
  uint8_t aa[300];
  int aaLen = aesEcbDecryptFixed(pkt + 40, cLen, aa);
  if (aaLen < 29) return false;
  if (aa[0] != 0xAA) return false;

  mdRawHex = bytesToHex(aa, aaLen > 40 ? 40 : aaLen);

  mdPower   = aa[11] > 0;
  mdStatus  = aa[12];
  mdMode    = aa[13];
  mdProgram = aa[14];
  // progress: bitmask em aa[26]
  mdProgress = 0;
  for (int i = 0; i < 7; i++) { if (aa[26] & (1 << i)) { mdProgress = i + 1; break; } }
  mdTimeLeft = mdPower ? (aa[27] + (aa[28] << 8)) : 0;
  return true;
}

// ============================================================================
// MIDEA — conexão e handshake
// ============================================================================

// Lê da máquina até idle ou timeout. Retorna bytes lidos.
int mideaRead(uint8_t* buf, int maxLen, int totalToMs) {
  int n = 0;
  uint32_t start = millis();
  uint32_t lastByte = millis();
  while (millis() - start < (uint32_t)totalToMs && n < maxLen) {
    if (mideaClient.available()) {
      buf[n++] = (uint8_t)mideaClient.read();
      lastByte = millis();
    } else {
      if (n > 0 && millis() - lastByte > 250) break;   // idle → fim do frame
      delay(2);
    }
  }
  return n;
}

bool mideaHandshake() {
  uint8_t frame[120];
  int fl = encode8370(mdToken, mdTokenLen, 0x00, frame);   // handshake request
  if (mideaClient.write(frame, fl) != (size_t)fl) { mdLastErr = "write handshake"; return false; }

  uint8_t resp[128];
  int n = mideaRead(resp, sizeof(resp), MIDEA_TCP_TO_MS);
  if (n < 72) { mdLastErr = "handshake resp curto (" + String(n) + ")"; return false; }

  const uint8_t* r64 = resp + 8;            // response[8:72]
  uint8_t plain[32];
  aesCbcDecrypt(r64, 32, mdKey, plain);     // AES-256-CBC decrypt do payload com a key
  uint8_t sign[32];
  sha256_hash(plain, 32, sign);
  if (memcmp(sign, r64 + 32, 32) != 0) { mdLastErr = "handshake sign mismatch"; return false; }
  for (int i = 0; i < 32; i++) tcpKey[i] = plain[i] ^ mdKey[i];
  reqCount = 0;
  mdLastErr = "—";
  return true;
}

// Descobre o IP da máquina pelo device_id via broadcast UDP.
// O device_id vem nos bytes [base+20 .. base+26] (LE) e o IP é a origem do pacote.
// Atualiza mideaIp e retorna true se achou a nossa máquina.
bool mideaDiscover() {
  if (mideaId == 0 || !WiFi.isConnected()) return false;

  WiFiUDP udp;
  if (!udp.begin(0)) return false;   // porta local efêmera

  IPAddress bcast = WiFi.broadcastIP();
  udp.beginPacket(bcast, MIDEA_DISCOVER_PORT);
  udp.write(MIDEA_BROADCAST, sizeof(MIDEA_BROADCAST));
  udp.endPacket();
  // fallback: broadcast global
  udp.beginPacket(IPAddress(255,255,255,255), MIDEA_DISCOVER_PORT);
  udp.write(MIDEA_BROADCAST, sizeof(MIDEA_BROADCAST));
  udp.endPacket();

  uint8_t buf[256];
  uint32_t start = millis();
  bool found = false;
  while (millis() - start < MIDEA_DISCOVER_TO_MS) {
    int sz = udp.parsePacket();
    if (sz <= 0) { delay(10); continue; }
    IPAddress from = udp.remoteIP();
    int n = udp.read(buf, sizeof(buf));
    if (n < 34) continue;

    // localiza o offset do 5A5A (0 direto, ou 8 quando vem em frame 8370)
    int base = -1;
    if (buf[0] == 0x5A && buf[1] == 0x5A) base = 0;
    else if (buf[0] == 0x83 && buf[1] == 0x70 && buf[8] == 0x5A && buf[9] == 0x5A) base = 8;
    if (base < 0 || n < base + 26) continue;

    // device_id = 6 bytes little-endian em [base+20 .. base+26]
    uint64_t id = 0;
    for (int i = 5; i >= 0; i--) id = (id << 8) | buf[base + 20 + i];

    if (id == mideaId) {
      mideaIp = from.toString();
      found = true;
      break;
    }
  }
  udp.stop();
  if (!found) mdLastErr = "device_id nao encontrado na rede";
  return found;
}

bool mideaConnect() {
  if (mideaId == 0 || mdTokenLen != 64 || mdKeyLen != 32) {
    mdLastErr = "config incompleta (id/token/key)";
    return false;
  }
  // Sem IP salvo? Descobre pelo device_id (broadcast).
  if (mideaIp.isEmpty()) {
    if (!mideaDiscover()) return false;
  }
  mideaClient.stop();
  mideaClient.setTimeout(MIDEA_TCP_TO_MS);
  if (!mideaClient.connect(mideaIp.c_str(), MIDEA_PORT)) {
    // IP pode ter mudado (DHCP) — re-descobre e tenta uma vez mais
    if (mideaDiscover() && mideaClient.connect(mideaIp.c_str(), MIDEA_PORT)) {
      // ok, seguiu com o IP novo
    } else {
      mdLastErr = "TCP falhou (" + mideaIp + ")";
      return false;
    }
  }
  if (!mideaHandshake()) { mideaClient.stop(); return false; }
  mdConnected = true;
  mdAuthed = true;
  return true;
}

bool mideaQuery() {
  if (!mideaClient.connected()) { mdConnected = false; return false; }

  uint8_t cmd[40];
  int cmdLen = buildQuery(cmd);
  uint8_t pkt[300];
  int pktLen = buildPacket5A5A(cmd, cmdLen, pkt);
  if (pktLen < 0) { mdLastErr = "build5A5A"; return false; }
  uint8_t frame[400];
  int fl = encode8370(pkt, pktLen, 0x06, frame);
  if (mideaClient.write(frame, fl) != (size_t)fl) { mdLastErr = "write query"; return false; }

  uint8_t resp[512];
  int n = mideaRead(resp, sizeof(resp), MIDEA_TCP_TO_MS);
  if (n < 6) { mdLastErr = "sem resposta query"; return false; }

  // pode vir mais de um frame; processa em sequência
  int off = 0;
  bool got = false;
  while (off + 6 <= n) {
    int fsize = ((resp[off+2] << 8) | resp[off+3]) + 8;
    if (fsize <= 0 || off + fsize > n) break;
    uint8_t pkt5a[420];
    int pl = decode8370(resp + off, fsize, pkt5a);
    if (pl > 0 && parsePacket5A5A(pkt5a, pl)) got = true;
    off += fsize;
  }
  if (got) { mdValid = true; mdLastErr = "—"; }
  return got;
}

bool mideaSendPower(bool on) {
  if (!mdConnected && !mideaConnect()) return false;
  uint8_t cmd[40];
  int cmdLen = buildPower(on, cmd);
  uint8_t pkt[300];
  int pktLen = buildPacket5A5A(cmd, cmdLen, pkt);
  if (pktLen < 0) return false;
  uint8_t frame[400];
  int fl = encode8370(pkt, pktLen, 0x06, frame);
  if (mideaClient.write(frame, fl) != (size_t)fl) return false;
  delay(500);
  mideaQuery();
  return true;
}

void mideaTick() {
  if (!WiFi.isConnected()) return;

  if (mdConnected && !mideaClient.connected()) {
    mdConnected = false; mdAuthed = false;
  }

  if (!mdConnected) {
    if (millis() - lastReconMs < MIDEA_RECON_MS) return;
    lastReconMs = millis();
    mideaConnect();
    return;
  }

  if (millis() - lastPollMs >= MIDEA_POLL_MS) {
    lastPollMs = millis();
    if (!mideaQuery()) {
      mdConnected = false; mdAuthed = false;
      mideaClient.stop();
    } else {
      digitalWrite(ledPin, mdPower ? HIGH : LOW);
    }
  }
}

// ============================================================================
// NVS
// ============================================================================

void loadPrefs() {
  nodeId        = prefs.getString("nodeid", "MIDEA-001");
  sSsid         = prefs.getString("ssid",   "");
  sPass         = prefs.getString("pass",   "");
  sSsid2        = prefs.getString("ssid2",  "");
  sPass2        = prefs.getString("pass2",  "");
  mideaIdS      = prefs.getString("mdId",   "");
  mideaIp       = prefs.getString("mdIp",   "");
  mideaTokenHex = prefs.getString("mdToken","");
  mideaKeyHex   = prefs.getString("mdKey",  "");
  mideaType     = prefs.getInt("mdType", 0xDB);
  bootCount     = prefs.getUInt("bootCount", 0);

  mideaId    = strtoull(mideaIdS.c_str(), nullptr, 10);
  mdTokenLen = hexToBytes(mideaTokenHex, mdToken, 64);
  mdKeyLen   = hexToBytes(mideaKeyHex,   mdKey,   32);
}

// ============================================================================
// WiFi
// ============================================================================

void setupAPSTA() {
  WiFi.mode(WIFI_AP_STA);
  String apName = nodeId + "-AP";
  WiFi.softAP(apName.c_str(), "12345678", 1, false);
  delay(200);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
}

void connectToWiFi_begin() {
  if (!hasSavedWiFi()) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(activeSSID(), activePass());
  wifiConnecting = true;
  wifiConnectStartMs = millis();
}

void wifiTick() {
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
    } else if (millis() - wifiConnectStartMs > WIFI_MAX_WAIT_MS) {
      wifiConnecting = false;
      // alterna slot se houver rede 2
      if (sSsid2.length() > 0) wifiSlot = (wifiSlot == 0) ? 1 : 0;
      WiFi.disconnect(false);
    }
  }
  if (WiFi.status() != WL_CONNECTED && !wifiConnecting && hasSavedWiFi()) {
    if (millis() - lastWiFiAttemptMs >= WIFI_RETRY_MS) {
      lastWiFiAttemptMs = millis();
      connectToWiFi_begin();
    }
  }
}

void apLifetimeTick() {
  if (apEnabled && millis() - bootTimeMs >= AP_LIFETIME_MS) {
    WiFi.softAPdisconnect(true);
    apEnabled = false;
  }
}

// ============================================================================
// HTTP
// ============================================================================

void handleRoot() {
  server.sendHeader("Location", "/config", true);
  server.send(302, "text/plain", "");
}

void handleConfigPage();
void handleConfigData();
void handleInfoPage();
void handleStatusJson();
void handleScan();
void handleSave();
void handleRestart();
void handleResetWiFi();
void handlePower();

void startWebServer() {
  server.on("/",            handleRoot);
  server.on("/config",      handleConfigPage);
  server.on("/config-data", HTTP_GET,  handleConfigData);
  server.on("/info",        handleInfoPage);
  server.on("/status",      HTTP_GET,  handleStatusJson);
  server.on("/scan",        HTTP_GET,  handleScan);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/power",       HTTP_GET,  handlePower);
  server.on("/restart",     HTTP_GET,  handleRestart);
  server.on("/resetwifi",   HTTP_GET,  handleResetWiFi);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void handleConfigData() {
  String j = "{";
  j += "\"ssid\":\""   + sSsid  + "\",";
  j += "\"pass\":\""   + sPass  + "\",";
  j += "\"ssid2\":\""  + sSsid2 + "\",";
  j += "\"pass2\":\""  + sPass2 + "\",";
  j += "\"nodeid\":\"" + nodeId + "\",";
  j += "\"mdId\":\""   + mideaIdS + "\",";
  j += "\"mdIp\":\""   + mideaIp + "\",";
  j += "\"mdToken\":\""+ mideaTokenHex + "\",";
  j += "\"mdKey\":\""  + mideaKeyHex + "\",";
  j += "\"mdType\":"   + String(mideaType);
  j += "}";
  server.send(200, "application/json", j);
}

void handleSave() {
  if (!server.hasArg("ssid") || !server.hasArg("nodeid")) {
    server.send(400, "text/plain", "ssid e nodeid obrigatórios"); return;
  }
  sSsid  = server.arg("ssid");
  sPass  = server.arg("pass");
  sSsid2 = server.arg("ssid2");
  sPass2 = server.arg("pass2");
  nodeId = server.arg("nodeid");
  if (server.hasArg("mdId"))    mideaIdS      = server.arg("mdId");
  if (server.hasArg("mdIp"))    mideaIp       = server.arg("mdIp");
  if (server.hasArg("mdToken")) mideaTokenHex = server.arg("mdToken");
  if (server.hasArg("mdKey"))   mideaKeyHex   = server.arg("mdKey");
  if (server.hasArg("mdType"))  mideaType     = server.arg("mdType").toInt();
  wifiSlot = 0;

  prefs.putString("nodeid",  nodeId);
  prefs.putString("ssid",    sSsid);
  prefs.putString("pass",    sPass);
  prefs.putString("ssid2",   sSsid2);
  prefs.putString("pass2",   sPass2);
  prefs.putString("mdId",    mideaIdS);
  prefs.putString("mdIp",    mideaIp);
  prefs.putString("mdToken", mideaTokenHex);
  prefs.putString("mdKey",   mideaKeyHex);
  prefs.putInt("mdType",     mideaType);
  prefs.putUInt("bootCount", bootCount);

  server.send(200, "text/plain", "Configurado. Reiniciando...");
  delay(400);
  ESP.restart();
}

void handleStatusJson() {
  bool staOk = (WiFi.status() == WL_CONNECTED);
  const char* phases[] = {"Standby","Pre-lavagem","Lavagem","Enxague","Centrifugacao","Secagem","Vapor","Fim"};
  int ph = (mdProgress >= 0 && mdProgress <= 7) ? mdProgress : 0;
  String j = "{";
  j += "\"nodeId\":\""    + nodeId + "\",";
  j += "\"wifi\":"        + String(staOk ? 1 : 0) + ",";
  j += "\"ssid\":\""      + (staOk ? WiFi.SSID() : String("")) + "\",";
  j += "\"rssi\":"        + String(staOk ? WiFi.RSSI() : 0) + ",";
  j += "\"ip_sta\":\""    + (staOk ? WiFi.localIP().toString() : String("")) + "\",";
  j += "\"ip_ap\":\""     + WiFi.softAPIP().toString() + "\",";
  j += "\"mdId\":\""      + mideaIdS + "\",";
  j += "\"mdIp\":\""      + mideaIp + "\",";
  j += "\"mdConn\":"      + String(mdConnected ? 1 : 0) + ",";
  j += "\"mdValid\":"     + String(mdValid ? 1 : 0) + ",";
  j += "\"power\":"       + String(mdPower ? 1 : 0) + ",";
  j += "\"running\":"     + String((mdPower && (mdStatus==2||mdStatus==6)) ? 1 : 0) + ",";
  j += "\"phase\":\""     + String(phases[ph]) + "\",";
  j += "\"progress\":"    + String(mdProgress) + ",";
  j += "\"timeLeft\":"    + String(mdTimeLeft) + ",";
  j += "\"program\":"     + String(mdProgram) + ",";
  j += "\"mode\":"        + String(mdMode) + ",";
  j += "\"status\":"      + String(mdStatus) + ",";
  j += "\"err\":\""       + mdLastErr + "\",";
  j += "\"raw\":\""       + mdRawHex + "\",";
  j += "\"boots\":"       + String(bootCount);
  j += "}";
  server.send(200, "application/json", j);
}

void handlePower() {
  if (!server.hasArg("on")) { server.send(400, "text/plain", "use ?on=1|0"); return; }
  bool on = server.arg("on").toInt() == 1;
  bool ok = mideaSendPower(on);
  server.send(200, "application/json", String("{\"ok\":") + (ok?"true":"false") + "}");
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "[]"); return; }
  if (n == WIFI_SCAN_FAILED || n < 0) { WiFi.scanNetworks(true); server.send(200, "application/json", "[]"); return; }
  String j = "[";
  int limit = (n > 25) ? 25 : n;
  for (int i = 0; i < limit; i++) {
    if (i > 0) j += ",";
    j += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + WiFi.RSSI(i) + "}";
  }
  j += "]";
  WiFi.scanDelete(); WiFi.scanNetworks(true);
  server.send(200, "application/json", j);
}

void handleRestart() {
  server.send(200, "text/plain", "Reiniciando..."); delay(300); ESP.restart();
}
void handleResetWiFi() {
  prefs.clear(); server.send(200, "text/plain", "Apagado. Reiniciando..."); delay(400); ESP.restart();
}

// ---------- /config (wizard estilo Foreasy) ----------
void handleConfigPage() {
  String html = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Foreasy MIDEA</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--cd2:#0b110d;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--ip:#0b130e}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'Cascadia Code',monospace;font-size:13px;min-height:100vh}
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
.fill{height:100%;width:0;background:linear-gradient(90deg,var(--ac2),var(--ac));transition:width .35s}
.card-wrap{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px 16px;min-height:230px}
.step{display:none}.step.on{display:block}
.sec{color:var(--lb);font-size:10px;letter-spacing:2px;text-transform:uppercase;margin:16px 0 8px;padding-bottom:5px;border-bottom:1px solid var(--bd)}
.sec:first-child{margin-top:0}
label{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;display:block;margin:11px 0 4px}
input,select{width:100%;background:var(--ip);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:10px 12px;font-family:inherit;font-size:13px;outline:none}
input:focus,select:focus{border-color:var(--ac);box-shadow:0 0 0 3px rgba(0,230,118,.1)}
.hint{color:var(--mu);font-size:10px;line-height:1.5;margin-top:10px;padding:8px 10px;border-left:2px solid var(--bd)}
.nav{display:flex;gap:8px;margin-top:16px}
.btn{flex:1;padding:13px;border:none;border-radius:5px;font-family:inherit;font-size:12px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}
.btn.ghost{background:transparent;border:1px solid var(--bd);color:var(--mu);flex:0 0 110px}
.msg{margin-top:12px;font-size:12px;min-height:16px;color:var(--ac);text-align:center}
.foot{margin-top:16px;text-align:center;font-size:11px}.foot a{color:var(--mu);text-decoration:none}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">midea iot · esp32-s3</div></header>
<main>
  <div class="steps">
    <div class="pill cur" id="pill0"><b>1</b><span>Rede</span></div>
    <div class="pill" id="pill1"><b>2</b><span>Maquina MIDEA</span></div>
  </div>
  <div class="track"><div class="fill" id="bar"></div></div>
  <div class="card-wrap">
    <div class="step on" id="step0">
      <div class="sec">Rede 1 — primaria</div>
      <label>Redes encontradas</label><select id="ssid_scan"></select>
      <label>Ou SSID manual</label><input id="ssid" placeholder="nome da rede">
      <label>Senha</label><input id="pass" type="text" placeholder="vazio se aberta">
      <div class="sec">Rede 2 — failover (opcional)</div>
      <label>SSID</label><input id="ssid2" placeholder="opcional">
      <label>Senha</label><input id="pass2" type="text" placeholder="opcional">
      <div class="sec">Identificacao</div>
      <label>Node ID</label><input id="nodeid" placeholder="ex: MIDEA-001">
    </div>
    <div class="step" id="step1">
      <div class="sec">Maquina MIDEA (do get_device_info.py)</div>
      <label>Device ID (obrigatorio)</label><input id="mdId" placeholder="ex: 150633095703018">
      <label>IP da maquina (opcional — descoberto sozinho)</label><input id="mdIp" placeholder="deixe vazio p/ auto">
      <label>Token (128 hex)</label><input id="mdToken" placeholder="token V3">
      <label>Key (64 hex)</label><input id="mdKey" placeholder="key V3">
      <label>Tipo (hex, ex: DB)</label><input id="mdType" placeholder="DB">
      <div class="hint">Rode <b>rodar.bat</b> na mesma rede da maquina para obter Device ID, Token e Key (1x). O <b>IP e opcional</b>: o ESP descobre pelo Device ID via broadcast e se reajusta se o IP mudar (DHCP).</div>
    </div>
  </div>
  <div class="nav">
    <button class="btn ghost" id="back">Voltar</button>
    <button class="btn" id="next">Avancar</button>
  </div>
  <div class="msg" id="msg"></div>
  <div class="foot"><a href="/info">→ status (/info)</a></div>
</main>
<script>
function qs(i){return document.getElementById(i);}
const N=2;let cur=0;
function paint(){for(let i=0;i<N;i++){qs('step'+i).classList.toggle('on',i===cur);qs('pill'+i).classList.toggle('done',i<cur);qs('pill'+i).classList.toggle('cur',i===cur);}
qs('bar').style.width=(cur/(N-1)*100)+'%';qs('back').style.visibility=cur?'visible':'hidden';qs('next').textContent=cur===N-1?'Salvar e Reiniciar':'Avancar';}
function msg(t){qs('msg').textContent=t;}
function valid(){if(cur===0){let s=qs('ssid').value.trim()||qs('ssid_scan').value;if(!s){msg('Selecione a rede 1');return false;}if(!qs('nodeid').value.trim()){msg('Preencha o Node ID');return false;}}msg('');return true;}
function next(){if(cur===N-1)return save();if(!valid())return;cur++;paint();}
function back(){if(cur>0){cur--;paint();}}
function save(){
  let ss=qs('ssid').value.trim()||qs('ssid_scan').value;
  let tp=qs('mdType').value.trim();let tpv=tp?parseInt(tp,16):219;
  msg('Salvando...');
  let b='ssid='+encodeURIComponent(ss)+'&pass='+encodeURIComponent(qs('pass').value)+
    '&ssid2='+encodeURIComponent(qs('ssid2').value.trim())+'&pass2='+encodeURIComponent(qs('pass2').value)+
    '&nodeid='+encodeURIComponent(qs('nodeid').value.trim())+
    '&mdId='+encodeURIComponent(qs('mdId').value.trim())+'&mdIp='+encodeURIComponent(qs('mdIp').value.trim())+
    '&mdToken='+encodeURIComponent(qs('mdToken').value.trim())+'&mdKey='+encodeURIComponent(qs('mdKey').value.trim())+
    '&mdType='+tpv;
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
    .then(r=>r.text()).then(t=>msg(t)).catch(()=>msg('Falha ao salvar.'));
}
function scan(retry){retry=retry||0;fetch('/scan').then(r=>r.json()).then(list=>{
  if(list.length===0&&retry<6){setTimeout(()=>scan(retry+1),2500);return;}
  let s=qs('ssid_scan');s.innerHTML='';
  list.forEach(i=>{let o=document.createElement('option');o.value=i.ssid;o.textContent=i.ssid+' · '+i.rssi+'dBm';s.appendChild(o);});
}).catch(()=>{if(retry<6)setTimeout(()=>scan(retry+1),2500);});}
window.onload=()=>{fetch('/config-data').then(r=>r.json()).then(d=>{
  qs('ssid').value=d.ssid||'';qs('pass').value=d.pass||'';qs('ssid2').value=d.ssid2||'';qs('pass2').value=d.pass2||'';
  qs('nodeid').value=d.nodeid||'';qs('mdId').value=d.mdId||'';qs('mdIp').value=d.mdIp||'';
  qs('mdToken').value=d.mdToken||'';qs('mdKey').value=d.mdKey||'';qs('mdType').value=(d.mdType||219).toString(16).toUpperCase();
}).catch(()=>{});scan();qs('next').onclick=next;qs('back').onclick=back;paint();};
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ---------- /info (status ao vivo) ----------
void handleInfoPage() {
  String html = R"rawliteral(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="refresh" content="3">
<title>Foreasy MIDEA — Info</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--lb:#4ade80;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,monospace;font-size:13px;min-height:100vh}
header{padding:18px 18px 6px;text-align:center}
.logo{color:var(--ac);font-size:20px;font-weight:700;letter-spacing:4px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px;text-transform:uppercase}
main{max-width:900px;margin:0 auto;padding:10px 16px 28px}
.big{background:var(--cd);border:1px solid var(--bd);border-radius:8px;padding:18px;text-align:center;margin:12px 0}
.big .l{color:var(--mu);font-size:10px;letter-spacing:2px;text-transform:uppercase}
.big .v{font-size:30px;font-weight:700;margin-top:6px;color:var(--ac)}
.big .s{color:var(--mu);font-size:11px;margin-top:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));gap:8px}
.it{background:var(--cd);border:1px solid var(--bd);border-radius:6px;padding:11px 13px}
.lb{color:var(--lb);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}
.vl{color:var(--tx);font-size:14px;font-weight:600;word-break:break-all}
pre{font-size:11px;background:#060a07;color:#4ade80;padding:10px;border-radius:6px;overflow:auto;margin-top:14px;border:1px solid var(--bd);white-space:pre-wrap}
.nav{margin-top:16px;font-size:11px;display:flex;gap:18px;flex-wrap:wrap}.nav a{color:var(--mu);text-decoration:none}
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">midea iot · status — atualiza 3s</div></header>
<main>
  <div class="big">
    <div class="l">Tempo restante</div>
    <div class="v" id="time">…</div>
    <div class="s" id="phase">…</div>
  </div>
  <div class="grid">
    <div class="it"><div class="lb">Node ID</div><div class="vl" id="node">…</div></div>
    <div class="it"><div class="lb">Energia</div><div class="vl" id="pw">…</div></div>
    <div class="it"><div class="lb">Em uso</div><div class="vl" id="run">…</div></div>
    <div class="it"><div class="lb">MIDEA TCP</div><div class="vl" id="conn">…</div></div>
    <div class="it"><div class="lb">Device ID</div><div class="vl" id="mid">…</div></div>
    <div class="it"><div class="lb">IP maquina</div><div class="vl" id="mip">…</div></div>
    <div class="it"><div class="lb">Wi-Fi</div><div class="vl" id="wifi">…</div></div>
    <div class="it"><div class="lb">RSSI</div><div class="vl" id="rssi">…</div></div>
    <div class="it"><div class="lb">IP (STA)</div><div class="vl" id="ipsta">…</div></div>
    <div class="it"><div class="lb">Programa/Modo</div><div class="vl" id="prog">…</div></div>
    <div class="it"><div class="lb">Ultimo erro</div><div class="vl" id="err">…</div></div>
    <div class="it"><div class="lb">Boots</div><div class="vl" id="boots">…</div></div>
  </div>
  <div class="nav"><a href="/config">← configuracao</a><a href="/restart">reiniciar</a></div>
  <pre id="raw"></pre>
</main>
<script>
fetch('/status').then(r=>r.json()).then(j=>{
  document.getElementById('time').textContent=j.power?(j.timeLeft+' min'):'—';
  document.getElementById('phase').textContent=j.power?j.phase:'maquina desligada';
  document.getElementById('node').textContent=j.nodeId||'—';
  document.getElementById('pw').textContent=j.power?'LIGADA':'desligada';
  document.getElementById('pw').style.color=j.power?'#00e676':'#557060';
  document.getElementById('run').textContent=j.running?'SIM':'nao';
  document.getElementById('conn').textContent=j.mdConn?'conectado':'desconectado';
  document.getElementById('conn').style.color=j.mdConn?'#00e676':'#f87171';
  document.getElementById('mid').textContent=j.mdId||'—';
  document.getElementById('mip').textContent=j.mdIp||'—';
  document.getElementById('wifi').textContent=j.ssid||'—';
  document.getElementById('rssi').textContent=(j.rssi||0)+' dBm';
  document.getElementById('ipsta').textContent=j.ip_sta||'—';
  document.getElementById('prog').textContent='P'+j.program+' / M'+j.mode+' (st'+j.status+')';
  document.getElementById('err').textContent=j.err||'—';
  document.getElementById('boots').textContent=j.boots;
  document.getElementById('raw').textContent=JSON.stringify(j,null,2);
}).catch(()=>{});
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

// ============================================================================
// SETUP / LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  prefs.begin("midea", false);
  loadPrefs();
  bootCount++;

  bootTimeMs = millis();
  apEnabled  = true;
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  setupAPSTA();
  startWebServer();
  WiFi.scanNetworks(true);

  if (hasSavedWiFi()) connectToWiFi_begin();

  Serial.printf("MIDEA IoT %s | nodeId=%s | mdId=%s | token=%dB key=%dB\n",
                FW_VERSION, nodeId.c_str(), mideaIdS.c_str(), mdTokenLen, mdKeyLen);
}

void loop() {
  server.handleClient();
  wifiTick();
  mideaTick();
  apLifetimeTick();
}
