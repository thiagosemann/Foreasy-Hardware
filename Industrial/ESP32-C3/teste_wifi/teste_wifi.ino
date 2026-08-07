// ============================================================================
// Foreasy — Teste de conexão WiFi (bancada)
//
// Único objetivo: saber se a peça CONSEGUE CONECTAR numa rede WiFi.
// Não tem AP, não tem WebSocket, não tem NVS, não tem servidor web.
//
// Por que este teste vale: associar numa rede exige TRANSMITIR (probe request,
// authentication, association request). Uma peça que enxerga a rede no scan mas
// nunca associa tem recepção boa e transmissão defeituosa.
//
// COMO USAR
//   1. Preencha SSID e SENHA abaixo (rede 2.4GHz — o ESP32-C3 não pega 5GHz).
//   2. Grave com USB CDC ligado, senão a serial não sai pelo USB-C:
//        arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc -u -p COM5 <esta pasta>
//        arduino-cli monitor -p COM5 -c baudrate=115200
//      (Na IDE: Tools -> USB CDC On Boot -> Enabled)
//   3. Se a peça PASSAR, ela sobe um AP `TESTE-<3 ultimos bytes do MAC>` (senha
//      12345678) e fica com ele no ar, para você confirmar no celular que a peça
//      também emite beacon — que é o sintoma original do lote ruim.
//   4. O teste PARA sozinho assim que há veredito. Pelo LED, sem monitor:
//        aceso fixo    = APROVADA (conectou)
//        piscando rápido = REPROVADA (falhou em todas as potências)
//        piscando lento  = ainda testando
//
// VARREDURA DE POTÊNCIA (automática)
//   Cada tentativa usa um nível de TX diferente (19,5 / 15 / 11 / 8,5 / 5 dBm) e,
//   ao fim da passada, sai um RESUMO com PASS/FAIL por nível. Uma gravação só
//   responde se a falha é entrega de energia da placa (conecta nos níveis baixos)
//   ou antena/casamento de RF (falha em todos). Leva ~2 min por peça.
// ============================================================================

#include <WiFi.h>

// ---------------------------------------------------------------------------
// Preencha antes de gravar. Deixei sem a senha real de propósito: este arquivo
// está dentro do repositório, e senha de WiFi commitada vaza junto com o código.
const char* SSID_TESTE  = "SEMANN";
const char* SENHA_TESTE = "Semann1991!";

// Varredura automática: cada tentativa usa um nível de potência diferente, então
// uma única gravação testa a escala inteira. Se a peça só associa nos níveis
// baixos, o problema é o 3V3 afundando nos picos de TX (regulador/capacitor da
// placa). Se falha em todos, é antena/casamento — sem correção por firmware.
const wifi_power_t NIVEIS[] = {
  WIFI_POWER_19_5dBm, WIFI_POWER_15dBm, WIFI_POWER_11dBm,
  WIFI_POWER_8_5dBm,  WIFI_POWER_5dBm
};
const uint8_t N_NIVEIS = sizeof(NIVEIS) / sizeof(NIVEIS[0]);

const uint32_t TIMEOUT_MS = 20000;    // tempo máximo por tentativa
const int      ledPin     = 8;        // LED azul integrado — ATIVO LOW
// ---------------------------------------------------------------------------

uint32_t tentativas = 0, sucessos = 0;
uint8_t  nivel = 0;                   // índice do nível em teste
bool     resultado[N_NIVEIS];         // placar por nível, para o resumo
float    aplicado[N_NIVEIS];          // potência REAL usada, lida de volta do rádio

// Estado terminal: assim que há veredito o teste para, para a bancada não precisar
// interpretar log rolando. LED aceso fixo = aprovada | piscando rápido = reprovada.
bool encerrado = false;
bool aprovado  = false;

String apNome = "";   // preenchido quando a peça passa e o AP sobe

void led(bool aceso) { digitalWrite(ledPin, aceso ? LOW : HIGH); }

// Sobe um AP na peça aprovada, para confirmar visualmente no celular que ela
// também transmite beacon — que é o sintoma original do lote ruim. Mantém o STA
// conectado de propósito: é o mesmo modo AP_STA que o firmware de produção usa.
void subirAP() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  apNome = "TESTE-" + mac.substring(6);   // últimos 3 bytes: identifica a peça

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);                   // power save deixa o beacon intermitente
  // softAPConfig ANTES do softAP: depois, ele reinicia o DHCP com o AP no ar.
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1),
                    IPAddress(255,255,255,0));
  bool ok = WiFi.softAP(apNome.c_str(), "12345678", 1, false);

  Serial.println();
  Serial.printf("   AP de verificacao: %s (senha 12345678)\n", apNome.c_str());
  Serial.printf("   softAP=%s | IP=%s | mac=%s | canal=%d\n",
                ok ? "OK" : "FALHOU",
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(), WiFi.channel());
  Serial.println("   Procure esta rede no celular. Em AP_STA o AP segue o canal");
  Serial.println("   da rede conectada, entao o canal acima e o esperado.");
}

// Traduz o código do WiFi.status() para algo legível no relatório.
const char* statusTxt(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE (parado)";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (nao achou a rede)";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (senha errada ou nao respondeu)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED (nao associou)";
    case WL_CONNECTED:       return "CONNECTED";
    default:                 return "desconhecido";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);            // o USB CDC precisa enumerar antes do primeiro print

  pinMode(ledPin, OUTPUT);
  led(false);

  Serial.println();
  Serial.println("========================================");
  Serial.println("   TESTE DE CONEXAO WIFI - Foreasy");
  Serial.println("========================================");
  Serial.printf("Rede alvo   : %s\n", SSID_TESTE);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // MAC só depois do mode(): antes disso a base MAC ainda não foi inicializada
  // e a leitura volta 00:00:00:00:00:00 em qualquer placa.
  Serial.printf("MAC da peca : %s\n", WiFi.macAddress().c_str());
  Serial.println();

  // ---- Etapa 1: RECEPCAO ---------------------------------------------------
  // Scan é passivo do ponto de vista do rádio de potência: se isto funciona e a
  // conexão não, o problema está isolado na transmissão.
  Serial.println("[1/2] Testando RECEPCAO (scan)...");
  int n = WiFi.scanNetworks();
  Serial.printf("      %d redes visiveis\n", n);

  bool achou = false;
  for (int i = 0; i < n; i++) {
    bool alvo = (WiFi.SSID(i) == SSID_TESTE);
    if (alvo) achou = true;
    if (alvo || i < 5) {
      Serial.printf("      %s%-24s ch=%2d rssi=%d\n",
                    alvo ? ">> " : "   ", WiFi.SSID(i).c_str(),
                    WiFi.channel(i), WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();

  if (n <= 0) {
    Serial.println("      FALHA: nao viu rede nenhuma. Recepcao defeituosa.");
  } else if (!achou) {
    Serial.printf("      ATENCAO: '%s' nao esta na lista. Rede 5GHz, fora de\n"
                  "      alcance ou nome errado? Corrija antes de julgar a peca.\n",
                  SSID_TESTE);
  } else {
    Serial.println("      OK: recepcao funcionando e a rede alvo esta visivel.");
  }
  Serial.println();
  Serial.println("[2/2] Testando TRANSMISSAO (associacao)...");
}

void loop() {
  if (encerrado) {
    if (aprovado) led(true);                          // aceso fixo
    else          led(((millis() / 150) % 2) == 0);   // piscando rápido
    static uint32_t ultimoAviso = 0;
    if (millis() - ultimoAviso > 10000) {
      ultimoAviso = millis();
      if (aprovado) {
        Serial.printf("   [encerrado] APROVADA | AP '%s' no ar | clientes=%u | wifi=%s\n",
                      apNome.c_str(), WiFi.softAPgetStationNum(),
                      WiFi.status() == WL_CONNECTED ? "conectado" : "caiu");
      } else {
        Serial.println("   [encerrado] PECA REPROVADA - troque a peca e resete.");
      }
    }
    delay(50);
    return;
  }

  tentativas++;

  // disconnect(false): o `true` do primeiro parâmetro é `wifioff` e DESLIGA o
  // rádio. Com o rádio desligado o setTxPower() é ignorado sem erro e o begin()
  // seguinte reinicia tudo na potência máxima — a varredura não acontecia.
  WiFi.disconnect(false);
  delay(200);

  // Potência aplicada com o rádio já iniciado, e conferida na leitura de volta.
  WiFi.setTxPower(NIVEIS[nivel]);
  delay(50);
  float pedido = NIVEIS[nivel] / 4.0;
  float real   = WiFi.getTxPower() / 4.0;
  Serial.printf("\n--- Tentativa %lu | TX pedido=%.1f dBm  aplicado=%.1f dBm%s ---\n",
                tentativas, pedido, real,
                (fabs(pedido - real) > 0.3) ? "   <<<< NAO APLICOU, IGNORE" : "");

  WiFi.begin(SSID_TESTE, SENHA_TESTE);

  uint32_t inicio = millis();
  int ultimo = -1;
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio) < TIMEOUT_MS) {
    int s = WiFi.status();
    if (s != ultimo) {                       // só loga quando o estado muda
      ultimo = s;
      Serial.printf("      [%4lums] status=%d %s\n", millis() - inicio, s, statusTxt(s));
    }
    led(((millis() / 200) % 2) == 0);        // piscando = tentando
    delay(50);
  }

  resultado[nivel] = (WiFi.status() == WL_CONNECTED);
  aplicado[nivel]  = WiFi.getTxPower() / 4.0;   // lido no fim: confirma que segurou

  if (WiFi.status() == WL_CONNECTED) {
    sucessos++;
    led(true);
    Serial.println();
    Serial.println("      >>> PASS - A PECA CONECTOU <<<");
    Serial.printf("      IP      : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("      Gateway : %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("      Canal   : %d\n", WiFi.channel());
    Serial.printf("      RSSI    : %d dBm\n", WiFi.RSSI());

    // Veredito imediato: não faz sentido seguir varrendo depois que a peça provou
    // que transmite. Mas ONDE ela passou importa — passar só em potência reduzida
    // não é peça normal, é peça que não aguenta os picos de corrente do TX.
    encerrado = true;
    aprovado  = true;
    Serial.println();
    if (nivel == 0) {
      Serial.println("===== VEREDITO: PECA APROVADA =====");
      Serial.printf("   Conectou na potencia maxima (%.1f dBm). Peca normal.\n",
                    aplicado[nivel]);
    } else {
      Serial.println("===== VEREDITO: APROVADA COM RESSALVA =====");
      Serial.printf("   Conectou com %.1f dBm, mas FALHOU na potencia maxima.\n",
                    aplicado[nivel]);
      Serial.println("   A peca transmite, porem nao aguenta os picos de corrente do");
      Serial.println("   TX em potencia cheia: entrega de energia da placa (regulador");
      Serial.println("   ou capacitor de bulk). Separe estas pecas das aprovadas.");
    }
    Serial.println("===========================================");
    subirAP();
    return;
  } else {
    led(false);
    Serial.println();
    Serial.printf("      >>> FAIL - nao associou em %lus (status=%d %s)\n",
                  TIMEOUT_MS / 1000, WiFi.status(), statusTxt(WiFi.status()));
    Serial.println("      Se o scan acima ENXERGOU a rede, a recepcao esta boa e");
    Serial.println("      a falha e de TRANSMISSAO.");
  }

  // Só chega aqui quem falhou: qualquer PASS encerra o teste antes. Uma passada
  // completa sem nenhuma conexão é o veredito de reprovação.
  nivel++;
  if (nivel >= N_NIVEIS) {
    nivel = 0;
    Serial.println("\n===== RESUMO DA VARREDURA DE POTENCIA =====");
    Serial.println("   pedido  aplicado  resultado");
    bool varreduraValida = true;
    for (uint8_t i = 0; i < N_NIVEIS; i++) {
      float ped = NIVEIS[i] / 4.0;
      bool  ok  = (fabs(ped - aplicado[i]) <= 0.3);
      if (!ok) varreduraValida = false;
      Serial.printf("   %5.1f    %5.1f     %s%s\n", ped, aplicado[i],
                    resultado[i] ? "PASS" : "FAIL", ok ? "" : "   <- potencia nao aplicada");
    }
    if (!varreduraValida) {
      // Sem isto o resumo condena a peca com base em testes que rodaram todos na
      // mesma potencia. O veredito so vale se cada nivel foi realmente aplicado.
      Serial.println("   -> VARREDURA INVALIDA: algum nivel nao foi aplicado ao radio.");
      Serial.println("      NAO reprove a peca. Me mostre este log.");
    } else {
      Serial.println("   -> VEREDITO: PECA REPROVADA");
      Serial.println("      Falhou em TODOS os niveis: antena ou casamento de RF.");
      Serial.println("      Sem correcao por firmware. Peca para devolucao.");
      encerrado = true;
      aprovado  = false;
    }
    Serial.println("===========================================");
  }

  Serial.printf("\n      Placar: %lu/%lu conexoes. Proxima em 5s.\n",
                sucessos, tentativas);
  delay(5000);
}
