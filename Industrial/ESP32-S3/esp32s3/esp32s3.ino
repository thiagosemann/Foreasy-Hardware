// ============================================================================
// Foreasy ESP32-S3 - Industrial
// Hardware: ESP32-S3 DevKit + saida START IN
//   Em uso: N8R2 (8MB flash / 2MB PSRAM). Previsto: N16R8 (16MB / 8MB).
//   A mesma imagem serve as duas - o que muda sao duas opcoes de build (ver BUILD).
//
// Porte do firmware ESP32-C3 1.3.0 (Industrial/ESP32-C3/esp32c3/esp32c3.ino).
// A logica de negocio, o protocolo e as telas sao os MESMOS - o que muda e o que
// e especifico do chip: pinos, LED, validacao de GPIO, USB CDC e a politica de
// potencia de radio. Ao corrigir um bug de logica, corrija nos dois.
//
// POR QUE ESTA PLACA: o lote de ESP32-C3 Super Mini do segundo fornecedor nao
// sustenta o pico de corrente do TX em potencia cheia - a peca enxerga a rede mas
// nao associa e nem sobe o proprio AP (docs/LAUDO-LOTE-ESP32C3.md). O S3 DevKit
// tem regulador e desacoplamento dimensionados para o modulo, entao aqui a peca
// opera em 19,5 dBm por padrao.
//
// PAGINAS WEB: "/" (menu), /wizard (= /config), /admin e /info.
//
// SISTEMA: Industrial - pulso de START IN + leitura do AVAIL OUT (Speed Queen).
//   Pulso de START IN (Speed Queen, conector H3-7) via GPIO direto (startPin).
//   GPIO sobe por PULSE_MS (100ms) e desce.
//   WS 0x01 => dispara pulso | WS 0x02 => ignorado
//
//   FAIL-SAFE AVAIL (availEnabled - opcional; modelos sem AVAIL deixam OFF):
//   maquina LIVRE (AVAIL LOW) -> pulso -> confirma LIVRE->OCUPADA (AVAIL HIGH) em
//   ate CREDIT_CONFIRM_MS; se nao confirmar, repulsa apos CREDIT_GAP_MS, ate
//   CREDIT_MAX_ATTEMPTS tentativas. Resposta assincrona ao backend:
//   "RelayStatus:ON" (sucesso) | "CreditFail" (esgotou) | "CreditBusy" (ja ocupada).
//   Sem availEnabled: um pulso so, sem confirmacao.
//
// PROTOCOLO WEBSOCKET (binario):
// - 0x01 => pulso START IN 100ms
// - 0x02 => ignorado
// - 0x03 => JSON: rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw
// - 0x04 => OTA: payload = 0x04 + "url|sha256" (sha256 opcional, 64 hex). Baixa o .bin,
//           grava com Update, valida SHA256 e reinicia. Respostas async:
//           "OTA:QUEUED" -> "OTA:START" -> "OTA:OK:restart" | "OTA:FAIL:<motivo>"
// - 0x05 => JSON status do AVAIL: {"type":"avail","livre":bool,"raw":-1/0/1,
//           "sinceMs":ms,"availEn":0/1,"machineMode":1}
// - 0x06 => Restart remoto. Responde "Restarting" e reinicia apos ~300ms.
// - 0x07 => Pede o buffer de log da serial (o mesmo ring de RAM do /log HTTP).
//           Resposta: JSON {"type":"log","data":"<texto escapado>"}.
//
// DETECCAO DE FICHA (token) - so com availEnabled e creditState IDLE:
// - "TokenInserted" : AVAIL LOW->HIGH externo (ficha) ou snapshot ocupado no connect
// - "TokenFinished" : AVAIL HIGH->LOW externo ou snapshot livre no connect
//   O backend cruza com is_in_use antes de marcar a ficha (nao toca is_in_use).
//
// WIFI:
// - Potencia de TX configuravel por peca no /admin (NVS "txpower"). Nesta placa o
//   padrao e 19,5 dBm (potencia cheia) - ver o bloco de comentario da secao.
//   Reafirmada a cada 5s (txPowerTick), porque toda troca de modo devolve o radio
//   ao maximo. Confira pelo campo "txp" em /status, que e leitura de volta do radio.
// - Dual WiFi com failover automatico entre rede 1 e rede 2 (sem restart)
// - Conexao nao-bloqueante: wifiTick() com timeout 40s e retry a cada 5s
// - Escada de associacao: 2 falhas -> disconnect(true) (reset de pilha);
//   3 falhas -> derruba o AP (radio unico: AP e STA dividem canal)
// - O softAP sobe no canal da ultima conexao OK (NVS "wifich"), nao em 1 fixo
// - Credenciais NUNCA apagadas por falha de conexao
//
// WEBSOCKET:
// - Servidor (host/porta) configuravel apenas via /admin e salvo na NVS
// - Reconexao feita pela lib, com backoff 3s->30s empurrado via
//   setReconnectInterval() (sem isso ela martela a cada 500ms, o default dela)
// - Escada de recuperacao, tudo automatico e sem depender de configuracao:
//     5 falhas de handshake     -> reset da pilha de WiFi (imediato)
//     >3min sem WS (WiFi de pe) -> reset da pilha de WiFi
//     >6min                     -> failover de rede + alterna a potencia de TX
//     >8min sem WiFi nem WS     -> failover de WiFi
//     >15min sem WS             -> REINICIA a peca (+ jitter de ate 3min por peca)
// - Deteccao de zumbi : sem ping/pong por >5min -> reconecta
// - App ping a cada 30s | heartbeat: 15s/3s/2 tentativas
//
// AP: ativo 10 min apos boot (lean mode apos expirar)
//     SSID: <nodeId>-AP | Senha: 12345678
//
// ARMAZENAMENTO: Preferences (NVS) - ssid, pass, ssid2, pass2, nodeid,
//                wsHost, wsPort, startPin, availPin, availEn, ledPin, ledMode,
//                txpower, wifich, bootCount, evlog
//
// SCAN WIFI: assincrono (nao bloqueia o loop)
// TEMPERATURA: sensor interno do ESP32-S3 via temperatureRead()
//
// ---------------------------------------------------------------------------
// PINOS (configuraveis no /admin) - padroes desta placa:
// - startPin : GPIO5  (START IN, ativo HIGH)
// - availPin : GPIO6  (AVAIL OUT, INPUT_PULLUP)
// - ledPin   : GPIO48 em modo RGB (LED WS2812 embutido do DevKit)
//
// GPIOs PROIBIDOS no S3 - gpioLivre() recusa no /save, entao um engano de
// digitacao nao derruba a peca em campo:
// - 22, 23, 24, 25 : nao existem neste chip
// - 26 a 32        : barramento da flash SPI embutida (usar = a peca nao da boot)
// - 33 a 37        : barramento da PSRAM octal. So a variante R8 ocupa esses pinos
//                    (na R2 a PSRAM e quad e divide o barramento da flash), mas
//                    eles ficam bloqueados em TODAS as placas de proposito - ver
//                    a nota em gpioLivre()
// - 19, 20         : USB D-/D+ (porta nativa; usar derruba a USB e o CDC)
// Usaveis com ressalva (aceitos, mas evite): 0/3/45/46 sao strapping (definem modo
// de boot e tensao da flash) e 43/44 sao a UART0, que e o console serial.
//
// LED (ledMode, NVS): 0=desligado - 1=comum ativo HIGH - 2=comum ativo LOW -
// 3=RGB WS2812 (padrao). O DevKit N16R8 nao tem LED monocromatico de usuario: o
// unico LED controlavel e o RGB enderecavel, e a maioria das placas o traz no
// GPIO48 (algumas revisoes usam o 38 - dai o pino ser configuravel).
// Verde = WebSocket conectado | vermelho = sem WebSocket.
//
// ---------------------------------------------------------------------------
// BUILD (Arduino IDE / arduino-cli) - a placa e "ESP32S3 Dev Module".
//
// A FLASH SELECIONADA TEM DE BATER COM O CHIP REAL. Marcar 16MB numa placa de
// 8MB compila e grava sem reclamar: quem reclama e o bootloader, comparando o
// cabecalho da imagem com o chip, e o resultado e panico em loop antes de o
// sketch rodar uma linha -
//     E (235) spi_flash: Detected size(8192k) smaller than the size in the
//     binary image header(16384k). Probe failed.
//     assert failed: __esp_system_init_fn_init_flash startup_funcs.c:118
// E facil cair nisso: estes DevKits sao anunciados como N16R8 e chegam N8R2.
// Confira no log de boot ou com `esptool --port <COM> flash-id` - nao no anuncio.
//
//   N8R2 (8MB flash / 2MB PSRAM) - a placa em uso:
//     Flash Size ...... 8MB (64Mb)
//     Partition ....... 8M with spiffs (3MB APP/1.5MB SPIFFS)  [2 slots de 3,2MB]
//
//   N16R8 (16MB flash / 8MB PSRAM):
//     Flash Size ...... 16MB (128Mb)
//     Partition ....... 16M Flash (3MB APP/9.9MB FATFS)        [2 slots de 3MB]
//
//   Comum as duas:
//     PSRAM ........... Disabled (o firmware nao usa)
//     USB CDC On Boot . Enabled se grava/monitora pela porta USB NATIVA;
//                       Disabled se usa a porta do conversor UART da placa
//
//   arduino-cli compile --fqbn
//     esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=default
//     Industrial/ESP32-S3/esp32s3
//
// A PARTICAO PRECISA DE DOIS SLOTS DE APP, senao nao existe OTA 0x04: "Huge APP",
// "Minimal" e todos os "No OTA" tem um slot so, e gravar com um deles deixa a
// peca alcancavel apenas por cabo. O padrao da IDE ("Default 4MB with spiffs")
// tem os dois slots e roda em qualquer flash >= 4MB, mas sao 1,2MB por slot e o
// binario ja ocupa ~90% disso - a proxima funcionalidade para de caber.
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>

// ---------- Log em RAM (espelho da serial, sem cabo USB) ----------
// Tudo que vai para Serial.print/println/printf cai também aqui — não persiste
// no reboot nem grava em flash, é só um ring buffer para inspecionar em /info
// (via /log) enquanto a peça está com problema, sem precisar de cabo. Se ela
// reiniciar (crash/watchdog), o buffer some junto — para isso já existe o campo
// "rst" na telemetria (motivo do reset), que sobrevive porque vai para o backend.
#define LOG_RING_SIZE 4096
char   logRingBuf[LOG_RING_SIZE];
size_t logRingHead = 0;
bool   logRingWrapped = false;

void logRingPush(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    logRingBuf[logRingHead] = (char)data[i];
    logRingHead = (logRingHead + 1) % LOG_RING_SIZE;
    if (logRingHead == 0) logRingWrapped = true;
  }
}

// Conteúdo atual em ordem cronológica (mais antigo primeiro).
String logRingRead() {
  size_t start = logRingWrapped ? logRingHead : 0;
  size_t count = logRingWrapped ? LOG_RING_SIZE : logRingHead;
  String out;
  out.reserve(count);
  for (size_t i = 0; i < count; i++) out += logRingBuf[(start + i) % LOG_RING_SIZE];
  return out;
}

// Espelha a Serial real (capturada ANTES do #define abaixo) no ring buffer.
// Print já implementa print()/println()/printf() em cima de write(), então
// nenhuma das chamadas Serial.xxx espalhadas pelo arquivo precisa mudar.
// "auto&" (em vez de HardwareSerial&) de propósito: com USB CDC On Boot
// habilitado (grava/monitora pela porta USB nativa) o Serial do core é um
// HWCDC, não um HardwareSerial — tipos incompatíveis. auto deduz o tipo real
// de cada configuração de build (native USB vs conversor UART da placa).
auto& realSerial = Serial;
class TeeSerial : public Print {
public:
  void begin(unsigned long baud) { realSerial.begin(baud); }
  size_t write(uint8_t c) override { logRingPush(&c, 1); return realSerial.write(c); }
  size_t write(const uint8_t* buf, size_t size) override { logRingPush(buf, size); return realSerial.write(buf, size); }
};
TeeSerial teeSerial;
#define Serial teeSerial

#define FW_VERSION "1.4.0"   // reportado no 0x03 para auditoria da frota
#define FW_CHIP    "esp32s3" // identifica o chip na telemetria / seleção de OTA

// ---------- Potencia de transmissao do radio ----------
// Configuravel POR PECA no /admin e persistida na NVS (chave "txpower", em
// quartos de dBm - a unidade do proprio enum wifi_power_t).
// PADRAO DESTA PLACA: 19,5 dBm, o maximo do core.
//
// No C3 esta opcao nasceu como remedio, nao como ajuste: o lote ruim nao emitia
// nem o beacon do proprio AP em potencia cheia, entao la a peca virgem tinha de
// NASCER em 15 dBm, senao era impossivel configura-la (docs/LAUDO-LOTE-ESP32C3.md).
// Aqui nao existe esse fail-safe, de proposito: a migracao para o S3 e a saida
// daquele hardware, e nascer rebaixada custaria alcance de subida em toda a frota
// para resolver um problema que esta placa nao tem.
//
// O controle continua existindo para o caso oposto - instalacao alimentada por
// fonte ou cabo USB ruim, onde o pico do TX afunda o 3V3. O sintoma que justifica
// reduzir e o do laudo, e so ele: a peca associa a 15 dBm e falha a 19,5.
//
// Enum, em quartos de dBm: 19,5=78 - 17=68 - 15=60 - 13=52 - 11=44 - 8,5=34 - 5=20
const int TXPOWER_MIN_Q     =  8;   // 2 dBm - piso de sanidade
const int TXPOWER_MAX_Q     = 78;   // 19,5 dBm - padrao
const int TXPOWER_REDUCED_Q = 60;   // 15 dBm - o degrau do fallback automatico
wifi_power_t txPower = (wifi_power_t)TXPOWER_MAX_Q;   // alvo configurado

// Fallback automático de potência. Depois de falha sustentada de conexão, a peça
// passa a tentar o OUTRO nível — e fica no que funcionar. Cobre os dois sentidos:
// placa do lote fraco presa em 19,5 dBm cai para 15 e conecta; placa boa em ponto
// de sinal ruim, presa em 15, sobe para 19,5 e conecta.
//
// É diferente de escolher a potência por um teste no boot, que dá falso positivo
// em placa marginal: aqui só agimos após minutos de falha real, e o critério é o
// único que importa — conectou ou não. Nada é gravado na NVS: se a peça reiniciar,
// volta ao valor configurado.
bool txPowerFallback = false;
wifi_power_t activeTxPower() {
  if (!txPowerFallback) return txPower;
  return (txPower > (wifi_power_t)TXPOWER_REDUCED_Q) ? (wifi_power_t)TXPOWER_REDUCED_Q
                                                     : (wifi_power_t)TXPOWER_MAX_Q;
}

// ---------- Reafirmação da potência ----------
// Com txPower no máximo (o padrão) isto é inócuo: nada fica ACIMA de 19,5. Passa
// a valer nas peças configuradas abaixo do máximo — e aí é indispensável, porque
// aplicar uma vez no boot não basta:
//
// 1. Qualquer troca de modo devolve o rádio à potência máxima. A pegadinha é o
//    softAPdisconnect(true) do apLifetimeTick: por dentro ele faz
//    AP.end() -> enableAP(false) -> mode(WIFI_STA) — ou seja, uma troca de modo.
//    Sem reaplicar, a peça volta a 19,5 dBm no minuto 10 de todo boot.
//
// 2. setTxPower() é ignorado EM SILÊNCIO se nenhuma interface tiver reportado
//    START: o guard testa STA.started()/AP.started(), que leem o bit
//    ESP_NETIF_STARTED_BIT, setado por evento de forma assíncrona. Chamado logo
//    depois de WiFi.mode(), pode simplesmente não valer.
//
// A comparação é ">" e não "!=" de propósito — o rádio pode aplicar um degrau um
// pouco abaixo do pedido, e isso é aceitável; o que não pode é voltar para cima.
const uint32_t TXPOWER_CHECK_MS = 5000;
uint32_t lastTxPowerCheckMs = 0;

bool radioStarted() { return WiFi.STA.started() || WiFi.AP.started(); }

// getTxPower() devolve 19,5 dBm como fallback quando nada está iniciado, então a
// checagem de radioStarted() vem antes — sem ela, leríamos um valor inventado.
bool applyTxPower() {
  if (!radioStarted()) return false;
  wifi_power_t alvo = activeTxPower();
  if (WiFi.getTxPower() <= alvo) return true;
  WiFi.setTxPower(alvo);
  return WiFi.getTxPower() <= alvo;
}

void txPowerTick() {
  if ((millis() - lastTxPowerCheckMs) < TXPOWER_CHECK_MS) return;
  lastTxPowerCheckMs = millis();
  if (!radioStarted()) return;
  wifi_power_t cur = WiFi.getTxPower();
  wifi_power_t alvo = activeTxPower();
  if (cur > alvo) {
    Serial.printf("TXPOWER: %.1f dBm fora do alvo, reaplicando %.1f dBm\n",
                  cur / 4.0, alvo / 4.0);
    WiFi.setTxPower(alvo);
  }
}

float readInternalTempC() {
  return temperatureRead();
}

WebServer server(80);
WebSocketsClient webSocket;
Preferences prefs;

// ---------- IO (pinos configuraveis no /admin, persistidos na NVS) ----------
int startPin = 5;   // pulso START IN (Speed Queen H3-7), ativo HIGH
int availPin = 6;   // leitura AVAIL OUT (Speed Queen H3-4), INPUT_PULLUP

// ---------- LED de status ----------
// O DevKit S3 N16R8 nao tem LED monocromatico de usuario - o unico LED
// controlavel e um WS2812 enderecavel, que a maioria das placas traz no GPIO48
// (algumas revisoes usam o 38). Por isso o pino E o modo sao configuraveis: a
// mesma imagem serve para a placa com RGB, para a com LED comum e para quem
// soldou um LED externo no painel.
enum LedMode : uint8_t { LED_MODE_OFF = 0, LED_MODE_HIGH = 1, LED_MODE_LOW = 2, LED_MODE_RGB = 3 };
int     ledPin  = 48;
uint8_t ledMode = LED_MODE_RGB;

// GPIOs que este chip/placa nao permite usar. Vale como guarda do /save: um erro
// de digitacao no pino do START IN e o tipo de coisa que so aparece quando a peca
// nao da mais boot - e ai exige alguem no local com cabo, nao um OTA.
//
//   22..25 : nao existem no ESP32-S3
//   26..32 : barramento da flash SPI embutida
//   33..37 : barramento da PSRAM octal. So a variante R8 usa esses pinos - na R2 a
//            PSRAM e quad e divide o barramento da flash, entao la eles estariam
//            livres. Ficam bloqueados nas duas assim mesmo: a mesma imagem roda na
//            frota inteira, e um pino que funciona na N8R2 e mata a N16R8 custa
//            muito mais caro do que um pino a menos na lista. Vale tambem com a
//            PSRAM desativada no build - o que manda e a trilha na placa.
//   19, 20 : USB D-/D+ da porta nativa
//
// 0/3/45/46 (strapping) e 43/44 (UART0) sao aceitos: dao trabalho, mas ha casos
// legitimos de uso e recusa-los engessaria a instalacao sem necessidade.
bool gpioLivre(int p) {
  if (p < 0 || p > 48)    return false;
  if (p >= 22 && p <= 37) return false;
  if (p == 19 || p == 20) return false;
  return true;
}

// Ultimo estado desenhado, para nao reescrever o LED a cada volta do loop: o
// WS2812 e bit-bang com interrupcoes desabilitadas (~30us por atualizacao), e
// repetir isso milhares de vezes por segundo e desperdicio puro.
int lastLedState = -1;

void ledSetup() {
  lastLedState = -1;
  if (ledMode == LED_MODE_HIGH || ledMode == LED_MODE_LOW) {
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, ledMode == LED_MODE_LOW ? HIGH : LOW);   // comeca apagado
  }
}

// Verde = WebSocket conectado | vermelho = sem WebSocket. O brilho e baixo de
// proposito: o WS2812 no maximo consome ~60mA e ofusca dentro da caixa.
void ledShow(bool on) {
  int st = on ? 1 : 0;
  if (st == lastLedState) return;
  lastLedState = st;
  switch (ledMode) {
    case LED_MODE_RGB:  rgbLedWrite(ledPin, on ? 0 : 12, on ? 12 : 0, 0); break;
    case LED_MODE_HIGH: digitalWrite(ledPin, on ? HIGH : LOW);            break;
    case LED_MODE_LOW:  digitalWrite(ledPin, on ? LOW  : HIGH);           break;
    default: break;   // LED_MODE_OFF: nada a fazer
  }
}

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

// ---------- Recuperação de WiFi ----------
// O rádio do ESP32-C3 é um só: em AP+STA as duas interfaces dividem o mesmo canal.
// Com o softAP fixo num canal e a rede alvo em outro, a associação simplesmente não
// completa — foi o que travou uma peça por 10 minutos em bancada, e só destravou
// quando o AP expirou. Guardamos o canal da última conexão bem-sucedida para subir
// o AP já casado com a rede; se ainda assim falhar, derrubamos o AP.
int      wifiChannelHint = 0;    // canal da última conexão OK (0 = desconhecido)
uint8_t  wifiFailStreak  = 0;    // tentativas de associação falhadas em sequência
uint32_t wifiDownSinceMs = 0;    // desde quando o status está desconectado (0 = está OK)
// Janela de confirmação antes de recomeçar a conexão. Curta o bastante para não
// atrasar uma queda real (o timeout de tentativa é 40s), longa o bastante para
// absorver o piscar do status.
const uint32_t WIFI_DOWN_CONFIRM_MS = 5000;
const uint8_t WIFI_FAIL_HARD_RESET = 3;   // após N falhas, desliga o rádio (disconnect(true))
// Intervalo mínimo entre resets da pilha, venha o pedido do wifiTick ou do
// contador de handshakes. esp_wifi_stop()/start() é operação pesada no driver, e
// repeti-la a cada 40s — como a versão anterior fazia — é fonte plausível dos
// travamentos de loop que vimos em campo. É escape de emergência, não rotina.
const uint32_t WIFI_HARD_RESET_MIN_INTERVAL_MS = 2UL * 60UL * 1000UL;
uint32_t lastHardResetMs = 0;
bool hardResetPermitido() {
  if (lastHardResetMs != 0 && (millis() - lastHardResetMs) < WIFI_HARD_RESET_MIN_INTERVAL_MS)
    return false;
  lastHardResetMs = millis();
  return true;
}
const uint8_t WIFI_FAIL_DROP_AP    = 3;   // após N falhas, derruba o AP e tenta em STA puro
const uint32_t AP_INSTALL_GRACE_MS = 3UL * 60UL * 1000UL;  // AP intocável neste período

// Handshakes de WebSocket que morreram em sequência sem completar. O sintoma é
// inequívoco (TCP abre, o "HTTP/1.1 101" nunca chega) e a cura é conhecida: só o
// reset da pilha de WiFi resolve — reiniciar o WebSocket não adianta. Não faz
// sentido esperar o watchdog quando o diagnóstico já está fechado.
uint8_t  wsHandshakeFailStreak = 0;
const uint8_t WS_HANDSHAKE_FAIL_RESET = 5;

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
// Quem reconecta é a PRÓPRIA LIB: WebSocketsClient::loop() tenta de novo sozinha
// a cada _reconnectInterval enquanto estiver desconectada. O default dela é
// 500 ms — duas tentativas por segundo, por peça, para sempre. Com a frota fora
// do ar isso vira uma enxurrada de handshakes em cima do servidor, que atrasa a
// volta de todo mundo. O backoff abaixo passa a DIRIGIR esse intervalo
// (setReconnectInterval), em vez de correr por fora sem efeito nenhum.
//
// O backoff é derivado do TEMPO fora do ar, não de contagem de tentativas: a lib
// não avisa por callback quando uma tentativa de conexão falha
// (connectFailedCb() só escreve no log, não dispara WStype_DISCONNECTED), então
// contar por evento deixaria o intervalo travado no valor inicial para sempre.
//
// Rápido no começo — a esmagadora maioria das quedas é um blip e volta na
// primeira ou segunda tentativa — e com teto de 30s, para que nenhuma peça fique
// mais que isso sem tentar, mesmo depois de horas fora.
uint32_t wsRetryIntervalMs = 0;   // último valor empurrado para a lib
bool     wsBegun = false;         // begin() já rodou ao menos uma vez?
// Pedido de reset da pilha vindo do callback do WS. Enfileirado de propósito: o
// fullReconnectWiFiWS() destrói o socket cujo callback estaria em execução.
bool     wsResetRequested = false;

// Piso de 3s, não 1s. Cada tentativa abre um socket TCP que fica preso em
// TIME_WAIT por ~60s, e o lwIP do ESP32 tem poucos PCBs: retentar rápido demais
// esgota o pool e passa a impedir até o handshake — uma espiral que só o reset da
// pilha desfaz. 3s ainda devolve a conexão em segundos num blip.
uint32_t wsBackoffFor(uint32_t downMs) {
  if (downMs <  30000UL) return  3000UL;
  if (downMs <  60000UL) return  5000UL;
  if (downMs < 300000UL) return 10000UL;
  return 30000UL;
}

void applyWsBackoff(uint32_t downMs) {
  uint32_t want = wsBackoffFor(downMs);
  if (want == wsRetryIntervalMs) return;   // evita chamada a cada volta do loop
  wsRetryIntervalMs = want;
  webSocket.setReconnectInterval(want);
}

// ---------- Ping/pong ----------
uint32_t lastPingMs = 0;
const uint32_t PING_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// ---------- App ping ----------
uint32_t lastAppPingMs = 0;
const uint32_t APP_PING_INTERVAL_MS = 30UL * 1000UL;

// ---------- Watchdogs ----------
uint8_t  wsDownEscalation = 0;   // 0 = próxima ação é reiniciar só o WS; 1 = failover de WiFi
uint32_t wsDownSinceMs = 0;
// 3min, não 5: a medição de bancada mostrou que a peça fica inalcançável o tempo
// inteiro dessa janela, e a ação (reset da pilha) leva 2 segundos e é inócua.
const uint32_t WS_DOWN_RESET_MS = 3UL * 60UL * 1000UL;
uint32_t lastConnectivityOkMs = 0;
const uint32_t GLOBAL_DOWN_RESET_MS = 8UL * 60UL * 1000UL;

// ---------- WS auto-restart ----------
// Último recurso, e INCONDICIONAL de propósito: é a única coisa que tira a peça
// de um estado travado sem alguém ir até lá. Antes era um checkbox que vinha
// desmarcado por padrão — ou seja, na prática a maioria da frota não tinha
// reinício automático nenhum e ficava fora até puxarem da tomada.
// Reiniciar aqui é inócuo: o START IN é um pulso, então a máquina em
// funcionamento não para, e o tick nunca reinicia no meio de um pulso.
uint32_t wsLastOkMs = 0;
const uint32_t WS_RESTART_TIMEOUT_MS = 15UL * 60UL * 1000UL; // 15 minutos
// Jitter por peça, derivado do MAC. Sem ele, uma queda longa do servidor faz a
// frota inteira reiniciar no mesmo segundo e voltar toda junta. Dimensionado em
// 3min para a janela real ficar entre 15 e 18min — espalhamento suficiente para
// 40 peças sem esticar demais o tempo de recuperação.
uint32_t wsRestartJitterMs = 0;
const uint32_t WS_RESTART_JITTER_MAX_MS = 3UL * 60UL * 1000UL;

// ---------- Watchdog do loop ----------
// O Arduino DESABILITA o task watchdog do loopTask por padrão (main.cpp:
// loopTaskWDTEnabled = false). Sem ele, um loop() travado não é detectado por
// nada: os ticks param, o auto-restart nunca roda, e a peça fica ligada e inerte
// por tempo indefinido. Em campo isso deu 2h45min de uma peça congelada — que só
// voltou porque um INT_WDT acabou disparando por outro caminho.
//
// O timeout é folgado de propósito. O loop tem chamadas bloqueantes legítimas:
// webSocket.loop() pode segurar até 5s num connect, o testWsSync do wizard chega
// a 20s. 60s não pega nenhuma delas e ainda assim reduz um congelamento de horas
// para um minuto. O OTA é bem mais longo e alimenta o watchdog por dentro.
const uint32_t LOOP_WDT_TIMEOUT_S = 60;

// ---------- Boot count ----------
uint32_t bootCount = 0;

// Motivo do último reset — distingue queda de energia, reinício por software,
// brownout (alimentação da placa) e crash. Sem isso não dá para saber se a peça
// voltou porque o auto-restart funcionou ou porque faltou luz.
const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "UNKNOWN";
  }
}

// ================= CAIXA-PRETA DE DESCONEXÃO =================
// Registra o que aconteceu enquanto a peça esteve sem servidor e entrega o
// relato assim que reconecta. Existe porque hoje, quando uma peça some, não
// sobra rastro nenhum: quem chega no local encontra a peça ligada e sem conexão,
// e a única ação disponível — reiniciar — apaga qualquer evidência.
//
// Duas decisões importantes:
//
// 1. O log é PERSISTIDO NA NVS. Manter só em RAM perderia exatamente o caso que
//    motivou o recurso: o técnico reinicia a peça na mão e leva o histórico
//    junto. Persistido, o relato sobrevive a reinício manual, queda de energia e
//    ao auto-restart, e é enviado no reconect seguinte.
//
// 2. O campo mais valioso é o `reason` do ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
//    o código com que o próprio AP/stack explica a queda. É ele que separa
//    "roteador sumiu" (201 NO_AP_FOUND) de "perdeu os beacons por distância"
//    (200 BEACON_TIMEOUT) de "o AP expulsou a peça" (8 ASSOC_LEAVE) de senha
//    errada (15 HANDSHAKE_TIMEOUT). E a AUSÊNCIA de qualquer EV_WIFI_DOWN numa
//    peça que ficou horas fora é diagnóstico por si só: significa que ela
//    seguia associada e só não conseguia transmitir.
enum EvCode : uint8_t {
  EV_BOOT      = 1,   // arg = esp_reset_reason(). Também marca onde o relógio zerou.
  EV_WIFI_UP   = 2,   // arg = RSSI
  EV_WIFI_DOWN = 3,   // arg = reason code do ESP-IDF
  EV_WS_UP     = 4,
  EV_WS_DOWN   = 5,
  EV_WD_WS     = 6,   // watchdog reiniciou só o WebSocket
  EV_WD_FAIL   = 7,   // watchdog trocou de rede; arg = slot que passou a valer
};

struct Ev { uint32_t t; int16_t arg; uint8_t code; };
const uint8_t EV_MAX = 20;

// count/head moram DENTRO do blob persistido para que salvar seja uma única
// escrita na NVS em vez de três — numa peça que oscila, a diferença é entre
// dezenas e centenas de escritas por dia.
struct EvLog { uint8_t count; uint8_t head; Ev ev[EV_MAX]; };
EvLog evLog = { 0, 0, {} };

bool     evDirty = false;
uint32_t evLastSaveMs = 0;
// Grava no máximo a cada 2min. Perder até 2min de eventos é irrelevante aqui, e
// mantém o desgaste da flash em dezenas de escritas por dia mesmo em queda crônica.
const uint32_t EV_SAVE_MIN_INTERVAL_MS = 2UL * 60UL * 1000UL;
// evAdd também é chamado da task de eventos do WiFi, não só do loop: sem o
// spinlock, uma preempção no meio da escrita corromperia o buffer.
portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;

void evAdd(uint8_t code, int16_t arg) {
  uint32_t t = millis() / 1000UL;
  portENTER_CRITICAL(&evMux);
  evLog.ev[evLog.head] = { t, arg, code };
  evLog.head = (evLog.head + 1) % EV_MAX;
  if (evLog.count < EV_MAX) evLog.count++;
  evDirty = true;
  portEXIT_CRITICAL(&evMux);
}

void evLoad() {
  if (prefs.getBytesLength("evlog") != sizeof(evLog)) return;
  prefs.getBytes("evlog", &evLog, sizeof(evLog));
  // Blob inconsistente (versão antiga, gravação interrompida): descarta em vez
  // de indexar fora do buffer.
  if (evLog.count > EV_MAX || evLog.head >= EV_MAX) { evLog.count = 0; evLog.head = 0; }
}

void evSaveTick() {
  if (!evDirty) return;
  if ((millis() - evLastSaveMs) < EV_SAVE_MIN_INTERVAL_MS) return;
  evDirty = false;
  evLastSaveMs = millis();
  prefs.putBytes("evlog", &evLog, sizeof(evLog));
}

// Não persiste na hora de propósito: deixa para o evSaveTick, respeitando o
// limite de 2min. O pior caso é reenviar um relato já entregue se a peça
// reiniciar nesse intervalo — duplicata é inofensiva, escrita em flash não é.
void evClear() {
  portENTER_CRITICAL(&evMux);
  evLog.count = 0;
  evLog.head  = 0;
  evDirty = true;
  portEXIT_CRITICAL(&evMux);
}

// Serializa em ordem cronológica. Formato compacto [t,codigo,arg] porque isto
// viaja por WebSocket e fica guardado por peça — 20 eventos dão ~300 bytes.
String evJson() {
  String s;
  s.reserve(420);
  s += "{\"up\":" + String(millis() / 1000UL);
  s += ",\"rst\":\"" + String(resetReasonStr()) + "\"";
  s += ",\"fw\":\"" FW_VERSION "\",\"ev\":[";
  uint8_t start = (evLog.head + EV_MAX - evLog.count) % EV_MAX;
  for (uint8_t i = 0; i < evLog.count; i++) {
    const Ev& e = evLog.ev[(start + i) % EV_MAX];
    if (i) s += ",";
    s += "[" + String(e.t) + "," + String(e.code) + "," + String(e.arg) + "]";
  }
  s += "]}";
  return s;
}

// Handler de eventos do WiFi — é a única forma de obter o motivo da queda.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    evAdd(EV_WIFI_UP, (int16_t)WiFi.RSSI());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    evAdd(EV_WIFI_DOWN, (int16_t)info.wifi_sta_disconnected.reason);
  }
}

// ---------- OTA (requisição enfileirada; executa em otaTick, fora do callback WS) ----------
bool   otaRequested = false;
bool   otaInProgress = false;
String otaPendingUrl;
String otaPendingSha;

// ---------- Restart remoto (enfileirado; executa em restartTick, fora do callback WS) ----------
bool     restartRequested = false;
uint32_t restartAtMs      = 0;

// ---------- Wizard: teste ao vivo de WiFi/WS (não-bloqueante) ----------
enum TestState : uint8_t { TST_IDLE, TST_RUN, TST_OK, TST_FAIL };
TestState wifiTestState   = TST_IDLE;
bool      wifiTestActive  = false;
uint32_t  wifiTestStartMs = 0;
uint32_t  wifiTestDoneMs  = 0;   // quando o teste terminou (0 = não terminou)
const uint32_t WIFI_TEST_TIMEOUT_MS = 12000;
// Janela em que a peça segue "em modo teste" depois do resultado, para o
// instalador ler a resposta e seguir para o /test-ws. Depois disso ela volta
// sozinha à operação normal, aconteça o que acontecer.
const uint32_t WIFI_TEST_HOLD_MS = 3UL * 60UL * 1000UL;
String    wifiTestSsid, wifiTestPass;
String    lastGoodSsid, lastGoodPass;   // última rede testada com sucesso (p/ o teste de WS)

// ================= PULSO START IN =================
// dispara pulso no startPin (START IN), ativo HIGH
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

// ================= FAILOVER =================
void fullReconnectWiFiWS() {
  webSocket.disconnect();
  isWebSocketConnected = false;
  delay(50);
  WiFi.disconnect(true);
  delay(150);
  wifiConnecting = false;
  lastWiFiAttemptMs = 0;
  lastPingMs = 0;
  wsDownSinceMs = 0;
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
  lastPingMs = 0;
  wsDownSinceMs = 0;
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
  wifiChannelHint  = prefs.getInt("wifich", 0);
  ledPin           = prefs.getInt("ledPin",  ledPin);
  ledMode          = (uint8_t)constrain(prefs.getInt("ledMode", ledMode), 0, 3);
  // 0 = chave ausente (nenhum valor valido e 0) -> potencia cheia, o padrao desta
  // placa. So opera reduzido quem salvou um valor menor pelo /admin.
  int txq          = prefs.getInt("txpower", 0);
  txPower          = (wifi_power_t)(txq ? constrain(txq, TXPOWER_MIN_Q, TXPOWER_MAX_Q)
                                        : TXPOWER_MAX_Q);
  bootCount        = prefs.getUInt("bootCount",   0);
  // Pino invalido gravado (placa trocada, digitacao errada num /save antigo) volta
  // ao padrao em vez de virar peca sem boot.
  if (!gpioLivre(startPin)) startPin = 5;
  if (!gpioLivre(availPin)) availPin = 6;
  if (!gpioLivre(ledPin))   ledPin   = 48;
}

// ================= AP + STA =================
void setupAPSTA() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(activeTxPower());
  String apName = nodeId + "-AP";
  // Canal da última conexão bem-sucedida, não 1 fixo: em AP+STA o rádio é único e
  // um AP em canal diferente do roteador impede a associação do STA.
  int apCh = (wifiChannelHint >= 1 && wifiChannelHint <= 13) ? wifiChannelHint : 1;
  WiFi.softAP(apName.c_str(), "12345678", apCh, false);
  delay(200);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  // Segunda aplicação, agora com o AP no ar: a de cima roda logo após o mode() e
  // pode ter caído no guard de "nenhuma interface iniciada". O tx= do log abaixo
  // é a leitura real do rádio, então serve de conferência.
  applyTxPower();
  // O tx= é leitura real do rádio; -1 quando nenhuma interface subiu ainda (nesse
  // caso getTxPower() devolveria 19,5 de fallback e o log mentiria).
  Serial.printf("AP: %s | IP: %s | ch=%d | tx=%.1fdBm\n", apName.c_str(),
                WiFi.softAPIP().toString().c_str(), apCh,
                radioStarted() ? WiFi.getTxPower() / 4.0 : -1.0);
}

void connectToWiFi_begin() {
  if (!hasSavedWiFi()) { Serial.println("Sem credenciais WiFi."); return; }
  WiFi.mode(WIFI_AP_STA);
  // Trocar de modo reseta a potência para o máximo, e este é o caminho crítico:
  // um failover faz WiFi.disconnect(true), que derruba o rádio inteiro
  // (mode(0) -> esp_wifi_stop). Ao voltar, as interfaces levam alguns
  // milissegundos para reportar START, e sem esse bit o setTxPower é ignorado em
  // silêncio — a associação sairia em 19,5 dBm, justamente o pico de corrente que
  // este lote não sustenta. A espera é limitada e só ocorre em tentativa de
  // conexão, no máximo a cada 5s.
  uint32_t t0 = millis();
  while (!radioStarted() && (millis() - t0) < 500) delay(10);
  applyTxPower();
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
  // Não mexemos no intervalo aqui: quem manda nele é o wsTick, que o recalcula a
  // partir do downtime real na próxima volta do loop. Forçar a base neste ponto
  // seria sobrescrito imediatamente e só confundiria a leitura do código.
  wsBegun = true;
  lastPingMs = millis();
  lastAppPingMs = 0;
  Serial.printf("WS begin: %s:%u\n", wsHost.c_str(), wsPort);
}

// ================= TESTE AO VIVO (wizard) =================
// Conecta o STA na rede candidata enquanto o AP segue de pé. Para reduzir a queda do
// celular (rádio único), casa o canal do AP com o da rede alvo antes de conectar.
void startWifiTest(const String& ssid, const String& pass, int ch) {
  wifiTestActive  = true;
  wifiTestState   = TST_RUN;
  wifiTestStartMs = millis();
  wifiTestDoneMs  = 0;
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
  applyTxPower();   // recriar o softAP em outro canal também mexe no rádio
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("TEST WiFi: SSID=%s ch=%d\n", ssid.c_str(), ch);
}

void wifiTestTick() {
  // Trava de segurança. wifiTestActive suspende wifiTick() E wsTick(), e antes
  // ela só era limpa dentro do apLifetimeTick — que roda UMA vez na vida da peça,
  // no minuto 10. Como o servidor HTTP também responde no IP da rede local, um
  // /test-wifi disparado depois disso deixava a peça sem gestão de WiFi e sem
  // WebSocket até alguém puxar da tomada. Agora a janela sempre expira sozinha.
  if (wifiTestActive && wifiTestState != TST_RUN && wifiTestDoneMs != 0 &&
      (millis() - wifiTestDoneMs) > WIFI_TEST_HOLD_MS) {
    Serial.println("TEST WiFi: janela expirou, retomando operacao normal.");
    wifiTestActive = false;
    wifiTestState  = TST_IDLE;
    wifiTestDoneMs = 0;
    if (hasSavedWiFi()) connectToWiFi_begin();
    return;
  }

  if (wifiTestState != TST_RUN) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiTestState  = TST_OK;
    wifiTestDoneMs = millis();
    lastGoodSsid = wifiTestSsid;
    lastGoodPass = wifiTestPass;
    Serial.printf("TEST WiFi OK. IP=%s\n", WiFi.localIP().toString().c_str());
  } else if ((millis() - wifiTestStartMs) > WIFI_TEST_TIMEOUT_MS) {
    wifiTestState  = TST_FAIL;
    wifiTestDoneMs = millis();
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

// Teste de WebSocket (síncrono): handshake → envia "WhoAmI:<nodeId>" → lê a resposta
// do servidor com prédio/máquina. Retorna true se conectou (101); `found` indica se o
// servidor reconheceu o nodeId.
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
    feedLoopWDT();
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
      feedLoopWDT();   // o download passa de 60s; sem isto o watchdog abortaria o OTA
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
      wsHandshakeFailStreak = 0;
      evAdd(EV_WS_UP, 0);
      String msg = pulseActive ? ("ID:" + nodeId) : ("NID:" + nodeId);
      webSocket.sendTXT(msg);
      // Entrega a caixa-preta logo depois do ID — o servidor precisa saber de
      // quem é o relato antes de recebê-lo. Limpa após enviar para não repetir
      // o mesmo histórico em cada reconexão.
      if (evLog.count > 0) {
        webSocket.sendTXT("Offline:" + evJson());
        evClear();
      }
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
      evAdd(EV_WS_DOWN, 0);
      // Este evento também dispara quando o handshake NUNCA completou: o
      // clientDisconnect() da lib emite WStype_DISCONNECTED sempre que há socket
      // para fechar, inclusive no "Header response timeout" (TCP abre, o 101 não
      // vem). Vários seguidos sem nenhum WStype_CONNECTED no meio = pilha de rede
      // travada, e aí só o reset resolve — sem esperar o watchdog.
      //
      // MAS só conta com o WiFi associado. Sem rede o WebSocket falha por motivo
      // óbvio, e contar isso disparava um reset de WiFi justamente enquanto ele
      // tentava subir — que produzia mais falhas de WebSocket, que disparavam
      // outro reset. Em bancada esse laço se repetiu dez vezes e nunca convergia.
      if (WiFi.status() != WL_CONNECTED) {
        wsHandshakeFailStreak = 0;
      } else if (wsHandshakeFailStreak < 250) {
        wsHandshakeFailStreak++;
      }
      Serial.printf("WS desconectado (%u seguidas). Retentativa a cargo da lib.\n",
                    wsHandshakeFailStreak);
      if (wsHandshakeFailStreak >= WS_HANDSHAKE_FAIL_RESET) {
        wsHandshakeFailStreak = 0;
        Serial.println("WS: falhas seguidas de handshake. Reset da pilha de WiFi.");
        wsResetRequested = true;   // executado no wsTick, fora do callback
      }
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
          char buf[420];
          snprintf(buf, sizeof(buf),
            "{\"rssi\":%d,\"ch\":%d,\"heap\":%u,\"block\":%u,\"cpu\":%u,"
            "\"uptime\":%lu,\"boots\":%lu,\"wifiSlot\":%u,\"temp\":%.1f,"
            "\"txp\":%.1f,\"rst\":\"%s\","
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
            radioStarted() ? WiFi.getTxPower() / 4.0 : -1.0,
            resetReasonStr(),
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
            "\"availEn\":%u,\"machineMode\":1}",
            availLivre() ? "true" : "false",
            availStable,
            (unsigned long)(millis() - availStableAtMs),
            (unsigned)(availEnabled ? 1 : 0)
          );
          webSocket.sendTXT(buf);
          break;
        }

        if (b == 0x07) {
          String log = logRingRead();
          String esc;
          esc.reserve(log.length() + 24);
          esc += "{\"type\":\"log\",\"data\":\"";
          for (size_t i = 0; i < log.length(); i++) {
            char c = log[i];
            if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
            else if (c == '\n') esc += "\\n";
            else if (c == '\r') { /* descarta — sobra so \n */ }
            else if (c == '\t') esc += "\\t";
            else if ((uint8_t)c < 0x20) { /* outros de controle: descarta */ }
            else esc += c;
          }
          esc += "\"}";
          webSocket.sendTXT(esc);
          break;
        }

        bool reply = true;
        if (b == 0x01) {
          if (availEnabled) { creditStart(); reply = false; }
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
  generateSessionToken();
#if ARDUINO_USB_CDC_ON_BOOT
  // Serial pela USB nativa. Sem este timeout, cada Serial.print segura o loop por
  // ate 100ms quando o host enumerou a porta e ninguem esta lendo - que e
  // exatamente o caso de uma peca alimentada por carregador ou power bank USB.
  // Zero = escreve o que couber no buffer e segue adiante; log e diagnostico, e
  // diagnostico nao pode atrasar o controle da maquina.
  Serial.setTxTimeoutMs(0);
#endif
  delay(100);

  prefs.begin("wifi", false);
  loadPrefs();

  bootCount++;
  // Gravado a cada boot, não só no /save. Antes o contador voltava sempre ao
  // valor da última configuração, então a telemetria reportava o mesmo número
  // para sempre e não dava para ver reinício nenhum. O desgaste é irrelevante:
  // a NVS faz wear leveling e são poucas dezenas de gravações por dia no pior caso.
  prefs.putUInt("bootCount", bootCount);
  Serial.printf("Boot #%lu | motivo do ultimo reset: %s\n",
                (unsigned long)bootCount, resetReasonStr());

  // Jitter do auto-restart, derivado do MAC de fábrica. Deliberadamente NÃO é
  // esp_random(): aqui no setup o rádio ainda não subiu, e sem Wi-Fi/BT ativo o
  // gerador não é verdadeiramente aleatório — havia risco real de a frota
  // inteira sortear o mesmo valor, que é exatamente o que o jitter existe para
  // evitar. O MAC é único por placa e estável entre reinícios, que é o que
  // queremos: espalhamento determinístico, não aleatoriedade.
  // Caixa-preta: carrega o que sobrou do boot anterior ANTES de registrar o
  // EV_BOOT, para que o relato preserve o que aconteceu antes do reinício —
  // que é justamente o trecho que interessa quando alguém reinicia na mão.
  evLoad();
  evAdd(EV_BOOT, (int16_t)esp_reset_reason());

  wsRestartJitterMs = (uint32_t)(ESP.getEfuseMac() % WS_RESTART_JITTER_MAX_MS);
  Serial.printf("Auto-restart sem WS em %lumin (base 15 + jitter %lumin)\n",
                (unsigned long)((WS_RESTART_TIMEOUT_MS + wsRestartJitterMs) / 60000UL),
                (unsigned long)(wsRestartJitterMs / 60000UL));

  ledSetup();
  pinMode(startPin, OUTPUT);
  digitalWrite(startPin, LOW);
  pinMode(availPin, INPUT_PULLUP);
  availReading    = digitalRead(availPin);
  availStable     = availReading;
  availLastMs     = millis();
  availStableAtMs = millis();
  pulseActive    = false;
  Serial.printf("Pinos: START=%d AVAIL=%d LED=%d (modo %u)\n",
                startPin, availPin, ledPin, (unsigned)ledMode);

  bootTimeMs            = millis();
  apEnabled             = true;
  lastConnectivityOkMs  = millis();
  wsLastOkMs            = millis();
  applyWsBackoff(0);

  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);   // captura o motivo das quedas de WiFi

  // Watchdog do loop. O reconfigure é necessário porque o core já inicializa o
  // TWDT com um timeout curto demais para as nossas chamadas bloqueantes.
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = LOOP_WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
  enableLoopWDT();
  Serial.printf("Loop watchdog ativo (%lus)\n", (unsigned long)LOOP_WDT_TIMEOUT_S);

  setupAPSTA();
  startWebServer();
  server.begin();
  Serial.printf("HTTP server iniciado. modo=INDUSTRIAL chip=%s fw=%s\n",
                FW_CHIP, FW_VERSION);

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
  txPowerTick();
  evSaveTick();
  wsRestartTick();
  otaTick();
  restartTick();

  ledShow(isWebSocketConnected);
}

// ================= TICKS =================
void wifiTick() {
  if (wifiTestActive) return;   // STA é gerido pelo teste do wizard
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      wifiFailStreak = 0;
      // Guarda o canal para o próximo boot subir o AP já casado com o roteador.
      int ch = WiFi.channel();
      if (ch >= 1 && ch <= 13 && ch != wifiChannelHint) {
        wifiChannelHint = ch;
        prefs.putInt("wifich", ch);
      }
      Serial.printf("WiFi conectado. IP=%s ch=%d\n",
                    WiFi.localIP().toString().c_str(), ch);
      if (!isWebSocketConnected) connectToWebSocket();
    } else if ((millis() - wifiConnectStartMs) > WIFI_MAX_WAIT_MS) {
      wifiConnecting = false;
      if (wifiFailStreak < 250) wifiFailStreak++;
      Serial.printf("WiFi timeout (%u seguidas). Tentará novamente.\n", wifiFailStreak);

      // Escalonamento. Antes eram N tentativas idênticas: disconnect(false) +
      // begin(), repetido para sempre. Em bancada foram 14 falhas iguais em 10
      // minutos — repetir a mesma coisa não tira o STA de um estado travado.
      // A janela inicial é intocável: derrubar o AP após ~2min deixaria o
      // instalador sem acesso no meio da configuração, justamente quando o Wi-Fi
      // ainda está errado e falhando. Só depois disso vale trocar o AP pelo rádio.
      bool janelaDoInstalador = (millis() - bootTimeMs) < AP_INSTALL_GRACE_MS;
      if (wifiFailStreak >= WIFI_FAIL_DROP_AP && apEnabled && !janelaDoInstalador) {
        // O AP é a causa mais provável: rádio único, canais diferentes. Derrubá-lo
        // libera o STA para associar em qualquer canal.
        Serial.println("WiFi: derrubando o AP para liberar o radio.");
        WiFi.softAPdisconnect(true);
        apEnabled = false;
        lastConnectivityOkMs = millis();
        applyTxPower();
      }
      if (wifiFailStreak >= WIFI_FAIL_HARD_RESET && hardResetPermitido()) {
        // disconnect(true) derruba a interface (esp_wifi_stop) e libera os PCBs
        // do lwIP; disconnect(false) só solta a associação e mantém o travamento.
        Serial.println("WiFi: reset da pilha (disconnect(true)).");
        WiFi.disconnect(true);
        delay(100);
      } else {
        WiFi.disconnect(false);
      }
    }
  }

  // O WiFi.status() oscila: em bancada ele reportou desconectado por um instante
  // com a associação e o WebSocket saudáveis, e o begin() disparado por esse
  // piscar derrubava os dois de uma vez — no log, "WiFi begin" e "WS desconectado"
  // no MESMO milissegundo, sem nenhum timeout antes. Só agimos depois que o status
  // se mantiver desconectado por WIFI_DOWN_CONFIRM_MS.
  bool staUp = (WiFi.status() == WL_CONNECTED);
  if (staUp) {
    if (wifiDownSinceMs != 0) {
      uint32_t d = millis() - wifiDownSinceMs;
      if (d > 200) Serial.printf("WiFi: oscilacao de %lums absorvida (nao reconectou)\n",
                                 (unsigned long)d);
      wifiDownSinceMs = 0;
    }
  } else if (wifiDownSinceMs == 0) {
    wifiDownSinceMs = millis();
  }

  if (!staUp && !wifiConnecting && hasSavedWiFi()
      && (millis() - wifiDownSinceMs) >= WIFI_DOWN_CONFIRM_MS) {
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
      // Rebobina o relógio do downtime até a última prova de vida. A linha lá em
      // cima acabou de empurrar wsLastOkMs para agora, mas na prática a conexão
      // estava morta desde lastPingMs — sem isso, um zumbi de 5min "zeraria" o
      // downtime e adiaria o auto-restart em outros 15min.
      wsLastOkMs = lastPingMs;
      webSocket.disconnect();
      isWebSocketConnected = false;
    }
  } else {
    // Fora do ar: espaça as tentativas conforme o tempo de queda. wsLastOkMs é
    // atualizado acima enquanto conectado, então a diferença é o downtime real.
    applyWsBackoff(millis() - wsLastOkMs);
  }

  // Reset pedido pelo callback (handshakes falhando em série). Executado aqui,
  // fora do callback, para não derrubar o socket de dentro do próprio evento.
  if (wsResetRequested) {
    wsResetRequested = false;
    if (hardResetPermitido()) {
      fullReconnectWiFiWS();
      return;
    }
    Serial.println("WS: reset da pilha adiado (intervalo minimo).");
  }

  // A partir daqui quem retenta é a lib, no ritmo do backoff. Este bloco só
  // garante que o begin() rodou uma vez com WiFi de pé — antes existia aqui uma
  // segunda camada de retentativa que disputava com a da lib e, de 10 em 10s,
  // abortava um handshake que já estava em andamento.
  if (WiFi.status() == WL_CONNECTED && !wsBegun) connectToWebSocket();
}

void watchdogTick() {
  if (WiFi.status() == WL_CONNECTED || isWebSocketConnected)
    lastConnectivityOkMs = millis();

  if (!apEnabled) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool wsOk   = isWebSocketConnected;

    // Escalonamento, corrigido em cima de medição de bancada. A versão anterior
    // reiniciava só o WebSocket na primeira janela — e isso comprovadamente NÃO
    // resolve: 7 tentativas seguidas falharam, e a conexão só voltou (em 2s) quando
    // o passo seguinte derrubou a pilha de WiFi. O WebSocket não era o problema;
    // ele está em cima de uma pilha travada que se declara conectada.
    //
    // Então a primeira ação passa a ser o reset da pilha — que, com uma única rede
    // salva, é exatamente o que fullReconnectWiFiWS() faz, sem trocar de rede
    // nenhuma. Trocar de slot fica para a segunda janela, e só se houver ssid2.
    if (wifiOk && !wsOk) {
      if (wsDownSinceMs == 0) wsDownSinceMs = millis();
      if ((millis() - wsDownSinceMs) > WS_DOWN_RESET_MS) {
        wsDownSinceMs = millis();
        if (wsDownEscalation == 0) {
          Serial.println("WATCHDOG: WS down. Reset da pilha de WiFi.");
          wsDownEscalation = 1;
          evAdd(EV_WD_WS, 0);
          fullReconnectWiFiWS();
        } else {
          Serial.println("WATCHDOG: WS down persistente. Failover + fallback de potencia.");
          wsDownEscalation = 0;
          // Alterna a potência: se o problema for a placa não sustentar o TX (ou o
          // contrário, sinal fraco demais para a potência reduzida), o outro nível
          // resolve. Não grava na NVS — é tentativa, não configuração.
          txPowerFallback = !txPowerFallback;
          Serial.printf("TXPOWER: fallback %s -> alvo %.1f dBm\n",
                        txPowerFallback ? "ligado" : "desligado",
                        activeTxPower() / 4.0);
          evAdd(EV_WD_FAIL, (int16_t)wifiSlot);
          failoverReconnect();
        }
      }
    } else {
      wsDownSinceMs = 0;
      if (wsOk) wsDownEscalation = 0;
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
    // ESTE é o ponto que faltava. softAPdisconnect(true) é uma troca de modo
    // (AP_STA -> STA) e derruba a potência de volta para 19,5 dBm. Era a única
    // troca de modo do firmware que não reaplicava o limite: a peça saía do
    // minuto 10 transmitindo em potência cheia, que é justamente o que este
    // lote de placas não sustenta (docs/LAUDO-LOTE-ESP32C3.md).
    applyTxPower();
    apEnabled = false;
    lastConnectivityOkMs = millis();
    if (wifiTestActive) {
      wifiTestActive = false;
      wifiTestState  = TST_IDLE;
      wifiTestDoneMs = 0;
      WiFi.disconnect(false);
      if (hasSavedWiFi()) connectToWiFi_begin();
    }
  }
}

void wsRestartTick() {
  if (isWebSocketConnected) { wsLastOkMs = millis(); return; }
  // Peça sem rede configurada não tem para onde reconectar: reiniciar de 30 em
  // 15min não resolve nada e só atrapalha quem estiver configurando. Idem
  // durante um teste do wizard.
  if (!hasSavedWiFi() || wifiTestActive) { wsLastOkMs = millis(); return; }
  // Só adia o reinício enquanto o pulso está no ar — sem empurrar wsLastOkMs,
  // que agora também é o relógio do backoff. Empurrá-lo aqui zeraria o downtime
  // e jogaria o intervalo de reconexão de volta para 1s.
  if (pulseActive) return;
  if ((millis() - wsLastOkMs) > (WS_RESTART_TIMEOUT_MS + wsRestartJitterMs)) {
    Serial.printf("WS_RESTART: sem WS por %lumin. Reiniciando.\n",
                  (unsigned long)((WS_RESTART_TIMEOUT_MS + wsRestartJitterMs) / 60000UL));
    delay(200);
    ESP.restart();
  }
}

// ================= AUTENTICAÇÃO DO PAINEL =================
// Login com página HTML própria (visual igual ao resto do painel) em vez do
// popup nativo do navegador (HTTP Basic Auth). Sessão via cookie: um token
// gerado no boot, guardado só em RAM — reinício da peça derruba a sessão de
// todo mundo, o que é o comportamento desejado (evita ter que gravar/expirar
// sessão na NVS). Senha de fábrica fixa no firmware, de propósito: sem tela de
// "trocar senha" — perder a senha numa peça em campo, sem cabo USB para
// recuperar, é pior do que a senha ser sempre a mesma de fábrica.
const char* PANEL_USER = "admin";
const char* PANEL_PASS = "Foreasy@12345678";
char sessionToken[17];

// Mistura (xorshift) do ID de fábrica do chip (único por peça) com millis()/
// micros() (variam por boot). Não é esp_random(): aqui no boot o rádio ainda
// não subiu e, sem WiFi/BT ativo, o gerador de hardware não é verdadeiramente
// aleatório — mesmo motivo pelo qual o jitter do auto-restart usa o MAC em vez
// de esp_random() (ver setup()).
void generateSessionToken() {
  uint64_t seed = ESP.getEfuseMac();
  seed ^= ((uint64_t)millis() << 16) ^ (uint64_t)micros();
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    sessionToken[i] = hex[seed & 0xF];
  }
  sessionToken[16] = '\0';
}

bool checkAuth() {
  if (server.hasHeader("Cookie") &&
      server.header("Cookie").indexOf(String("session=") + sessionToken) != -1) {
    return true;
  }
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
  return false;
}

void handleLoginPage() {
  bool erro = server.hasArg("erro");
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — Login</title>
<style>
:root{--bg:#070b08;--cd:#0f1612;--bd:#1e3028;--ac:#00e676;--tx:#d4f5e0;--mu:#557060;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(120% 80% at 50% -10%,#0d1a13 0%,var(--bg) 60%);color:var(--tx);font-family:ui-monospace,'SF Mono',monospace;font-size:14px;min-height:100vh;display:flex;align-items:center;justify-content:center}
.box{background:var(--cd);border:1px solid var(--bd);border-radius:10px;padding:32px;width:min(300px,88vw)}
.logo{color:var(--ac);font-size:22px;font-weight:700;letter-spacing:4px;text-align:center;margin-bottom:4px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;text-align:center;margin-bottom:22px}
label{display:block;color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:5px}
input{width:100%;background:#060a07;border:1px solid var(--bd);border-radius:6px;color:var(--tx);padding:9px 10px;font:inherit;margin-bottom:14px}
input:focus{outline:none;border-color:var(--ac)}
.pwrap{position:relative;margin-bottom:14px}
.pwrap input{margin-bottom:0;padding-right:38px}
.pwtoggle{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:30px;height:30px;display:flex;align-items:center;justify-content:center;background:none;border:0;padding:0;color:var(--mu);cursor:pointer;font-size:15px;line-height:1}
.pwtoggle:hover{color:var(--ac)}
button{width:100%;background:var(--ac);color:#04150b;border:0;border-radius:6px;padding:10px;font:inherit;font-weight:700;cursor:pointer;letter-spacing:1px}
button:hover{opacity:.9}
.err{color:var(--red);font-size:11px;text-align:center;margin-bottom:14px}
</style></head><body>
<form class="box" method="POST" action="/login">
  <div class="logo">FOREASY</div>
  <div class="sub">acesso ao painel</div>
)rawliteral";
  if (erro) html += "  <div class=\"err\">usuário ou senha incorretos</div>\n";
  html += R"rawliteral(  <label>Usuário</label>
  <input type="text" name="user" autocomplete="username" autofocus>
  <label>Senha</label>
  <div class="pwrap">
    <input type="password" name="pass" id="pass" autocomplete="current-password">
    <button type="button" class="pwtoggle" id="pwtoggle" tabindex="-1" aria-label="Mostrar senha">&#128065;</button>
  </div>
  <button type="submit">Entrar</button>
</form>
<script>
document.getElementById('pwtoggle').onclick=function(){
  var p=document.getElementById('pass');
  var show=p.type==='password';
  p.type=show?'text':'password';
  this.innerHTML=show?'&#128584;':'&#128065;';
};
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleLoginSubmit() {
  if (server.arg("user") == PANEL_USER && server.arg("pass") == PANEL_PASS) {
    server.sendHeader("Set-Cookie", String("session=") + sessionToken + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    server.sendHeader("Location", "/login?erro=1");
    server.send(302, "text/plain", "");
  }
}

void handleLogout() {
  server.sendHeader("Set-Cookie", "session=deslogado; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

// ================= HTTP ROUTES =================
void startWebServer() {
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  server.on("/login",       HTTP_GET,  handleLoginPage);
  server.on("/login",       HTTP_POST, handleLoginSubmit);
  server.on("/logout",      HTTP_GET,  handleLogout);
  server.on("/",            [](){ if (checkAuth()) handleRoot(); });            // landing: Wizard / Administrador
  server.on("/wizard",      [](){ if (checkAuth()) handleConfigPage(); });      // assistente passo a passo
  server.on("/config",      [](){ if (checkAuth()) handleConfigPage(); });      // alias (compat)
  server.on("/admin",       [](){ if (checkAuth()) handleAdminPage(); });       // edições pontuais
  server.on("/config-data", HTTP_GET,  [](){ if (checkAuth()) handleConfigData(); });
  server.on("/info",        [](){ if (checkAuth()) handleInfoPage(); });
  server.on("/scan",        HTTP_GET,  [](){ if (checkAuth()) handleScan(); });
  server.on("/save",        HTTP_POST, [](){ if (checkAuth()) handleSave(); });
  server.on("/status",      HTTP_GET,  [](){ if (checkAuth()) handleStatusJson(); });
  server.on("/log",         HTTP_GET,  [](){ if (checkAuth()) handleLog(); });
  server.on("/log/clear",   HTTP_GET,  [](){ if (checkAuth()) handleLogClear(); });
  server.on("/test-wifi",        HTTP_GET, [](){ if (checkAuth()) handleTestWifi(); });
  server.on("/test-wifi-status", HTTP_GET, [](){ if (checkAuth()) handleTestWifiStatus(); });
  server.on("/test-ws",          HTTP_GET, [](){ if (checkAuth()) handleTestWs(); });
  server.on("/resetwifi",   HTTP_GET,  [](){ if (checkAuth()) handleResetWiFi(); });
  server.on("/restart",     HTTP_GET,  [](){ if (checkAuth()) handleRestart(); });
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
</style></head><body>
<header><div class="logo">FOREASY</div><div class="sub">assistente de configuração · esp32-s3</div></header>
<main>
  <div class="steps">
    <div class="pill cur" id="pill0"><b>1</b><span>Rede 1</span></div>
    <div class="pill" id="pill1"><b>2</b><span>Rede 2</span></div>
    <div class="pill" id="pill2"><b>3</b><span>Servidor</span></div>
    <div class="pill" id="pill3"><b>4</b><span>Opções</span></div>
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
      <div class="hint">O servidor vem configurado de fábrica (alterável só no Administrador). O teste confirma o prédio/máquina deste Node ID.</div>
      <button class="btn tbtn" id="t2">Testar WebSocket</button>
      <div class="ts" id="ts2">Testa a conexão com o servidor.</div>
    </div>

    <div class="step" id="step3">
      <div class="sec">Opções — Industrial</div>
      <div class="chk"><input id="availEn" type="checkbox"><label for="availEn">Fail-safe AVAIL — confirma o ciclo e repulsa se a máquina não ligar</label></div>
      <label>Potência de transmissão do rádio</label>
      <select id="txpower">
        <option value="78">19,5 dBm — máximo (padrão)</option>
        <option value="68">17 dBm</option>
        <option value="60">15 dBm — alimentação fraca</option>
        <option value="52">13 dBm</option>
        <option value="44">11 dBm</option>
        <option value="34">8,5 dBm</option>
      </select>
      <div class="hint">Esta placa opera em <b>potência cheia</b> por padrão. Só reduza se a peça <b>associar a 15 dBm e falhar a 19,5</b> — sintoma de fonte ou cabo USB que não sustenta o pico do transmissor.</div>
      <div class="hint">Reinício automático após 15min sem WebSocket: <b>sempre ativo</b>.</div>
      <div class="hint"><b>Modelos sem AVAIL: deixe o fail-safe desmarcado.</b> Os pinos (START IN <b>GPIO5</b> / AVAIL OUT <b>GPIO6</b>) e o LED de status usam os padrões desta placa e só são ajustados no Administrador.</div>
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
    '&availEn='+(qs('availEn').checked?1:0)+
    '&txpower='+qs('txpower').value+
    '&wizard=1';
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
    qs('availEn').checked=(d.availEn===1);
    // txpower 0 = nunca configurado: 19,5 e o padrao real desta placa.
    qs('txpower').value = d.txpower ? String(d.txpower) : '78';
  }).catch(()=>{});
  scan();
  qs('t0').onclick=()=>{net1ok=false;testWifi(qs('manual_ssid').value.trim()||qs('ssid').value,qs('pass').value,'ts0',(ok)=>{net1ok=ok;});};
  qs('t1').onclick=()=>{testWifi(qs('ssid2').value.trim()||qs('ssid2_scan').value,qs('pass2').value,'ts1',()=>{});};
  qs('t2').onclick=()=>{wsdone=false;testWs();};
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
<header><div class="logo">FOREASY</div><div class="sub">administrador · esp32-s3</div></header>
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
    <div style="color:var(--mu);font-size:10px;line-height:1.5;margin-top:8px">Livres nesta placa: <b>0–18, 21 e 38–48</b>. A peça recusa 19/20 (USB) e 22–37 (flash/PSRAM). Evite 0, 3, 45 e 46 (strapping de boot) e 43/44 (serial).</div>
    <button class="btn" id="bPin">Salvar pinos</button>
  </div>
  <div class="box">
    <div class="sec">LED de status</div>
    <label>GPIO do LED</label><input id="ledPin" type="number" min="0" max="48">
    <label>Tipo</label>
    <select id="ledMode">
      <option value="3">RGB WS2812 embutido (padrão)</option>
      <option value="1">LED comum — ativo HIGH</option>
      <option value="2">LED comum — ativo LOW</option>
      <option value="0">Desligado</option>
    </select>
    <div style="color:var(--mu);font-size:10px;line-height:1.5;margin-top:8px">Verde = WebSocket conectado, vermelho = sem conexão. O DevKit traz o RGB no <b>GPIO48</b>; algumas revisões usam o <b>38</b>.</div>
    <button class="btn" id="bLed">Salvar LED</button>
  </div>
  <div class="box">
    <div class="sec">Avançado</div>
    <label>Potência de transmissão do rádio</label>
    <select id="txpower">
      <option value="78">19,5 dBm — máximo (padrão)</option>
      <option value="68">17 dBm</option>
      <option value="60">15 dBm — alimentação fraca</option>
      <option value="52">13 dBm</option>
      <option value="44">11 dBm</option>
      <option value="34">8,5 dBm</option>
    </select>
    <div style="color:var(--mu);font-size:10px;line-height:1.5;margin:8px 0 0">Esta placa opera em <b>19,5 dBm</b> (potência cheia) por padrão — diferente do C3, cujo lote ruim exigia nascer em 15. Só reduza onde a alimentação não sustenta o pico do transmissor: o sintoma é a peça <b>associar a 15 dBm e falhar a 19,5</b>. Reduzir encurta o alcance de subida em ~4,5 dB sem alterar o RSSI, que mede a descida.</div>
    <button class="btn" id="bAdv">Salvar potência</button>
    <div style="color:var(--mu);font-size:10px;line-height:1.5;margin:10px 0">Auto-restart após 15min sem WebSocket: <b>sempre ativo</b> (não é mais configurável).</div>
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
    qs('availEn').checked=(d.availEn===1);
    qs('ledPin').value=d.ledPin!=null?d.ledPin:48; qs('ledMode').value=String(d.ledMode!=null?d.ledMode:3);
    qs('txpower').value = d.txpower ? String(d.txpower) : '78';   // 0 = nunca configurado
  }).catch(()=>{});
  scan();
  qs('bNode').onclick=()=>{if(!val('nodeid')){msg('Preencha o Node ID');return;}save({nodeid:val('nodeid')});};
  qs('bSrv').onclick=()=>{if(!val('host')){msg('Preencha o host');return;}save({host:val('host'),port:val('port')||80});};
  qs('bN1').onclick=()=>{let s=val('m1')||val('ssid');if(!s){msg('Escolha a rede 1');return;}save({ssid:s,pass:qs('p1').value});};
  qs('bN2').onclick=()=>{save({ssid2:(val('m2')||val('ssid2')),pass2:qs('p2').value});};
  qs('bPin').onclick=()=>{save({startPin:val('startPin')||5,availPin:val('availPin')||6,availEn:(qs('availEn').checked?1:0)});};
  qs('bLed').onclick=()=>{save({ledPin:val('ledPin')||48,ledMode:qs('ledMode').value});};
  qs('bAdv').onclick=()=>{save({txpower:qs('txpower').value});};
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
  json += "\"ledPin\":"      + String(ledPin) + ",";
  json += "\"ledMode\":"     + String(ledMode) + ",";
  // Valor GRAVADO, não o efetivo: 0 significa "nunca configurado", e é isso que
  // permite às telas pré-selecionarem 19,5 enquanto a peça roda no fail-safe de 15.
  json += "\"txpower\":"     + String(prefs.getInt("txpower", 0));
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
  // Pino fora da lista permitida e RECUSADO, nao "aproximado" por constrain: o
  // GPIO valido mais proximo de um invalido continua sendo o pino errado, e aqui
  // o engano se paga com uma peca que nao da mais boot - conserto presencial, com
  // cabo, porque nem OTA tem mais como chegar nela.
  bool pinoRuim = false;
  if (server.hasArg("startPin")) {
    int p = server.arg("startPin").toInt();
    if (gpioLivre(p)) { startPin = p; prefs.putInt("startPin", p); any = true; }
    else pinoRuim = true;
  }
  if (server.hasArg("availPin")) {
    int p = server.arg("availPin").toInt();
    if (gpioLivre(p)) { availPin = p; prefs.putInt("availPin", p); any = true; }
    else pinoRuim = true;
  }
  if (server.hasArg("ledPin")) {
    int p = server.arg("ledPin").toInt();
    if (gpioLivre(p)) { ledPin = p; prefs.putInt("ledPin", p); any = true; }
    else pinoRuim = true;
  }
  if (server.hasArg("ledMode")) { ledMode = (uint8_t)constrain(server.arg("ledMode").toInt(), 0, 3); prefs.putInt("ledMode", ledMode); any = true; }
  if (server.hasArg("availEn"))  { availEnabled = server.arg("availEn").toInt() == 1; prefs.putInt("availEn", availEnabled ? 1 : 0); any = true; }
  // txpower em quartos de dBm (unidade do enum wifi_power_t). O constrain evita
  // que um valor absurdo vindo da URL desligue o rádio da peça em campo.
  if (server.hasArg("txpower"))  { int q = constrain(server.arg("txpower").toInt(), TXPOWER_MIN_Q, TXPOWER_MAX_Q); txPower = (wifi_power_t)q; prefs.putInt("txpower", q); any = true; }
  // "wsrestart" saiu de cena: o auto-restart passou a ser incondicional. A chave
  // antiga na NVS é simplesmente ignorada, então peças já configuradas não
  // precisam ser mexidas — inclusive as que tinham o valor salvo como 0.

  if (server.hasArg("ssid") || server.hasArg("ssid2")) wifiSlot = 0;

  // Com pino invalido a peca NAO reinicia, mesmo tendo gravado os outros campos:
  // reiniciar aqui tiraria a peca do ar em cima de uma configuracao que o
  // instalador ainda vai corrigir. O que ele salvou de valido ja esta na NVS e
  // entra em vigor no proximo /save.
  if (pinoRuim) {
    server.send(200, "text/plain",
                "GPIO invalido nesta placa. Livres: 0-18, 21 e 38-48 "
                "(19/20 = USB, 22-37 = flash/PSRAM).");
    return;
  }

  // "wizard=1" só vem do passo final do assistente (não do /admin). Zera o
  // contador ali para a fidelidade da telemetria: reinícios do bench/teste
  // durante a configuração não devem aparecer como reinícios em campo.
  if (server.hasArg("wizard")) { bootCount = 0; prefs.putUInt("bootCount", bootCount); any = true; }

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
  // Leitura de volta do rádio (não o valor pedido) — é como se confere em campo
  // se o limite de potência está mesmo valendo. -1 = rádio ainda não iniciado.
  json += "\"txp\":"          + String(radioStarted() ? WiFi.getTxPower() / 4.0 : -1.0, 1) + ",";
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
  json += "\"ledPin\":"       + String(ledPin) + ",";
  json += "\"ledMode\":"      + String(ledMode) + ",";
  json += "\"creditState\":"  + String((int)creditState) + ",";
  json += "\"wifiSlot\":"     + String(wifiSlot) + ",";
  json += "\"rst\":\""        + String(resetReasonStr()) + "\",";
  // Caixa-preta no /status: o /info já despeja este JSON inteiro no bloco final,
  // então quem está na frente da peça lê o histórico ANTES de reiniciar — que é
  // exatamente o momento em que ele seria perdido se dependesse só do envio.
  json += "\"blackbox\":"     + evJson() + ",";
  json += "\"boots\":"        + String(bootCount);
  json += "}";
  server.send(200, "application/json", json);
}

// ========== /info ==========
// Devolve o ring buffer de log em texto puro — é o que /info mostra no painel
// de baixo, para inspecionar a serial sem cabo enquanto a peça está com problema.
void handleLog() {
  server.send(200, "text/plain; charset=utf-8", logRingRead());
}

// Zera o buffer de verdade (não é só limpar a tela no navegador) — útil para
// isolar um teste: zera, reproduz o problema, olha só o que aconteceu depois.
void handleLogClear() {
  logRingHead = 0;
  logRingWrapped = false;
  server.send(200, "text/plain", "OK");
}

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
<header><div class="logo">FOREASY</div><div class="sub">esp32-s3 · status — atualiza a cada 2s</div></header>
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
    <a href="/logout">sair</a>
  </div>
  <div class="it" style="margin-top:14px">
    <div class="lb" style="display:flex;justify-content:space-between;align-items:center">
      <span>Serial (ao vivo — buffer em RAM, some no reboot)</span>
      <span style="display:flex;gap:12px">
        <a id="logPause" style="cursor:pointer;color:var(--mu)">pausar</a>
        <a id="logClear" style="cursor:pointer;color:var(--mu)">zerar buffer</a>
      </span>
    </div>
    <pre id="log" style="max-height:320px">…</pre>
  </div>
  <pre id="raw"></pre>
</main>
<script>
function g(id){return document.getElementById(id);}
let logPaused = false;
function updLog(){
  if (logPaused) return;
  fetch('/log').then(r=>r.text()).then(t=>{
    const el = g('log');
    const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 10;
    el.textContent = t || '(vazio)';
    if (atBottom) el.scrollTop = el.scrollHeight;
  }).catch(()=>{});
}
g('logPause').onclick=()=>{logPaused=!logPaused; g('logPause').textContent = logPaused?'retomar':'pausar';};
g('logClear').onclick=()=>{fetch('/log/clear').then(updLog);};
setInterval(updLog,2000); updLog();
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
