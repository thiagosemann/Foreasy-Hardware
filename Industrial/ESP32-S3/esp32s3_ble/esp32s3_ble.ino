// ============================================================================
// Foreasy ESP32-S3 - Industrial - CONFIGURACAO POR BLE
// Hardware: ESP32-S3 DevKit + saida START IN
//   Em uso: N8R2 (8MB flash / 2MB PSRAM). Previsto: N16R8 (16MB / 8MB).
//   A mesma imagem serve as duas - o que muda sao duas opcoes de build (ver BUILD).
//
// O QUE MUDA EM RELACAO AO esp32s3.ino (ao lado):
//   Este firmware NAO tem modo AP nem servidor web. A configuracao em campo e
//   feita pelo app Foreasy Instalador (repo Foreasy-App-Instalador), por BLE, e
//   o acompanhamento da peca ja em operacao e pelo painel do servidor, via a
//   telemetria do WebSocket - nao ha mais nenhuma pagina HTML na peca.
//
//   Motivo: o radio Wi-Fi e unico. Subir o softAP obriga o modo WIFI_AP_STA, e e
//   dai que nasce boa parte da instabilidade ja investigada na frota - potencia
//   de TX voltando ao maximo a cada troca de modo, e conflito de canal entre o AP
//   e o roteador (AP num canal e rede alvo em outro trava a associacao do STA).
//   Com BLE a peca fica em WIFI_STA puro a vida inteira.
//
//   Removido junto: wizard, /admin, /info, /scan de redes, /save, testes ao vivo
//   de Wi-Fi/WS, /log HTTP e a sessao por cookie. O ring de log em RAM ficou -
//   quem o le agora e o WS 0x07, nao mais o /log.
//
// Porte do firmware ESP32-C3 (Industrial/ESP32-C3/esp32c3/esp32c3.ino). A logica
// de negocio e o protocolo WS sao os MESMOS - o que muda e o que e especifico do
// chip: pinos, LED, validacao de GPIO, USB CDC e a politica de potencia de radio.
// Ao corrigir um bug de logica, corrija nos dois (o C3 ainda usa AP).
//
// POR QUE ESTA PLACA: o lote de ESP32-C3 Super Mini do segundo fornecedor nao
// sustenta o pico de corrente do TX em potencia cheia - a peca enxerga a rede mas
// nao associa e nem sobe o proprio AP (docs/LAUDO-LOTE-ESP32C3.md). O S3 DevKit
// tem regulador e desacoplamento dimensionados para o modulo, entao aqui a peca
// opera em 19,5 dBm por padrao.
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
// - 0x07 => Pede o buffer de log da serial (ring de RAM; unica forma de le-lo agora).
//           Resposta: JSON {"type":"log","data":"<texto escapado>"}.
//
// DETECCAO DE FICHA (token) - so com availEnabled e creditState IDLE:
// - "TokenInserted" : AVAIL LOW->HIGH externo (ficha) ou snapshot ocupado no connect
// - "TokenFinished" : AVAIL HIGH->LOW externo ou snapshot livre no connect
//   O backend cruza com is_in_use antes de marcar a ficha (nao toca is_in_use).
//
// WIFI:
// - Potencia de TX: sem controle nenhum, roda no padrao do core (19,5 dBm). O
//   aparato de ajuste/fallback existia por causa do lote ruim de C3, que nao
//   sustentava o pico do TX (docs/LAUDO-LOTE-ESP32C3.md); esta placa sustenta.
// - Dual WiFi com failover automatico entre rede 1 e rede 2 (sem restart)
// - Conexao nao-bloqueante: wifiTick() com timeout 40s e retry a cada 5s
// - Escada de associacao: 2 falhas -> disconnect(true) (reset de pilha). O degrau
//   de "derrubar o AP" saiu junto com o AP - aqui ja e STA puro desde o boot.
// - Credenciais NUNCA apagadas por falha de conexao
//
// WEBSOCKET:
// - Servidor (host/porta) na NVS (sem painel: so por regravacao)
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
// BLE: anuncia como FOREASY-<nodeId> depois de configurada e FOREASY-<4 hex do
//      MAC> enquanto virgem - e assim que a lista do Instalador mostra quais
//      pecas ja foram provisionadas sem conectar em cada uma. Toda escrita exige o segredo
//      compartilhado antes (ver PROVISION_SECRET). A janela SO ABRE em evento de
//      energia (ligar na tomada, botao RST, brownout) - reinicio por software,
//      OTA ou watchdog sobe sem BLE nenhum, porque configurar peca e coisa que
//      se faz com ela na mao. Sem isso, a peca offline (que se auto-reinicia a
//      cada ~15min) reabria a janela para sempre e ficava disputando antena com
//      o proprio Wi-Fi que tentava levantar. Dura 5 min contados da ultima
//      interacao com o app (nao do boot: quem trabalhou 20 min na peca precisa
//      conseguir reabrir o app depois). Unica peca que mantem o BLE alem da
//      janela e a que nao tem Wi-Fi salvo - nela o radio Wi-Fi esta ocioso e nao
//      ha nada a disputar. Fechar faz deinit do controlador, devolvendo tempo de
//      antena ao Wi-Fi (os dois radios dividem os 2,4 GHz). Para configurar uma
//      peca ja instalada: tirar da tomada e ligar de novo.
//
// ARMAZENAMENTO: Preferences (NVS) - ssid, pass, ssid2, pass2, nodeid,
//                wsHost, wsPort, startPin, availPin, availEn, ledPin, ledMode,
//                bootCount, evlog
//
// TEMPERATURA: sensor interno do ESP32-S3 via temperatureRead()
//
// ---------------------------------------------------------------------------
// PINOS (na NVS; sem painel, so por regravacao) - padroes desta placa:
// - startPin : GPIO5  (START IN, ativo HIGH)
// - availPin : GPIO6  (AVAIL OUT, INPUT_PULLUP)
// - ledPin   : GPIO48 em modo RGB (LED WS2812 embutido do DevKit)
//
// GPIOs PROIBIDOS no S3 - gpioLivre() recusa no loadPrefs(), entao um valor
// invalido gravado nao derruba a peca em campo:
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
//     Industrial/ESP32-S3/esp32s3_ble
//
// A PARTICAO PRECISA DE DOIS SLOTS DE APP, senao nao existe OTA 0x04: "Huge APP",
// "Minimal" e todos os "No OTA" tem um slot so, e gravar com um deles deixa a
// peca alcancavel apenas por cabo. O padrao da IDE ("Default 4MB with spiffs")
// tem os dois slots e roda em qualquer flash >= 4MB, mas sao 1,2MB por slot - e a
// pilha BLE sozinha pesa ~700KB, entao aqui nem cabe. Use os esquemas de 8M/16M
// acima, que tem folga de sobra.
// ============================================================================
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>   // esp_ble_gap_set_device_name: renomeia sem reiniciar
#endif
#include <Update.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>   // esp_wifi_scan_stop: aborta scan travado no driver

// ---------- Log em RAM (espelho da serial, sem cabo USB) ----------
// Tudo que vai para Serial.print/println/printf cai também aqui — não persiste
// no reboot nem grava em flash, é só um ring buffer lido pelo WS 0x07 enquanto
// a peça está com problema, sem precisar de cabo. Se ela reiniciar
// (crash/watchdog), o buffer some junto — para isso já existe o campo "rst" na
// telemetria (motivo do reset), que sobrevive porque vai para o backend.
//
// A retenção é por TAMANHO, não por tempo: ficam as últimas N letras de log,
// seja isso uma semana de peça saudável ou dez minutos de peça em crise. Foi
// dimensionado pelo segundo caso, que é quando alguém vai ler.
//
// 16KB ≈ 240 linhas (a linha típica daqui tem ~68 bytes com o carimbo de hora).
// Com os 4KB anteriores cabiam ~60, e um único storm de reconexão já enchia o
// buffer e comia justamente o começo do incidente — a parte que explica a causa.
// O custo são 16KB de RAM estática, contra >139KB de heap livre no pior momento
// (com o BLE ligado): irrelevante perto de perder o diagnóstico.
#define LOG_RING_SIZE 16384
char   logRingBuf[LOG_RING_SIZE];
size_t logRingHead = 0;
bool   logRingWrapped = false;

// Copia em bloco (no máximo dois memcpy: até o fim do buffer e o resto na
// volta). A versão anterior fazia byte a byte com um `%` — uma divisão por
// caractere de log, no caminho por onde passa TODA saída de Serial da peça.
void logRingPush(const uint8_t* data, size_t len) {
  if (len == 0) return;
  // Escrita maior que o buffer inteiro: só o final sobreviveria de qualquer
  // forma, então pula direto para ele em vez de dar voltas sobrescrevendo.
  if (len >= LOG_RING_SIZE) {
    data += len - LOG_RING_SIZE;
    len = LOG_RING_SIZE;
  }

  size_t ateOFim = LOG_RING_SIZE - logRingHead;
  size_t primeiro = (len < ateOFim) ? len : ateOFim;
  memcpy(logRingBuf + logRingHead, data, primeiro);

  if (len > primeiro) {
    memcpy(logRingBuf, data + primeiro, len - primeiro);
    logRingHead = len - primeiro;
    logRingWrapped = true;
  } else {
    logRingHead += primeiro;
    if (logRingHead == LOG_RING_SIZE) {
      logRingHead = 0;
      logRingWrapped = true;
    }
  }
}

// A peça não tem RTC — a noção de tempo no log vem do NTP assim que ela pega
// IP (ver ARDUINO_EVENT_WIFI_STA_GOT_IP em onWiFiEvent). time(nullptr) < ano
// 2020 é o sinal de "ainda não sincronizou" (o clock do ESP32 nasce zerado em
// 1970); nesse caso cai para tempo decorrido desde o boot, que sempre existe.
// Fuso fixo -3h (America/Sao_Paulo, sem horário de verão desde 2019) — mesma
// convenção já usada no backend (ver spNow() em src/websocket.js do FRST-BACK).
//
// Escreve direto no buffer do chamador (pilha) em vez de devolver String: o
// prefixo sai uma vez por LINHA de log, e a versão com String alocava três
// vezes no heap a cada linha ("[" + ts + "] " cria dois temporários além do
// próprio ts). Numa peça que fica meses ligada, é o suficiente para fragmentar
// o heap sem nunca vazar um byte.
size_t logTimestampInto(char* buf, size_t cap) {
  time_t now = time(nullptr);
  if (now > 1600000000) {  // 2020-09-13 — bem antes de qualquer boot real
    struct tm t;
    localtime_r(&now, &t);
    return snprintf(buf, cap, "[%02d:%02d:%02d] ", t.tm_hour, t.tm_min, t.tm_sec);
  }
  unsigned long s = millis() / 1000;
  return snprintf(buf, cap, "[+%02lu:%02lu:%02lu] ", s / 3600, (s / 60) % 60, s % 60);
}

// Conteúdo atual em ordem cronológica (mais antigo primeiro).
String logRingRead() {
  size_t start = logRingWrapped ? logRingHead : 0;
  size_t count = logRingWrapped ? LOG_RING_SIZE : logRingHead;
  String out;
  out.reserve(count + 1);
  // Dois concat de bloco (da cabeça até o fim, e do início até a cabeça) em vez
  // de 4096 concatenações de um caractere.
  if (start + count <= LOG_RING_SIZE) {
    out.concat(logRingBuf + start, count);
  } else {
    size_t ateOFim = LOG_RING_SIZE - start;
    out.concat(logRingBuf + start, ateOFim);
    out.concat(logRingBuf, count - ateOFim);
  }
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
  // true logo após um '\n' (ou no boot): a próxima escrita começa uma linha
  // nova no ring buffer, então ganha o prefixo de horário antes do conteúdo.
  bool atLineStart = true;
  // Empurra o texto em TRECHOS (do início da linha até o \n), não byte a byte:
  // antes era uma chamada de função por caractere. O prefixo de horário entra
  // uma vez por linha, num buffer de pilha.
  void pushTee(const uint8_t* buf, size_t size) {
    size_t ini = 0;
    for (size_t i = 0; i < size; i++) {
      if (atLineStart) {
        char ts[16];
        logRingPush((const uint8_t*)ts, logTimestampInto(ts, sizeof(ts)));
        atLineStart = false;
      }
      if (buf[i] == '\n') {
        logRingPush(buf + ini, i - ini + 1);
        ini = i + 1;
        atLineStart = true;
      }
    }
    if (ini < size) logRingPush(buf + ini, size - ini);
  }
public:
  void begin(unsigned long baud) { realSerial.begin(baud); }
  size_t write(uint8_t c) override { pushTee(&c, 1); return realSerial.write(c); }
  size_t write(const uint8_t* buf, size_t size) override { pushTee(buf, size); return realSerial.write(buf, size); }
};
TeeSerial teeSerial;
#define Serial teeSerial

// Major bumpado para 2.x: NAO e um incremento do 1.4.0 do esp32s3.ino ao lado.
// Uma peca nesta versao nao tem AP nem painel web, e so aceita configuracao pelo
// app Instalador. O campo "fw" da telemetria e o unico jeito de o servidor
// distinguir as duas na frota - deixar as duas como 1.4.0 tornaria impossivel
// saber, olhando o painel, se uma peca ainda pode ser configurada pelo AP, e um
// OTA mirado por versao poderia mandar a imagem errada para a peca errada.
#define FW_VERSION "2.1.0"   // reportado no 0x03 para auditoria da frota
#define FW_CHIP    "esp32s3" // identifica o chip na telemetria / seleção de OTA

// radioStarted(): a interface Wi-Fi ja reportou START? O bit e setado por
// evento, de forma assincrona, entao logo apos um WiFi.mode() ele ainda pode
// estar falso. Sobrou como guarda de leitura da potencia no log/telemetria.
bool radioStarted() { return WiFi.STA.started(); }

float readInternalTempC() {
  return temperatureRead();
}

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

// Ultimo estado (+fase do pisca-pisca) desenhado, para nao reescrever o LED a
// cada volta do loop: o WS2812 e bit-bang com interrupcoes desabilitadas
// (~30us por atualizacao), e repetir isso milhares de vezes por segundo e
// desperdicio puro. A funcao que decide a cor (ledShow) fica mais abaixo,
// perto de onde wifiConnecting/isWebSocketConnected sao declaradas.
int lastLedKey = -1;

void ledSetup() {
  lastLedKey = -1;
  if (ledMode == LED_MODE_HIGH || ledMode == LED_MODE_LOW) {
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, ledMode == LED_MODE_LOW ? HIGH : LOW);   // comeca apagado
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
// Valor de fabrica do nodeId. Ele e o que separa peca configurada de peca
// virgem - e agora isso aparece no nome anunciado por BLE (ver
// bleNomeAnunciado), para o Instalador saber quais pecas ja foram provisionadas
// so de olhar a lista, sem precisar conectar em cada uma.
const char* NODEID_PADRAO = "FOREASY";
String nodeId = NODEID_PADRAO;

bool nodeIdConfigurado() { return nodeId.length() > 0 && nodeId != NODEID_PADRAO; }

// Sufixo do MAC de fábrica (2 bytes, 4 dígitos hex) — entra no nome anunciado
// no BLE para diferenciar peças ligadas lado a lado na bancada (todas nascem
// com o mesmo nodeId padrão, então sem isso o instalador não sabe em qual está
// mexendo). Vem do efuse via ESP.getEfuseMac(), não de esp_random(): mesma
// lógica do jitter do auto-restart abaixo, pelo mesmo motivo — o rádio ainda
// não subiu neste ponto do boot, e sem Wi-Fi/BT ativo o gerador não é
// verdadeiramente aleatório (risco real de a frota inteira sortear o mesmo
// valor). O MAC é único por placa e estável entre reinícios, o que também
// mantém o nome igual se precisar reconectar na mesma peça após um reboot.
String macSuffix() {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04X", (unsigned)(ESP.getEfuseMac() & 0xFFFF));
  return String(buf);
}

// ---------- Credenciais cacheadas (carregadas no boot) ----------
String sSsid, sPass, sSsid2, sPass2;
uint8_t wifiSlot = 0;  // 0=rede1, 1=rede2

const char* activeSSID() { return wifiSlot == 0 ? sSsid.c_str() : sSsid2.c_str(); }
const char* activePass() { return wifiSlot == 0 ? sPass.c_str() : sPass2.c_str(); }
bool hasSavedWiFi()      { return strlen(activeSSID()) > 0; }

// ---------- Recuperação de WiFi ----------
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
// Handshakes de WebSocket que morreram em sequência sem completar. O sintoma é
// inequívoco (TCP abre, o "HTTP/1.1 101" nunca chega) e a cura é conhecida: só o
// reset da pilha de WiFi resolve — reiniciar o WebSocket não adianta. Não faz
// sentido esperar o watchdog quando o diagnóstico já está fechado.
uint8_t  wsHandshakeFailStreak = 0;
const uint8_t WS_HANDSHAKE_FAIL_RESET = 5;

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
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    // Dispara e esquece — configTime() com hostname faz o SNTP sincronizar em
    // segundo plano; logTimestamp() sozinho detecta quando o relógio já
    // "pegou" (time(nullptr) > 2020) e troca de "+HH:MM:SS desde o boot" para
    // hora real. Não bloqueia o event handler nem falha se o NTP não responder.
    configTime(-3 * 3600, 0, "a.st1.ntp.br", "pool.ntp.org");
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
  ledPin           = prefs.getInt("ledPin",  ledPin);
  ledMode          = (uint8_t)constrain(prefs.getInt("ledMode", ledMode), 0, 3);
  // 0 = chave ausente (nenhum valor valido e 0) -> potencia cheia, o padrao desta
  // placa. So opera reduzido quem salvou um valor menor pelo /admin.
  bootCount        = prefs.getUInt("bootCount",   0);
  // Pino invalido gravado (placa trocada, digitacao errada num /save antigo) volta
  // ao padrao em vez de virar peca sem boot.
  if (!gpioLivre(startPin)) startPin = 5;
  if (!gpioLivre(availPin)) availPin = 6;
  if (!gpioLivre(ledPin))   ledPin   = 48;
}

// ============================================================================
// BLE — provisionamento pelo app Foreasy Instalador
// ============================================================================
// GATT próprio e enxuto, não o esp-idf-provisioning da Espressif: aquele é
// protobuf sobre GATT, feito para os SDKs nativos deles, e pesa mais do que se
// paga aqui. Uma characteristic por campo, texto puro UTF-8.
//
// Espelha lib/ble.ts do Foreasy-App-Instalador — UUID trocado de um lado sem o
// outro faz a peça sumir do scan do app, sem erro nenhum. Mantenha os dois em
// sincronia. Mesmo serviço do Convencional/ESP32/esp32_ble.
#define BLE_SVC_UUID    "0000fe70-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_SSID    "0000fe71-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_PASS    "0000fe72-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_NODEID  "0000fe73-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_COMMAND "0000fe74-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_STATUS  "0000fe75-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_AUTH    "0000fe76-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_SSID2   "0000fe77-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_PASS2   "0000fe78-0000-1000-8000-00805f9b34fb"
// Write "en|startPin|availPin" — formato de pipe, não JSON, para o firmware não
// precisar de parser. Ex.: "1|5|6".
#define BLE_CHR_AVAIL   "0000fe79-0000-1000-8000-00805f9b34fb"
// Read+Notify: JSON com a configuração que está GRAVADA na peça, para o wizard
// abrir já preenchido em vez de em branco. Nunca inclui senha.
#define BLE_CHR_CONFIG  "0000fe7a-0000-1000-8000-00805f9b34fb"
// Read+Notify: JSON com as redes que a peça enxerga (comando "scan").
#define BLE_CHR_SCAN    "0000fe7b-0000-1000-8000-00805f9b34fb"

// Segredo compartilhado com o app. Fixo no firmware de propósito, como era a
// senha do painel web: sem tela de "trocar senha" — perder a senha numa peça em
// campo, sem cabo USB para recuperar, é pior do que ser sempre a de fábrica.
const char* PROVISION_SECRET = "Foreasy@12345678";

BLECharacteristic* chrStatus = nullptr;
BLECharacteristic* chrConfig = nullptr;
BLECharacteristic* chrScan   = nullptr;

// Recebidos por BLE, ainda não gravados. Só viram NVS quando chega o "apply" —
// assim uma escrita parcial (app fechou no meio) não deixa a peça com metade da
// configuração nova e metade da velha. String vazia = "não mexer neste campo",
// o que deixa o wizard pular a rede 2 sem apagar a que já estava salva.
String bleSsid, blePass, bleSsid2, blePass2, bleNodeId, bleAvailCfg;
bool bleAutenticado     = false;   // escreveu o segredo correto nesta conexão?
bool bleApplyRequested  = false;   // "apply" recebido, pendente de processar no loop
bool bleRebootRequested = false;
bool bleScanRequested   = false;   // "scan" recebido, pendente de processar no loop
bool bleProvisionando   = false;   // há um provisionamento em curso a reportar?
bool bleTestRequested   = false;   // "test" recebido, pendente de disparar
bool bleConectado       = false;   // há um app falando com a peça agora?
// Início da janela de instalação. Vale do boot, mas cada conexão do app o
// empurra para frente: quem trabalhou 20 minutos numa peça teria a janela
// vencida havia muito tempo e, ao desconectar, a peça sumiria na hora — sem
// chance de reabrir o app para conferir o que acabou de gravar, a não ser
// desligando a máquina da tomada. Não custa nada ao dia a dia: só se estende
// quando alguém realmente conectou.
uint32_t bleJanelaInicioMs = 0;
// Desde quando o app está conectado — usado para limitar por quanto tempo a
// presença dele pode segurar o auto-restart.
uint32_t bleConexaoDesdeMs = 0;

// Fases do scan de redes. Antes eram dois bools e nenhum tratamento de erro, e
// era dai que vinha a "selecao de Wi-Fi que as vezes nao funciona":
//
//   1. scanNetworks() devolve WIFI_SCAN_FAILED quando o esp_wifi_scan_start e
//      recusado, e ele E recusado enquanto o STA esta no meio de um
//      WiFi.begin() - que e o estado permanente de uma peca ainda sem rede
//      valida, ou seja, exatamente a peca que o instalador esta configurando.
//      O retorno era ignorado, o scanComplete() seguinte devolvia FAILED e o
//      app recebia lista vazia: "nenhuma rede", sem erro nenhum na tela.
//   2. Sem teto de tempo, um scan que nao terminasse deixava bleScanEmCurso
//      preso em true e TODO pedido seguinte era descartado em silencio - o
//      botao "buscar redes" parava de funcionar ate reiniciar a peca.
//
// A maquina abaixo resolve os dois: libera o radio antes de disparar, confere o
// retorno, tem teto de tempo por tentativa e tenta de novo antes de desistir.
enum BleScanFase : uint8_t {
  SCAN_OCIOSO,      // nada pendente
  SCAN_PREPARANDO,  // radio liberado, esperando o intervalo para disparar
  SCAN_RODANDO      // scan no driver, aguardando resultado
};
BleScanFase bleScanFase       = SCAN_OCIOSO;
uint32_t    bleScanTimerMs    = 0;   // inicio da fase atual
uint32_t    bleScanEsperaMs   = 0;   // espera desta fase PREPARANDO
uint32_t    bleScanRodadaFimMs = 0;  // teto ABSOLUTO da rodada inteira
uint8_t     bleScanTentativa  = 0;   // tentativas ja gastas nesta rodada

const uint8_t  BLE_SCAN_TENTATIVAS   = 3;
const uint32_t BLE_SCAN_PREP_MS      = 400;    // radio assentar apos abortar o begin()
const uint32_t BLE_SCAN_RETRY_MS     = 1500;   // espera entre tentativas
const uint32_t BLE_SCAN_TIMEOUT_MS   = 12000;  // teto por tentativa
// Menor que o padrao do core (300ms/canal): BLE e Wi-Fi dividem a mesma antena,
// e quanto mais tempo o scan segura o radio maior a chance de a conexao BLE com
// o app cair no meio da busca.
const uint32_t BLE_SCAN_MS_POR_CANAL = 200;
// Teto da rodada inteira, incluindo esperas. Existe para o resto do firmware
// nunca ficar refem do instalador: passado esse prazo, o scan deixa de segurar
// qualquer coisa, mesmo que a maquina de estados tenha parado no meio (o caso
// real e o BLE ser desligado durante um scan, o que congela bleScanFase porque
// o bleTick para de rodar).
const uint32_t BLE_SCAN_RODADA_MAX_MS = 45000;

/**
 * O scan esta segurando o rádio AGORA? É o que o wifiTick consulta antes de
 * reconectar, e por isso é obrigatoriamente auto-curável: a resposta volta a ser
 * "não" no prazo, aconteça o que acontecer com a máquina de estados. O uso
 * diário da peça não pode depender de o app do instalador ter se comportado.
 */
bool bleScanEmAndamento() {
  if (bleScanFase == SCAN_OCIOSO) return false;
  return (int32_t)(millis() - bleScanRodadaFimMs) < 0;
}

// Intervalo de anúncio, em unidades de 0,625ms. Definido explicitamente porque
// o padrão da pilha é 30-60ms — o mais agressivo que existe — e isso gasta em
// torno de 10% do tempo de antena com BLE justamente no boot, que é quando o
// Wi-Fi precisa associar e subir o WebSocket. Os dois rádios dividem os mesmos
// 2,4 GHz, e foi disputa de antena que tirou o AP deste firmware; não faz
// sentido reintroduzir o mesmo custo pela porta do BLE. A 300ms o app continua
// listando a peça em menos de um segundo.
const uint16_t BLE_ADV_ITVL_INSTALACAO = 480;    // 300ms
const uint16_t BLE_ADV_ITVL_FOLGA      = 160;    // +100ms entre min e max

// Tempo de vida do BLE, contado do boot. BLE e Wi-Fi dividem a mesma antena de
// 2,4 GHz: anunciar para sempre custa tempo de rádio pelo resto da vida da peça
// em troca de uma janela que só interessa na instalação.
const uint32_t BLE_LIFETIME_MS = 5UL * 60UL * 1000UL;
// Teto de quanto tempo um app conectado pode segurar o auto-restart por falta de
// WebSocket. Cobre um wizard inteiro com folga; passado isso vale a recuperação
// da peça, que é o que interessa para o dia a dia.
const uint32_t BLE_SEGURA_RESTART_MS = 10UL * 60UL * 1000UL;
bool     bleAtivo   = true;
uint32_t bootTimeMs = 0;
// Teste em curso: conecta com as credenciais recebidas SEM gravar na NVS, para
// o instalador validar a rede antes de commitar. Enquanto vale, o wifiTick não
// pode reconectar sozinho com as credenciais salvas — atropelaria o teste.
bool bleTestando        = false;

/**
 * Publica o status para o app. Chamado tanto do callback do BLE quanto dos
 * ticks de Wi-Fi/WS — é só setValue + notify, seguro nos dois contextos.
 * Valores (contrato com o app): idle, connecting, connected, ws_ok,
 * bad_password, not_found, error.
 */
void bleSetStatus(const char* status) {
  if (!chrStatus) return;
  chrStatus->setValue((uint8_t*)status, strlen(status));
  chrStatus->notify();
  Serial.printf("BLE status: %s\n", status);
}

/** Escapa aspas e barras — nome de rede aceita praticamente qualquer caractere. */
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c < 0x20) continue;   // controle: descarta
    else out += c;
  }
  return out;
}

/**
 * O que está GRAVADO na peça, para o wizard abrir preenchido em vez de em
 * branco (o instalador confere o que já existe antes de trocar).
 *
 * Inclui as senhas de Wi-Fi. É uma decisão consciente: quem chega aqui já
 * passou pelo segredo compartilhado do BLE (e pelo login de admin/síndico no
 * app), e o instalador precisa ver a senha gravada para conferir se é a certa
 * — sem isso, uma rede que já funcionava teria de ser redigitada às cegas a
 * cada visita. A senha do painel/BLE em si nunca sai daqui.
 */
void blePublishConfig() {
  if (!chrConfig) return;
  String j;
  j.reserve(320);   // sem isto, cada += pode realocar e copiar de novo
  j = "{";
  j += "\"nodeId\":\""  + jsonEscape(nodeId) + "\",";
  j += "\"ssid\":\""    + jsonEscape(sSsid)  + "\",";
  j += "\"pass\":\""    + jsonEscape(sPass)  + "\",";
  j += "\"ssid2\":\""   + jsonEscape(sSsid2) + "\",";
  j += "\"pass2\":\""   + jsonEscape(sPass2) + "\",";
  j += "\"availEn\":"   + String(availEnabled ? 1 : 0) + ",";
  j += "\"startPin\":"  + String(startPin) + ",";
  j += "\"availPin\":"  + String(availPin) + ",";
  j += "\"fw\":\""      + String(FW_VERSION) + "\",";
  j += "\"chip\":\""    + String(FW_CHIP) + "\",";
  j += "\"wifiOk\":"    + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  j += "\"wsOk\":"      + String(isWebSocketConnected ? "true" : "false");
  j += "}";
  chrConfig->setValue((uint8_t*)j.c_str(), j.length());
  chrConfig->notify();
}

/**
 * Publica as redes encontradas. Limitado a BLE_SCAN_MAX e a campos curtos de
 * propósito: mesmo com MTU negociado em 512, um notify carrega ~509 bytes, e a
 * lista inteira precisa caber em UMA mensagem — o alternativo seria fatiar em
 * pedaços e remontar no app, complexidade que não se paga para escolher uma
 * rede.
 *
 * O corte NAO pode ser pelos primeiros n do driver, como era antes: o
 * comentario antigo dizia que o resultado vem ordenado por sinal e isso nao e
 * verdade - o esp_wifi_scan_get_ap_records devolve na ordem em que os APs
 * responderam. Num predio com muitas redes, a rede certa podia simplesmente nao
 * estar nos 12 primeiros, e o instalador via uma lista sem a rede que queria (e
 * nenhuma pista de por que). Por isso aqui ordena por RSSI e so entao corta.
 *
 * Tambem deduplica por SSID: mesh e repetidor anunciam o mesmo nome em varios
 * APs, o que enchia a lista de repeticoes (empurrando redes reais para fora do
 * corte) e ainda dava chave duplicada na lista do app.
 */
const int BLE_SCAN_MAX = 12;
// Teto do JSON em bytes. Um notify carrega MTU-3 (509 com MTU 512); a margem
// cobre celular que negocia um MTU um pouco menor sem truncar a lista no meio.
const size_t BLE_SCAN_BUDGET = 460;

struct RedeVista {
  String  ssid;
  int32_t rssi;
  bool    aberta;
};

void blePublishScan(int n) {
  if (!chrScan) return;

  // Insercao ordenada por RSSI, com dedupe por SSID, em vetor de tamanho fixo:
  // o que nao cabe e sempre o mais fraco.
  RedeVista lista[BLE_SCAN_MAX];
  int usados = 0;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;          // rede oculta: nada a mostrar
    int32_t rssi = WiFi.RSSI(i);
    bool aberta  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

    // Mesmo SSID ja visto: fica o de sinal melhor, e ele e reinserido para nao
    // ficar fora de ordem.
    int dup = -1;
    for (int k = 0; k < usados; k++) {
      if (lista[k].ssid == ssid) { dup = k; break; }
    }
    if (dup >= 0) {
      if (rssi <= lista[dup].rssi) continue;
      for (int k = dup; k < usados - 1; k++) lista[k] = lista[k + 1];
      usados--;
    }

    int pos = 0;
    while (pos < usados && lista[pos].rssi >= rssi) pos++;
    if (pos >= BLE_SCAN_MAX) continue;          // mais fraca que todas as que ja cabem
    int ultimo = (usados < BLE_SCAN_MAX) ? usados : (BLE_SCAN_MAX - 1);
    for (int k = ultimo; k > pos; k--) lista[k] = lista[k - 1];
    lista[pos].ssid   = ssid;
    lista[pos].rssi   = rssi;
    lista[pos].aberta = aberta;
    if (usados < BLE_SCAN_MAX) usados++;
  }

  String j;
  j.reserve(BLE_SCAN_BUDGET + 8);
  j = "[";
  int publicadas = 0;
  for (int k = 0; k < usados; k++) {
    String item = "{\"s\":\"" + jsonEscape(lista[k].ssid) + "\"";
    item += ",\"r\":" + String(lista[k].rssi);
    item += ",\"e\":" + String(lista[k].aberta ? 0 : 1);
    item += "}";
    // +2 = a virgula que separa e o ] do fim. Cortar por bytes (e nao so por
    // quantidade) e o que garante que o JSON sempre chega fechado no app: SSID
    // longo e comum e estourava o notify silenciosamente.
    if (j.length() + item.length() + 2 > BLE_SCAN_BUDGET) break;
    if (publicadas > 0) j += ",";
    j += item;
    publicadas++;
  }
  j += "]";

  chrScan->setValue((uint8_t*)j.c_str(), j.length());
  chrScan->notify();
  Serial.printf("BLE scan: %d brutas -> %d unicas -> %d publicadas (%u bytes)\n",
                n, usados, publicadas, j.length());
}

class ProvisionCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* chr) override {
    // Nada pesado aqui: isto roda na task da pilha BLE. Gravar NVS e mexer no
    // rádio Wi-Fi ficam para o bleTick(), no loop principal.
    String uuid  = chr->getUUID().toString().c_str();
    String valor = String(chr->getValue().c_str());

    if (uuid.equalsIgnoreCase(BLE_CHR_AUTH)) {
      bleAutenticado = valor.equals(PROVISION_SECRET);
      bleSetStatus(bleAutenticado ? "idle" : "error");
      return;
    }

    // Sem o segredo, nada é aceito — o BLE fica anunciando a vida toda (é o
    // único canal de configuração que sobrou), então não pode ser um portão
    // aberto para qualquer celular por perto reescrever a rede da peça.
    if (!bleAutenticado) { bleSetStatus("error"); return; }

    if      (uuid.equalsIgnoreCase(BLE_CHR_SSID))   bleSsid     = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_PASS))   blePass     = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_SSID2))  bleSsid2    = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_PASS2))  blePass2    = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_NODEID)) bleNodeId   = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_AVAIL))  bleAvailCfg = valor;
    else if (uuid.equalsIgnoreCase(BLE_CHR_COMMAND)) {
      if      (valor == "apply")  bleApplyRequested  = true;
      else if (valor == "reboot") bleRebootRequested = true;
      else if (valor == "scan")   bleScanRequested   = true;
      else if (valor == "test")   bleTestRequested   = true;
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* srv) override {
    bleConectado       = true;
    bleConexaoDesdeMs  = millis();
    bleJanelaInicioMs  = millis();
    Serial.println("BLE: app conectado.");
    // Republica: o wizard abre com o estado do momento (inclusive wifiOk/wsOk),
    // não com o snapshot congelado do boot.
    blePublishConfig();
  }
  void onDisconnect(BLEServer* srv) override {
    // Autenticação vale por conexão: o próximo celular precisa se identificar
    // de novo. Volta a anunciar, senão a peça fica invisível após a primeira
    // configuração e só um reboot a traz de volta.
    bleConectado      = false;
    bleAutenticado    = false;
    bleProvisionando  = false;
    // Reinicia a contagem da janela: a peça continua alcançável por mais alguns
    // minutos depois que o instalador fecha o app.
    bleJanelaInicioMs = millis();
    if (bleAtivo) {
      Serial.println("BLE: app desconectado. Voltando a anunciar.");
      BLEDevice::startAdvertising();
    }
  }
};

/**
 * Desliga o BLE depois da janela de instalação. Não é só parar de anunciar: o
 * deinit libera o controlador, que é o que de fato devolve tempo de antena ao
 * Wi-Fi (BLE e Wi-Fi dividem o mesmo rádio de 2,4 GHz).
 *
 * Duas condições antes de desligar, e as duas importam:
 *
 * 1. Nenhum app conectado. Cortar no minuto 5 com o instalador no meio do
 *    wizard seria pior do que qualquer ganho de antena.
 * 2. A peça precisa estar online (Wi-Fi + WebSocket). Se ela ainda não
 *    conectou, não há disputa de antena a proteger — e é exatamente a peça que
 *    mais precisa do BLE, senão vira caixa-preta e só volta com cabo. Desligar
 *    aí seria pagar o custo alto sem receber o benefício.
 */
void bleLifetimeTick() {
  if (!bleAtivo) return;
  if ((millis() - bleJanelaInicioMs) < BLE_LIFETIME_MS) return;
  if (bleConectado) return;
  // Peça sem Wi-Fi salvo é a única que mantém o BLE além da janela. Não é
  // exceção de conveniência: sem credencial o rádio Wi-Fi não tenta associar
  // nada, então não existe disputa de antena a proteger — e é a peça que mais
  // precisa ser alcançada. Peça já configurada fecha a janela mesmo estando
  // offline; para reabrir, tira da tomada e liga de novo (ver bleDeveSubir).
  if (!hasSavedWiFi()) return;
  // Desligar o BLE no meio de um scan congelaria a maquina de estados (o
  // bleTick para de rodar junto), e um scan congelado nao pode existir: e por
  // isso que o estado tambem e zerado logo abaixo, alem do teto de tempo.
  if (bleScanEmAndamento()) return;

  uint32_t heapAntes = ESP.getFreeHeap();
  bleAtivo = false;
  bleScanFase = SCAN_OCIOSO;
  WiFi.scanDelete();
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);   // true = libera memória do controlador também
  // Ponteiros para characteristics que acabaram de ser destruídas: zerar aqui é
  // o que faz bleSetStatus()/blePublish*() virarem no-op em vez de escrever em
  // memória liberada se algo ainda tentar publicar depois do desligamento.
  chrStatus = nullptr;
  chrConfig = nullptr;
  chrScan   = nullptr;
  Serial.printf("BLE: janela de instalacao expirou. Radio BLE desligado ate o proximo religamento na tomada | heap %lu -> %lu (devolveu %lu bytes)\n",
                (unsigned long)heapAntes,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(ESP.getFreeHeap() - heapAntes));
}

/**
 * Nome anunciado no advertise. Peca ja provisionada anuncia o nodeId; peca de
 * fabrica continua anunciando o sufixo do MAC.
 *
 * E o que deixa a lista do Instalador dizer, ANTES de conectar, quais pecas ja
 * foram configuradas - antes disso todas apareciam iguais e a unica forma de
 * saber era abrir uma por uma. O prefixo FOREASY tem que ficar nas duas: e por
 * ele que o app filtra o scan.
 *
 * O nome viaja no scan response (31 bytes: 2 de cabecalho + 29 de texto), nao
 * no pacote de advertise, que ja esta ocupado pelo UUID de 128 bits. Passar de
 * 29 caracteres faria o pacote inteiro ser recusado - e a peca sumiria da lista
 * sem erro nenhum -, dai o corte.
 */
const size_t BLE_NOME_MAX = 29;

String bleNomeAnunciado() {
  String nome = "FOREASY-" + (nodeIdConfigurado() ? nodeId : macSuffix());
  if (nome.length() > BLE_NOME_MAX) nome = nome.substring(0, BLE_NOME_MAX);
  return nome;
}

/**
 * Renomeia sem reiniciar. O payload do advertise so e remontado no
 * startAdvertising(), que acontece quando o app desconecta - entao na pratica a
 * peca ja aparece com o nodeId na lista logo depois de ser configurada, sem
 * precisar de reboot.
 */
void bleAtualizarNome() {
  if (!bleAtivo) return;
  String nome = bleNomeAnunciado();
#if defined(CONFIG_NIMBLE_ENABLED)
  // O core 3.x compila o BLE com NimBLE, e la o nome do advertise e copiado uma
  // unica vez, no primeiro start; depois disso o objeto nao rele mais o nome de
  // GAP. Ou seja, so chamar ble_svc_gap_device_name_set nao mudaria nada no ar.
  // setName() troca a copia E marca o payload para ser remontado.
  BLEDevice::getAdvertising()->setName(nome);
#elif defined(CONFIG_BLUEDROID_ENABLED)
  esp_ble_gap_set_device_name(nome.c_str());
#endif
  Serial.printf("BLE: passando a anunciar como %s\n", nome.c_str());
}

/**
 * O BLE deve subir NESTE boot?
 *
 * Configurar peça é coisa que se faz com ela na mão, ligando na tomada — então a
 * janela de BLE abre em evento de energia, não em qualquer reinício. O que isso
 * evita é concreto: peça offline se auto-reinicia a cada ~15min (auto-restart
 * por falta de WebSocket) e cada reinício reabria uma janela de BLE. Na prática,
 * a peça que mais precisa da antena para levantar o Wi-Fi era justamente a que
 * ficava anunciando BLE a vida inteira, disputando os mesmos 2,4 GHz — a mesma
 * doença que tirou o AP deste firmware. Reinício por software, OTA, watchdog ou
 * pânico agora sobem sem BLE nenhum: nem tempo de antena, nem os ~60KB de heap
 * da pilha.
 *
 * As exceções existem por segurança, não por conveniência:
 *   - Peça sem Wi-Fi salvo: não há o que atrapalhar e é a que mais precisa do
 *     canal. Sempre com BLE.
 *   - BROWNOUT conta como evento de energia: numa fonte fraca o próprio ligar na
 *     tomada pode ser reportado assim, e é exatamente a hora em que o instalador
 *     está esperando a peça aparecer.
 *   - Motivo desconhecido: na dúvida, abre. Peça inalcançável é pior do que
 *     cinco minutos de anúncio.
 *
 * Para reabrir a janela em campo: tirar da tomada e ligar de novo.
 */
bool bleDeveSubir() {
  if (!hasSavedWiFi()) return true;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_UNKNOWN:
      return true;
    default:
      return false;
  }
}

void setupBLE() {
  bleJanelaInicioMs = millis();
  String nome = bleNomeAnunciado();
  // A pilha Bluedroid custa ~90KB de heap, alocados aqui de uma vez (controlador
  // + buffers do host). Não é vazamento e volta no deinit da janela, mas é a
  // diferença que aparece na telemetria durante os 5 min — por isso o antes/depois
  // no log, para não virar mistério em campo.
  uint32_t heapAntes = ESP.getFreeHeap();
  BLEDevice::init(nome.c_str());

  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCallbacks());

  BLEService* svc = srv->createService(BLE_SVC_UUID);
  static ProvisionCallbacks cbs;

  const char* escritas[] = {
    BLE_CHR_AUTH, BLE_CHR_SSID, BLE_CHR_PASS, BLE_CHR_SSID2, BLE_CHR_PASS2,
    BLE_CHR_NODEID, BLE_CHR_AVAIL, BLE_CHR_COMMAND
  };
  for (const char* uuid : escritas) {
    BLECharacteristic* c = svc->createCharacteristic(uuid, BLECharacteristic::PROPERTY_WRITE);
    c->setCallbacks(&cbs);
  }

  chrStatus = svc->createCharacteristic(
    BLE_CHR_STATUS,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  // Sem o 0x2902 o Android não consegue habilitar notificação — o app ficaria
  // preso em "conectando" para sempre, sem erro visível.
  chrStatus->addDescriptor(new BLE2902());
  chrStatus->setValue("idle");

  chrConfig = svc->createCharacteristic(
    BLE_CHR_CONFIG,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chrConfig->addDescriptor(new BLE2902());

  chrScan = svc->createCharacteristic(
    BLE_CHR_SCAN,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chrScan->addDescriptor(new BLE2902());
  // Vazio (e nao "[]") de proposito: o app rele esta characteristic quando o
  // notify nao chega (celular que nao negociou MTU recebe notify truncado), e
  // "[]" aqui seria lido como "a peca nao achou nenhuma rede" antes mesmo de o
  // scan comecar. Texto vazio = "resultado ainda nao pronto".
  chrScan->setValue("");

  svc->start();
  blePublishConfig();   // já disponível para leitura assim que o app conectar

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  // Anunciar o UUID do service é o que faz o app achar a peça: o scan dele
  // filtra por service, não só por nome.
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinInterval(BLE_ADV_ITVL_INSTALACAO);
  adv->setMaxInterval(BLE_ADV_ITVL_INSTALACAO + BLE_ADV_ITVL_FOLGA);
  BLEDevice::startAdvertising();

  Serial.printf("BLE anunciando: %s | heap %lu -> %lu (BLE custou %lu bytes)\n",
                nome.c_str(),
                (unsigned long)heapAntes,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)(heapAntes - ESP.getFreeHeap()));
}

/**
 * Prepara o radio e agenda o disparo do scan.
 *
 * Abortar uma tentativa de conexao em curso e de proposito: com um WiFi.begin()
 * em andamento o esp_wifi_scan_start e recusado, e quem pediu a lista de redes
 * esta com a peca na mao - a tentativa que esta rodando e justamente a que nao
 * esta dando certo. Ao fim do scan o radio volta ao fluxo normal
 * (bleScanEncerrar).
 */
void bleScanPrepararRadio(uint32_t espera) {
  if (wifiConnecting) {
    Serial.println("BLE scan: abortando tentativa de conexao em curso.");
    WiFi.disconnect(false);
    wifiConnecting = false;
  }
  // Pedir a lista de redes cancela um teste em andamento — a tentativa de
  // conexao acabou de ser derrubada, entao o resultado dele nunca viria. Avisa o
  // app, senao o botao de testar fica girando ate o instalador tentar de novo.
  if (bleTestando) {
    bleTestando      = false;
    bleProvisionando = false;
    bleSetStatus("error");
  }
  bleScanFase     = SCAN_PREPARANDO;
  bleScanTimerMs  = millis();
  bleScanEsperaMs = espera;
}

void bleScanIniciar() {
  bleScanTentativa   = 0;
  bleScanRodadaFimMs = millis() + BLE_SCAN_RODADA_MAX_MS;
  // Zera o resultado anterior: enquanto o scan roda, uma leitura desta
  // characteristic tem que dizer "ainda nao pronto", nao devolver a lista velha.
  if (chrScan) chrScan->setValue("");
  bleScanPrepararRadio(BLE_SCAN_PREP_MS);
}

/** Fim de rodada: devolve o radio ao fluxo normal de conexao. */
void bleScanEncerrar() {
  bleScanFase = SCAN_OCIOSO;
  if (WiFi.status() != WL_CONNECTED && !wifiConnecting && hasSavedWiFi()) {
    lastWiFiAttemptMs = millis();
    connectToWiFi_begin();
  }
}

/**
 * Tentativa frustrada. So publica lista vazia quando acabam as tentativas -
 * antes, uma unica recusa do driver ja virava "nenhuma rede encontrada" na tela.
 */
void bleScanFalhou(const char* motivo) {
  bleScanTentativa++;
  Serial.printf("BLE scan: falhou (%s) - tentativa %u/%u\n",
                motivo, bleScanTentativa, BLE_SCAN_TENTATIVAS);
  // O driver pode ter ficado escaneando por baixo (o scanComplete() do core
  // desiste por tempo sem parar o scan de verdade); sem este stop a tentativa
  // seguinte cai no mesmo erro de estado.
  esp_wifi_scan_stop();
  WiFi.scanDelete();

  if (bleScanTentativa >= BLE_SCAN_TENTATIVAS) {
    // Publica ANTES de devolver o radio ao Wi-Fi: o app precisa da resposta, e
    // um begin() logo em seguida so disputaria antena com o notify.
    blePublishScan(0);
    bleScanEncerrar();
    return;
  }
  bleScanPrepararRadio(BLE_SCAN_RETRY_MS);
}

void bleScanTick() {
  if (bleScanFase == SCAN_OCIOSO) return;

  // Teto da rodada: desiste e responde, em vez de continuar tentando e segurar
  // o Wi-Fi do dia a dia por tempo indeterminado.
  if ((int32_t)(millis() - bleScanRodadaFimMs) >= 0) {
    Serial.println("BLE scan: rodada estourou o teto de tempo. Desistindo.");
    blePublishScan(0);
    bleScanEncerrar();
    return;
  }

  if (bleScanFase == SCAN_PREPARANDO) {
    // O ciclo de credito (pulso de START + confirmacao do AVAIL) e uso real da
    // maquina, e dura poucos segundos: o scan espera ele acabar. O instalador
    // pode esperar 3s; uma ficha perdida, nao. Se a maquina nao sossegar, o teto
    // da rodada la em cima encerra tudo sozinho.
    if (pulseActive || creditState != CR_IDLE || otaRequested || otaInProgress) return;
    if ((millis() - bleScanTimerMs) < bleScanEsperaMs) return;
    // async = true: o resultado chega pelo scanComplete(), sem travar o loop
    // nem o watchdog. O retorno E conferido - ignora-lo era a causa raiz da
    // lista vazia em peca ainda sem rede.
    int16_t r = WiFi.scanNetworks(true, false, false, BLE_SCAN_MS_POR_CANAL);
    if (r == WIFI_SCAN_RUNNING) {
      bleScanFase    = SCAN_RODANDO;
      bleScanTimerMs = millis();
      Serial.println("BLE scan: iniciado.");
    } else {
      bleScanFalhou("scan_start recusado");
    }
    return;
  }

  int16_t n = WiFi.scanComplete();
  if (n > 0) {
    blePublishScan(n);
    WiFi.scanDelete();
    bleScanEncerrar();
  } else if (n == 0) {
    // Zero rede e quase sempre scan atropelado pelo radio, nao um lugar sem
    // Wi-Fi nenhum - vale gastar outra tentativa antes de dizer isso ao app.
    bleScanFalhou("nenhuma rede");
  } else if (n == WIFI_SCAN_FAILED) {
    bleScanFalhou("driver devolveu FAILED");
  } else if ((millis() - bleScanTimerMs) > BLE_SCAN_TIMEOUT_MS) {
    bleScanFalhou("timeout");
  }
}

/**
 * Aplica o que chegou por BLE: grava na NVS e reconecta o Wi-Fi já com a rede
 * nova. Roda no loop (não no callback do BLE) porque mexe em NVS e no rádio.
 */
void bleTick() {
  if (!bleAtivo) return;   // radio BLE ja desligado apos a janela de instalacao
  if (bleRebootRequested) {
    bleRebootRequested = false;
    Serial.println("BLE: reboot solicitado.");
    delay(200);
    ESP.restart();
  }

  // ---- Scan de redes (assíncrono: não trava o loop nem o watchdog) ----
  if (bleScanRequested) {
    bleScanRequested = false;
    bleScanIniciar();
  }
  bleScanTick();

  // ---- Teste de rede: conecta SEM gravar ----
  // É o "testar conexão" do wizard: o instalador confirma que a rede funciona
  // antes de commitar. Falhou, a peça volta sozinha para o que estava salvo —
  // um teste errado não pode deixar a máquina offline.
  if (bleTestRequested) {
    bleTestRequested = false;
    if (bleSsid.length() == 0) {
      bleSetStatus("error");
    } else {
      Serial.printf("BLE test: SSID=%s (sem gravar)\n", bleSsid.c_str());
      bleTestando      = true;
      bleProvisionando = true;
      bleSetStatus("connecting");
      webSocket.disconnect();
      isWebSocketConnected = false;
      WiFi.disconnect(false);
      delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.begin(bleSsid.c_str(), blePass.c_str());
      wifiConnecting     = true;
      wifiConnectStartMs = millis();
    }
  }

  if (!bleApplyRequested) return;
  bleApplyRequested = false;
  // Um apply confirma o que estava em teste: daqui pra frente vale o fluxo
  // normal de reconexão, agora com as credenciais gravadas.
  bleTestando = false;

  if (bleSsid.length() == 0) { bleSetStatus("error"); return; }

  sSsid = bleSsid;
  sPass = blePass;
  prefs.putString("ssid", sSsid);
  prefs.putString("pass", sPass);

  // Campo vazio = "não mexer". É o que permite pular a rede 2 no wizard sem
  // apagar a que já estava gravada — o app manda string vazia quando pula.
  if (bleSsid2.length() > 0) {
    sSsid2 = bleSsid2;
    sPass2 = blePass2;
    prefs.putString("ssid2", sSsid2);
    prefs.putString("pass2", sPass2);
  }

  // "en|startPin|availPin" — pipe em vez de JSON para não carregar um parser
  // no firmware só por causa de três inteiros.
  if (bleAvailCfg.length() > 0) {
    int p1 = bleAvailCfg.indexOf('|');
    int p2 = bleAvailCfg.indexOf('|', p1 + 1);
    if (p1 > 0 && p2 > p1) {
      int en = bleAvailCfg.substring(0, p1).toInt();
      int sp = bleAvailCfg.substring(p1 + 1, p2).toInt();
      int ap = bleAvailCfg.substring(p2 + 1).toInt();
      // Mesma validação do loadPrefs: pino proibido não derruba a peça em campo.
      if (gpioLivre(sp) && gpioLivre(ap)) {
        availEnabled = (en != 0);
        startPin = sp;
        availPin = ap;
        prefs.putInt("availEn",  availEnabled ? 1 : 0);
        prefs.putInt("startPin", startPin);
        prefs.putInt("availPin", availPin);
        pinMode(startPin, OUTPUT);
        digitalWrite(startPin, LOW);
        pinMode(availPin, INPUT_PULLUP);
        Serial.printf("BLE: AVAIL en=%d START=%d AVAIL=%d\n", availEnabled, startPin, availPin);
      } else {
        Serial.printf("BLE: pinos invalidos (%d/%d), mantendo os atuais.\n", sp, ap);
      }
    }
  }

  if (bleNodeId.length() > 0) {
    nodeId = bleNodeId;
    prefs.putString("nodeid", nodeId);
    // A peca passa a se anunciar pelo nodeId: na proxima vez que alguem abrir o
    // Instalador, ela ja aparece na lista como configurada.
    bleAtualizarNome();
  }
  // Volta para o slot 1: a rede recém-configurada é a principal, e insistir no
  // slot 2 (de um failover anterior) faria a peça testar a rede errada
  // justamente enquanto o instalador olha o resultado no celular.
  wifiSlot = 0;

  Serial.printf("BLE apply: SSID=%s nodeId=%s\n", sSsid.c_str(), nodeId.c_str());

  bleProvisionando = true;
  bleSetStatus("connecting");

  webSocket.disconnect();
  isWebSocketConnected = false;
  WiFi.disconnect(false);
  delay(100);
  connectToWiFi_begin();
}

void connectToWiFi_begin() {
  if (!hasSavedWiFi()) { Serial.println("Sem credenciais WiFi. Configure pelo app Instalador (BLE)."); return; }
  // Sempre STA puro: sem AP, o rádio nunca precisa do modo dual — que era a
  // origem do conflito de canal (AP num canal, rede alvo em outro, associação
  // do STA travada) e das trocas de modo que derrubavam a potência de TX.
  WiFi.mode(WIFI_STA);
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
      // Sucesso pleno: é aqui que o instalador vê "Tudo certo!" no celular.
      if (bleProvisionando) {
        bleSetStatus("ws_ok");
        bleProvisionando = false;
      }
      blePublishConfig();   // wsOk mudou — o app reflete na hora
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
            "\"txp\":%.1f,\"rst\":\"%s\",\"ble\":%d,"
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
            bleAtivo ? 1 : 0,
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

  // STA puro desde o boot. Ao contrário do firmware com AP, aqui o rádio Wi-Fi
  // só sobe quando há credencial salva — peça virgem fica só no BLE, esperando
  // o app, sem gastar rádio tentando associar em nada.
  WiFi.mode(WIFI_STA);
  if (bleDeveSubir()) {
    setupBLE();
  } else {
    bleAtivo = false;
    Serial.printf("BLE nao sobe neste boot (reset=%s, peca ja configurada). "
                  "Para configurar: tirar da tomada e ligar de novo.\n",
                  resetReasonStr());
  }

  Serial.printf("modo=INDUSTRIAL chip=%s fw=%s\n", FW_CHIP, FW_VERSION);

  if (hasSavedWiFi()) connectToWiFi_begin();
  else Serial.println("Sem WiFi salvo. Configure pelo app Foreasy Instalador (BLE).");
}

// ---------- LED de status: estados e cores ----------
// Prioridade de cima para baixo (o primeiro que bater vence):
//   AP_WAIT    roxo solido       | sem WiFi salvo - peca nova/resetada aguardando configuracao (normal)
//   CONNECTING vermelho piscando | tentativa de conexao em andamento (transitorio, ate WIFI_MAX_WAIT_MS)
//   NO_WIFI    vermelho solido   | tem credencial salva mas nao conseguiu conectar (problema real)
//   WIFI_ONLY  verde solido      | WiFi ok, WebSocket fora (rede local ok, backend/servidor fora)
//   ONLINE     azul solido       | WiFi + WebSocket ok - tudo certo
// No LED comum (HIGH/LOW, nao-RGB) so da para ligar/desligar/piscar - sem cor
// nao da para diferenciar os 5 estados, entao ele so acende fixo em ONLINE e
// pisca em CONNECTING; os demais ficam apagados.
enum LedState : uint8_t { LED_ST_AP_WAIT = 0, LED_ST_CONNECTING, LED_ST_NO_WIFI, LED_ST_WIFI_ONLY, LED_ST_ONLINE };
const uint32_t LED_BLINK_MS = 300;   // ~1,7 Hz

uint8_t ledCurrentState() {
  if (!hasSavedWiFi())               return LED_ST_AP_WAIT;
  if (wifiConnecting)                return LED_ST_CONNECTING;
  if (WiFi.status() != WL_CONNECTED) return LED_ST_NO_WIFI;
  if (!isWebSocketConnected)         return LED_ST_WIFI_ONLY;
  return LED_ST_ONLINE;
}

// Brilho baixo de proposito: o WS2812 no maximo consome ~60mA e ofusca dentro da caixa.
void ledShow() {
  uint8_t st = ledCurrentState();
  bool blinking = (st == LED_ST_CONNECTING);
  bool phaseOn  = !blinking || (((millis() / LED_BLINK_MS) % 2) == 0);

  int key = ((int)st << 1) | (phaseOn ? 1 : 0);
  if (key == lastLedKey) return;
  lastLedKey = key;

  uint8_t r = 0, g = 0, b = 0;
  if (phaseOn) {
    switch (st) {
      case LED_ST_AP_WAIT:    r = 8;  g = 0;  b = 8;  break;   // roxo
      case LED_ST_CONNECTING: r = 12; g = 0;  b = 0;  break;   // vermelho (piscando)
      case LED_ST_NO_WIFI:    r = 12; g = 0;  b = 0;  break;   // vermelho solido
      case LED_ST_WIFI_ONLY:  r = 0;  g = 12; b = 0;  break;   // verde
      case LED_ST_ONLINE:     r = 0;  g = 0;  b = 12; break;   // azul
    }
  }
  bool litSimple = (st == LED_ST_ONLINE) || (blinking && phaseOn);

  switch (ledMode) {
    case LED_MODE_RGB:  rgbLedWrite(ledPin, r, g, b);                  break;
    case LED_MODE_HIGH: digitalWrite(ledPin, litSimple ? HIGH : LOW);  break;
    case LED_MODE_LOW:  digitalWrite(ledPin, litSimple ? LOW  : HIGH); break;
    default: break;   // LED_MODE_OFF: nada a fazer
  }
}

// ================= LOOP =================
void loop() {
  bleTick();
  bleLifetimeTick();
  wifiTick();
  wsTick();
  handleRelayTick();
  readAvailTick();
  creditTick();
  watchdogTick();
  evSaveTick();
  wsRestartTick();
  otaTick();
  restartTick();

  ledShow();
}

// ================= TICKS =================
void wifiTick() {
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      wifiFailStreak = 0;
      Serial.printf("WiFi conectado. IP=%s ch=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
      // Associou, mas ainda falta o servidor — o app mostra isso como etapa
      // intermediária, senão parece travado enquanto o WS não sobe.
      if (bleProvisionando) bleSetStatus("connected");
      // Teste que deu certo encerra aqui. Este estado existe só para o wifiTick
      // não reconectar com as credenciais SALVAS no meio da tentativa; deixá-lo
      // ligado depois é o que fazia a peça parar de reconectar sozinha PARA
      // SEMPRE (o guard lá embaixo tem !bleTestando). E o caminho era o mais
      // comum do wizard: testar a rede, dar certo e fechar o app sem aplicar —
      // a peça seguia funcionando até a primeira queda de Wi-Fi e então ficava
      // offline até alguém reiniciar na mão.
      bleTestando = false;
      if (!isWebSocketConnected) connectToWebSocket();
    } else if ((millis() - wifiConnectStartMs) > WIFI_MAX_WAIT_MS) {
      wifiConnecting = false;
      if (wifiFailStreak < 250) wifiFailStreak++;
      wl_status_t st = WiFi.status();
      Serial.printf("WiFi timeout (%u seguidas, status=%d). Tentará novamente.\n",
                    wifiFailStreak, (int)st);

      // Traduz o motivo para o app: "senha errada" e "rede não existe" pedem
      // ações opostas do instalador, e as duas apareceriam como um erro
      // genérico se olhássemos só o timeout.
      if (bleProvisionando) {
        if      (st == WL_NO_SSID_AVAIL)  bleSetStatus("not_found");
        else if (st == WL_CONNECT_FAILED) bleSetStatus("bad_password");
        else                              bleSetStatus("error");
        bleProvisionando = false;
      }

      // Teste que falhou: nada foi gravado, então a peça volta para a rede que
      // já estava salva. Sem isto ela ficaria offline por causa de uma
      // tentativa do instalador — inaceitável numa máquina em operação.
      if (bleTestando) {
        bleTestando = false;
        WiFi.disconnect(false);
        if (hasSavedWiFi()) {
          Serial.println("BLE test falhou: voltando para a rede salva.");
          connectToWiFi_begin();
        }
        return;
      }

      // Escalonamento. Antes eram N tentativas idênticas: disconnect(false) +
      // begin(), repetido para sempre. Em bancada foram 14 falhas iguais em 10
      // minutos — repetir a mesma coisa não tira o STA de um estado travado.
      // O degrau de "derrubar o AP" saiu junto com o AP: aqui o rádio já é STA
      // puro desde o boot, então o reset da pilha é a única carta que sobrou.
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

  // Durante um teste as credenciais válidas são as que estão sendo testadas, e
  // connectToWiFi_begin() usa as SALVAS — reconectar aqui atropelaria o teste
  // no meio e o instalador veria o resultado da rede errada.
  // O scan tambem segura a reconexao: um begin() disparado no meio dele faz o
  // driver recusar o scan (e o instalador ver "nenhuma rede").
  if (!staUp && !wifiConnecting && !bleTestando && !bleScanEmAndamento() && hasSavedWiFi()
      && (millis() - wifiDownSinceMs) >= WIFI_DOWN_CONFIRM_MS) {
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
        // O degrau seguinte era alternar a potência de TX, remédio do lote ruim
        // de C3. Nesta placa a alimentação sustenta o pico do TX, então sobrou o
        // failover de rede — que é o que de fato resolve aqui.
        Serial.println("WATCHDOG: WS down persistente. Failover de rede.");
        wsDownEscalation = 0;
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
}

void wsRestartTick() {
  if (isWebSocketConnected) { wsLastOkMs = millis(); return; }
  // Peça sem rede configurada não tem para onde reconectar: reiniciar de 30 em
  // 30min não resolve nada e só atrapalha quem estiver configurando pelo app.
  if (!hasSavedWiFi()) { wsLastOkMs = millis(); return; }
  // Só adia o reinício enquanto o pulso está no ar — sem empurrar wsLastOkMs,
  // que agora também é o relógio do backoff. Empurrá-lo aqui zeraria o downtime
  // e jogaria o intervalo de reconexão de volta para 1s.
  if (pulseActive) return;
  // Mesmo adiamento com o instalador conectado: uma peça que está offline há 15
  // min é exatamente a que ele foi configurar, e reiniciar no meio do wizard
  // derruba o BLE e o trabalho junto. O comentário aqui em cima já prometia
  // isso, mas a checagem nunca existiu. Com teto: um celular esquecido
  // conectado não pode desativar a recuperação da peça para sempre.
  if (bleConectado && (millis() - bleConexaoDesdeMs) < BLE_SEGURA_RESTART_MS) return;
  if ((millis() - wsLastOkMs) > (WS_RESTART_TIMEOUT_MS + wsRestartJitterMs)) {
    Serial.printf("WS_RESTART: sem WS por %lumin. Reiniciando.\n",
                  (unsigned long)((WS_RESTART_TIMEOUT_MS + wsRestartJitterMs) / 60000UL));
    delay(200);
    ESP.restart();
  }
}

