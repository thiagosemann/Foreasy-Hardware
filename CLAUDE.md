# Foreasy Hardware — Contexto do Projeto
Produto próprio (hardware + firmware) para automação de lavadoras, substituindo
soluções concorrentes (VMlav, VENDPAGO).

## Índice
- [1. Visão Geral](#1-visão-geral)
- [2. Modelos de Sistema](#2-modelos-de-sistema)
- [3. Modelos de Hardware](#3-modelos-de-hardware)
- [4. Hardware da Máquina — Speed Queen Quantum 440G](#4-hardware-da-máquina--speed-queen-quantum-440g)
- [5. Hardware do ESP — Shield Serial](#5-hardware-do-esp--shield-serial-modelos-1-e-5)
- [6. Firmware](#6-firmware)
- [7. Protocolos WebSocket](#7-protocolos-websocket)
- [8. Configuração da Máquina Speed Queen (MDC)](#8-configuração-da-máquina-speed-queen-mdc)
- [9. Próximos Passos](#9-próximos-passos)
- [10. Referências](#10-referências)
- [11. Plano — Detecção de Uso por Ficha (Token)](#11-plano--detecção-de-uso-por-ficha-token)

---

## 1. Visão Geral

O que o produto faz:
- **Modo Industrial** — controle direto de energia: relay ativo libera, relay inativo corta.
- **Modo Convencional** — integração com lavadoras **Speed Queen** via pulso elétrico no conector H5.
- **Status da máquina** (livre/ocupada) lido via AVAIL OUT (Speed Queen).
- **Comunicação com o backend** via WebSocket.

> ⚠️ **Inversão de nomenclatura:** a descrição conceitual acima é o *inverso* da
> convenção do firmware. No firmware, `machineMode`: **Industrial (1) = pulso**,
> **Convencional (0) = relé ON/OFF**. Ao ler código, vale a convenção do firmware.

---

## 2. Modelos de Sistema

| Sistema | O que faz | Comunica com a placa da máquina? |
|---------|-----------|----------------------------------|
| **Industrial** | Controle puro de energia: backend liga/desliga o relay, que libera ou corta a alimentação. | Não — funciona com qualquer lavadora. |
| **Convencional** | Integração Speed Queen via conector H3/H5: pulso de crédito libera o ciclo; AVAIL OUT confirma (em implementação). | Sim — Speed Queen. |

---

## 3. Modelos de Hardware

### Industrial

| # | MCU | Shield | Observação |
|---|-----|--------|------------|
| 1 | ESP8266 ESP-01S | Shield relay serial (AZ-Delivery) | Relay controlado via UART 9600 baud (STC15F104W) |
| 2 | ESP8266 ESP-01S | Shield relay GPIO V1 | Relay controlado diretamente por GPIO |

### Convencional

| # | MCU | Shield | Observação |
|---|-----|--------|------------|
| 3 | ESP32 | Shield relé 30A | Relay de alta corrente controlado por GPIO |
| 4 | ESP32 | Shield relé 30A + SSR | Relay 30A aciona SSR (Solid State Relay) |
| 5 | ESP8266 ESP-01S | Shield serial + SSR | Shield serial (STC15F104W) aciona SSR |

---

## 4. Hardware da Máquina — Speed Queen Quantum 440G

### 4.1 Conector de pagamento — AP1 Front End Control

> **Atenção sobre nomenclatura:** o schematic 807300 chama o conector de **H3** (7 pinos);
> o manual elétrico ALPM-39201 chama de **H5**. Verifique fisicamente qual plug você usa.

#### Mapeamento por função (schematic 807300 — H3)

| Pino H3 | Label ALPM | Função |
|---------|------------|--------|
| H3-7   | START IN   | START PULSE IN — cátodo do optoacoplador H11L1 (100Ω interno) |
| H3-6   | —          | START PULSE (2ª entrada paralela, com 100Ω interno) |
| H3-5   | AVAIL EMIT | AVAILABLE OUTPUT — emissor do 4N25 (conectar ao GND/H3-2) |
| H3-4   | AVAIL COL  | AVAILABLE OUTPUT — coletor do 4N25 (pull-up externo aqui) |
| H3-3   | +5V        | EXTERNAL 5VDC — alimentação fornecida pela máquina (20mA) |
| H3-2   | COM        | EXTERNAL COMMON — GND de referência |
| H3-1   | TxD        | SCI TxD OUT — serial da máquina (4.5–5.5VDC, 5mA) |
| H3-8   | —          | UNUSED |

> **Componentes confirmados pelo schematic:** START PULSE usa optoacoplador **H11L1**;
> AVAIL OUT usa optoacoplador **4N25** com coletor (H3-4) e emissor (H3-5) expostos separadamente.

### 4.2 Especificações elétricas (schematic 807300 + ALPM-39201)

> ℹ️ Os valores abaixo são os **limites de catálogo** (com pull-up externo / fonte +5V).
> A ligação **realmente usada e validada** dispensa resistores externos — ver [§4.5](#45-ligação-validada--sem-resistores-externos-).

- **START IN**: 3–30mA, **mínimo 20ms** (schematic 807300); ALPM diz 45ms — usar 100ms no firmware é seguro.
- **AVAIL OUT**: Vmax 28VDC, Imax 5mA — pull-up externo 10kΩ em H3-4 (COL); H3-5 (EMIT) → GND (H3-2).
- **Circuito de pulso (alternativa com fonte +5V)**: `+5V (H3-3) → 150Ω externo → H3-7 (START IN)` | `H3-2 → GND do shield`.
  - Cálculo: ~5V / (100Ω interno + 150Ω externo) ≈ 14mA — dentro de 3–30mA ✓

### 4.3 Como medir AVAIL OUT com osciloscópio

```
H3-3 (+5V) ── 10kΩ ──┬── CH1 osciloscópio
                      |
                    H3-4 (AVAIL COL — 4N25)
                    H3-5 (AVAIL EMIT) ── H3-2 (COM/GND) ── GND osciloscópio
```

### 4.4 Polaridade do AVAIL OUT — IMPORTANTE

O schematic 807300 indica **"ON = AVAILABLE"** — o transistor conduz quando a máquina está **livre**:

| Nível (com pull-up 10kΩ → 3.3V) | Estado da máquina |
|----------------------------------|-------------------|
| **LOW (0V)** — transistor ON     | **Livre / disponível** |
| **HIGH (3.3V)** — transistor OFF | **Ocupada / em ciclo** |

**Lógica de confirmação após pulso (implementada no firmware como `creditTick`):**
1. Antes: AVAIL OUT = LOW (máquina livre) → ok para enviar pulso.
2. Enviar pulso.
3. Máquina aceita → inicia ciclo → AVAIL OUT vai para HIGH.
4. HIGH em até 800ms = sucesso | ainda LOW após 800ms = máquina não aceitou → retry.

### 4.5 Ligação validada — sem resistores externos ✅

> **Testado e funcionando** na bancada (`esp32_avail.ino`) e portado para o ESP32-S3
> (`esp32s3.ino`). Difere do pull-up externo 10kΩ que as specs originais sugeriam:
> na prática o **pull-up interno do ESP32 basta** e o **START IN dispensa resistor**.

```
AVAIL OUT (leitura de status):
  GPIO availPin ──── H3-4 (AVAIL COL — coletor 4N25)   [pull-up INTERNO do ESP32, ~45kΩ]
  GND ESP32     ──── H3-5 (AVAIL EMIT) ── H3-2 (COM)

START IN (pulso de crédito):
  GPIO startPin ──── H3-7 (START IN)                    [SEM resistor externo]
  GND ESP32     ──── H3-2 (COM)                         [mesmo nó do AVAIL]
```

- **AVAIL OUT — sem resistor externo:** o pull-up interno (~45kΩ) segura a linha em HIGH;
  o 4N25 só precisa puxar para GND quando conduz — corrente bem dentro do Imax (5mA). LOW=livre, HIGH=ocupada.
- **START IN — sem resistor externo:** o 100Ω interno do H11L1 já limita a corrente.
  `(3.3V − ~1.3V do LED) / 100Ω ≈ 20mA` — dentro da faixa 3–30mA do H11L1 e do limite do GPIO.

---

## 5. Hardware do ESP — Shield Serial (Modelos 1 e 5)

**ESP-01S + Shield Relay AZ-Delivery**

- MCU relay: STC15F104W (também chamado 7250M / SOC 121011-Q)
- Relay: SRD-5VDC-SL-C (10A)
- Regulador: AMS1117-3.3 — GPIOs em 3.3V TTL
- **Relay controlado via Serial 9600 baud** (não GPIO direto)
- Modelo 5 (Convencional): relay aciona SSR externo de alta corrente

### 5.1 Comandos Serial do Relay (STC15F104W)

```cpp
uint8_t ON_CMD[]  = {0xA0, 0x01, 0x01, 0xA2};  // Relay ON
uint8_t OFF_CMD[] = {0xA0, 0x01, 0x00, 0xA1};  // Relay OFF
```

### 5.2 Limitações do ESP-01S

- UART única (GPIO1/TX, GPIO3/RX) — ocupada pelo relay.
- GPIO0 e GPIO2 = strapping pins (boot) — usar com cuidado.
- Brownout no acionamento do relay → desabilitado no firmware.
- **Pulso mínimo real: ~100ms** (STC15F104W não processa OFF abaixo de 80ms).

---

## 6. Firmware

### 6.1 Arquivos

| Arquivo | Hardware | Descrição |
|---------|----------|-----------|
| `ESP32-S3/esp32s3/esp32s3.ino` | ESP32-S3 | **Firmware principal / centralizado** — Industrial (pulso START) + Convencional (relé); pinos e servidor configuráveis via wizard |
| `ESP8266/industrial_serial/industrial_serial.ino` | Modelos 1 e 5 | ESP-01S + shield serial (STC15F104W) — firmware principal ESP8266 |
| `ESP8266/industrial_sem_serial/industrial_sem_serial.ino` | Modelo 2 | ESP-01S + shield GPIO V1 — relay via GPIO0 direto |
| `ESP32/esp32/esp32.ino` | Modelos 3 e 4 | ESP32 + shield relé 30A (com ou sem SSR) |
| `ESP32-AVAIL/esp32_avail/esp32_avail.ino` | Protótipo de bancada | Teste de AVAIL OUT + pulso START IN (AP próprio, sem WiFi STA/WS) |

---

### 6.2 ESP32-S3 — `esp32s3.ino` (firmware principal / centralizado)

Firmware unificado para onde estamos migrando Industrial + Convencional.

> **Nomenclatura (atenção):** `machineMode` segue a convenção dos outros ESPs —
> **Industrial (1) = pulso**, **Convencional (0) = relé ON/OFF** — o *inverso* da
> descrição conceitual em [Modelos de Sistema](#2-modelos-de-sistema). Aqui vale a do firmware.

#### Modos de operação
*(`relayMode` Sempre ON/OFF foi removido — só Industrial/Convencional.)*

- **Industrial** (`machineMode=1`, padrão): **não usa relé**. Dá pulso de `PULSE_MS` (100ms)
  no `startPin`, direto no optoacoplador START IN (Speed Queen H3-7), ativo HIGH.
  WS `0x01` dispara o pulso (ver [§6.2.1](#621-credit-fail-safe--avail-industrial)); `0x02` ignorado.
- **Convencional** (`machineMode=0`): relé ON/OFF no `relayPin`. WS `0x01`=ON, `0x02`=OFF.
  `relayType`: `0`=NA (ON=HIGH) / `1`=NF (ON=LOW).

#### 6.2.1 Credit fail-safe — AVAIL (Industrial) ✅ implementado

Controlado pela flag `availEnabled` (NVS `availEn`). Modelos sem AVAIL ligado → deixar OFF.

**Máquina de estados `creditTick()` — não-bloqueante:**

| Estado | Descrição |
|--------|-----------|
| `CR_IDLE` | Aguardando comando |
| `CR_PULSE` | Pulso ativo no startPin (100ms) |
| `CR_CONFIRM` | Aguarda AVAIL ir de LOW→HIGH em até `CREDIT_CONFIRM_MS` (800ms) |
| `CR_GAP` | Espera `CREDIT_GAP_MS` (400ms) antes do próximo pulso |

**Constantes:**
- `CREDIT_MAX_ATTEMPTS = 3` — tentativas antes de desistir
- `CREDIT_CONFIRM_MS = 800` — janela para máquina ficar OCUPADA após pulso
- `CREDIT_GAP_MS = 400` — pausa entre tentativas

**Comportamento do WS `0x01` no modo Industrial:**
- **Sem `availEnabled`:** dispara `startPulse()` e responde `"RelayStatus:ON"` imediatamente.
- **Com `availEnabled`:** chama `creditStart()`, que:
  - Se máquina OCUPADA (AVAIL=HIGH): responde `"CreditBusy"` e ignora.
  - Se máquina LIVRE (AVAIL=LOW): entra na máquina de estados; respostas assíncronas:
    - `"RelayStatus:ON"` — crédito aceito (AVAIL foi para HIGH dentro da janela)
    - `"CreditFail"` — esgotou `CREDIT_MAX_ATTEMPTS` sem confirmação

#### AVAIL OUT (leitura) — ✅ testado e funcionando
- `availPin` em `INPUT_PULLUP` (pull-up interno, **sem resistor externo** — ver [§4.5](#45-ligação-validada--sem-resistores-externos-)) com debounce 50ms (`readAvailTick()` no loop).
- LOW=livre, HIGH=ocupada. Exposto em `/status`, `/info` e no WS `0x05`.

> ⚠️ **Discrepância de default do `availPin`:** o C++ inicializa `availPin = 4` (mesmo que `ledPin`),
> mas o wizard `/config` exibe `6` como fallback. Se nada estiver salvo na NVS, o pino efetivo
> será GPIO4 — conflitando com o LED. Definir e salvar o pino correto via wizard antes de usar.

#### OTA (Over-The-Air)
- Disparado pelo WS `0x04` (ver [§7.4](#74-detalhe--ota-0x04)).
- Baixa o `.bin` por HTTP(S), grava com `Update`, valida SHA256 e reinicia.
- `FW_VERSION` é reportado no WS `0x03` (campo `fw`) para auditoria da frota.
- Storage do binário **nunca** no Heroku — ver `docs/PLANO-NOVA-VERSAO.md` §4.

#### Pinos e servidor (configuráveis, persistidos na NVS, aplicados no boot)
- Pinos default C++: `relayPin`=GPIO2, `startPin`=GPIO5, `availPin`=GPIO4 (⚠️ ver discrepância acima), `ledPin`=GPIO4 (fixo).
- Wizard `/config` usa como fallback: `relayPin`=2, `startPin`=5, `availPin`=6.
- Servidor WebSocket (`wsHost`/`wsPort`) editável pelo wizard — default `frst-back-...herokuapp.com:80`.

#### Páginas web (somente duas)
- `/config` — wizard step-by-step (Rede → Servidor → Modo → Pinos), self-contained e mobile.
- `/info` — status com auto-refresh 2s (inclui AVAIL, pinos, servidor, estado do credit fail-safe).
- `/` redireciona para `/config`.
- Endpoints de apoio: `/config-data`, `/save`, `/scan`, `/status`, `/resetwifi` (= `prefs.clear()`), `/restart`.
- `/save` salva na NVS e reinicia o dispositivo.

#### Armazenamento (NVS namespace `wifi`)
`ssid`, `pass`, `ssid2`, `pass2`, `nodeid`, `machineMode`, `relayType`, `wsHost`,
`wsPort`, `relayPin`, `startPin`, `availPin`, `availEn`, `wsrestart`, `bootCount`.

#### Resiliência (reutilizado dos Modelos 3/4)
Dual WiFi + failover sem restart, WiFi não-bloqueante, WS backoff 10s→120s, watchdogs
(WS >5min / global >8min), WS zombie, `wsRestartEnabled`, temperatura interna, scan WiFi
async. Protocolo WS idêntico ao ESP32 (+ `0x04`/`0x05`).

---

### 6.3 Modelos 1 e 5 — `Foreasy-Hardware-Serial.ino`

**Relay controlado via Serial 9600 baud → STC15F104W (não GPIO)**

- Serial 100% dedicada ao relay — sem debug por Serial; logs só em `/info` enquanto AP ativo.
- `machineMode`: `INDUSTRIAL` (pulso 100ms por `0x01`) ou `CONVENTIONAL` (`0x01`=ON / `0x02`=OFF).
- `relayInvert`: inverte lógica ON/OFF — útil para relay NC.
- **EEPROM v4** (`magic=0xF0EA5E01`): `ssid`, `pass`, `ssid2`, `pass2` (failover), `nodeId`,
  `machineMode`, `relayInvert`, `bootCount`, `lastResetReason`, `wsRestartEnabled`.
- AP: 10 min ativo após boot → lean mode (AP desliga, logs apagados do buffer).
- SSID do AP: `<nodeId>-AP` | Senha: `12345678`.
- Failover automático entre rede 1 e rede 2 sem restart.
- **Watchdogs sem restart**: WS down >5 min → failover WiFi+WS; sem WiFi+WS >8 min → failover.
- `wsRestartEnabled`: restart automático se sem WS por 1 hora (configurável).
- WS backoff exponencial: 10s base → 120s máx.
- Scan WiFi assíncrono (não bloqueia o loop).
- Erase de setor EEPROM antes de salvar (`ERASE_SECTOR_BEFORE_SAVE=true`) + 3 tentativas de commit com verificação.
- Rotas HTTP: `/config`, `/config-data`, `/info`, `/scan`, `/save`, `/diagflash`.

---

### 6.4 Modelo 2 — `Foreasy-Hardware.ino`

**Relay controlado via GPIO0 direto — Serial 115200 disponível para debug**

- Mesma estrutura de EEPROM v4 e mesmo protocolo WS que o firmware Serial.
- Mesmo comportamento `machineMode` (INDUSTRIAL/CONVENTIONAL) e `relayInvert`.
- Flags de debug habilitados: `DEBUG_FLASH_INFO=true`, `DEBUG_EEPROM_DUMP=true`, `DEBUG_EEPROM_SECTORINFO=true`.
- Scan WiFi **síncrono** (bloqueia o loop durante o scan — diferença do Serial).
- GPIO0 = relay (strapping pin — cuidado no boot); GPIO2 = LED ativo LOW.
- `applyRelayPhysical()` usa `digitalWrite` em vez de `Serial.write`.
- Logs críticos (`[CRIT]`) enviados via Serial mesmo após AP desligar.

---

### 6.5 Modelos 3 e 4 — `esp32.ino`

**Relay controlado via GPIO2; LED em GPIO4**

- Usa **Preferences (NVS)** — `prefs.begin("wifi", false)`; credenciais cacheadas em Strings no boot.
- `machineMode`: `0`=Convencional (`0x01`=Relay ON / `0x02`=Relay OFF) | `1`=Industrial (`0x01`=pulso de `PULSE_MS` 100ms, `0x02` ignorado).
- `relayMode`: `0`=Normal (segue WS/machineMode), `1`=Sempre ON, `2`=Sempre OFF.
- `relayInvert`: `false`=normal (ON=HIGH/OFF=LOW), `true`=invertido (ON=LOW/OFF=HIGH) — útil para relay NF.
- Em modo Sempre ON/OFF: comandos WS, machineMode e botões locais são ignorados.
- **Dual WiFi com failover** (ssid/ssid2) — alterna sem restart, `wifiSlot` rastreado.
- **WiFi não-bloqueante**: `wifiTick()` com timeout de 40s, retry a cada 5s — **sem apagar credenciais por falha de conexão**.
- **WS backoff exponencial**: 10s base → 120s máx.
- **Watchdogs**: WS down >5min → failover; sem WiFi+WS >8min → failover.
- `wsRestartEnabled`: restart após 1h sem WS (configurável).
- **WS zombie**: detecta ausência de ping/pong por 5min e reconecta.
- App ping a cada 30s; `webSocket.enableHeartbeat(15000, 3000, 2)`.
- Ao conectar WS: envia `"ID:<nodeId>"` se relay ON, `"NID:<nodeId>"` se OFF.
- Comando `0x03` responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse`.
- Lê temperatura interna (`temperatureRead()`).
- `bootCount` incrementado em RAM; salvo em Preferences apenas no `/save`.
- Scan WiFi **assíncrono** (`WiFi.scanNetworks(true)`).
- `/config-data` endpoint JSON separado da página HTML.
- Rotas HTTP: `/config`, `/config-data`, `/info`, `/scan`, `/save`, `/status`, `/relay`, `/relay/on`, `/relay/off`, `/relay/config`, `/nodeid`, `/savenodeid`, `/wifistatus`, `/wsstatus`, `/resetwifi`, `/restart`.

---

## 7. Protocolos WebSocket

Mensagens binárias (1 byte de comando); resposta sempre em texto.

> **Identidade no `0x03`:** todos os firmwares reportam `chip` e `fw` (constantes de
> compilação `FW_CHIP`/`FW_VERSION`) na telemetria, para auditoria da frota e seleção
> correta do binário de OTA. Valores de `chip`: `esp8266` (Modelos 1, 2, 5),
> `esp32` (Modelos 3, 4) e `esp32s3` (firmware principal). `fw` é semver (ex. `1.0.0`).

### 7.1 ESP8266 (Modelos 1, 2, 5)

| Byte | Modo Industrial | Modo Convencional |
|------|----------------|-------------------|
| `0x01` | Pulso relay (100ms) | Relay ON |
| `0x02` | Ignorado | Relay OFF |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, machineMode, pulse, chip, fw` | idem |
| `0x06` | Restart remoto — responde `"Restarting"` e reinicia após ~200ms | idem |

> **Não disponível no ESP-01S:** `0x04` (OTA — 1MB de flash não cabe imagem dupla) e
> `0x05` (AVAIL — sem GPIO livre, UART ocupada pelo relay). Ambos só no ESP32-S3.
> Sem sensor de temperatura interno → o `0x03` do ESP8266 **não** tem campo `temp`.

### 7.2 ESP32 (Modelos 3, 4)

| Byte | Modo Convencional | Modo Industrial |
|------|--------------------|-----------------|
| `0x01` | Relay ON (se `relayMode`=Normal) | Pulso relay (`PULSE_MS`=100ms, se `relayMode`=Normal) |
| `0x02` | Relay OFF (se `relayMode`=Normal) | Ignorado |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw` | idem |

Resposta sempre: `"RelayStatus:ON"` ou `"RelayStatus:OFF"`.

### 7.3 ESP32-S3 (firmware principal)

Mesmos bytes do ESP32, com duas diferenças: **não há `relayMode`** (Sempre ON/OFF removido)
e no **Industrial o pulso sai pelo `startPin` (GPIO START IN), não pelo relé**.

| Byte | Modo Convencional | Modo Industrial |
|------|--------------------|-----------------|
| `0x01` | Relay ON | Pulso no START IN (`PULSE_MS`=100ms). Sem `availEn`: resposta imediata `"RelayStatus:ON"`. Com `availEn`: resposta assíncrona via `creditTick` (ver [§6.2.1](#621-credit-fail-safe--avail-industrial)) |
| `0x02` | Relay OFF | Ignorado |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw` | idem |
| `0x04` | OTA — ver [§7.4](#74-detalhe--ota-0x04) | idem |
| `0x05` | Status do AVAIL — ver [§7.5](#75-detalhe--avail-0x05) | idem |
| `0x06` | Restart remoto — responde `"Restarting"` e reinicia após ~300ms | idem |

**Respostas do `0x01` no Industrial com `availEn`:**
- `"RelayStatus:ON"` — máquina aceitou o crédito (AVAIL foi para HIGH)
- `"CreditBusy"` — máquina já estava OCUPADA quando o comando chegou
- `"CreditFail"` — máquina não respondeu após `CREDIT_MAX_ATTEMPTS` tentativas

Resposta de `0x01`/`0x02` no Convencional: `"RelayStatus:ON"` ou `"RelayStatus:OFF"`.

#### 7.4 Detalhe — OTA (`0x04`)

- **Payload:** `0x04 + "url|sha256"` (sha opcional, 64 hex; separador aceita `|`, `\n` ou espaço).
- **Fluxo:** baixa o `.bin` por HTTP(S) → grava com `Update` → valida SHA256 (se informado) → reinicia.
- **Execução:** fora do callback WS (`otaTick()` no loop — é bloqueante).
- **Respostas assíncronas:** `"OTA:QUEUED"` → `"OTA:START"` → `"OTA:OK:restart"` (reinicia),
  ou `"OTA:FAIL:<motivo>"` / `"OTA:BUSY"`.
- **Storage do binário:** **nunca** no Heroku — ver `docs/PLANO-NOVA-VERSAO.md` §4.

#### 7.5 Detalhe — AVAIL (`0x05`)

- Retorna o status atual da leitura AVAIL OUT.
- JSON: `{"type":"avail","livre":bool,"raw":-1/0/1,"sinceMs":ms,"availEn":0/1,"machineMode":0/1}`.
- O campo `"type":"avail"` diferencia da telemetria do `0x03`.

---

## 8. Configuração da Máquina Speed Queen (MDC)

### 8.1 Parâmetros críticos

| Parâmetro | Função | Valor recomendado |
|-----------|--------|-------------------|
| PLSE | Valor de cada pulso (centavos) | = valor total do ciclo |
| PLSNod | Modo de pulso | **1** = pulso único libera ciclo |

- **PLSNod=1**: um pulso libera ciclo completo, extras ignorados ✓
- **PLSNod=128**: múltiplos pulsos somam crédito (padrão fábrica)

### 8.2 Como entrar no MDC (Manual Mode)

1. Pressione NORMAL/HOT + START simultaneamente por 3 segundos.
2. Display mostra `Prog` — navegue com PERM PRESS (↑) / DELICATES (↓).
3. START = confirmar | NORMAL/HOT = sair.

---

## 9. Próximos Passos

- [x] Leitura do AVAIL OUT implementada no ESP32-S3 (`availPin`, INPUT_PULLUP, debounce)
- [x] AVAIL OUT **testado e validado** no ESP32-S3 com pull-up interno, sem resistor externo (ver [§4.5](#45-ligação-validada--sem-resistores-externos-))
- [x] Credit fail-safe implementado no firmware (`availEnabled` + `creditTick` state machine — ver [§6.2.1](#621-credit-fail-safe--avail-industrial))
- [ ] Configurar PLSNod = 1 na máquina
- [ ] **Validar credit fail-safe em campo** (ligar `availEn=1` no wizard e testar ciclo real na Speed Queen)
- [ ] Corrigir discrepância do `availPin` default (C++ usa 4, wizard usa 6 — resolver no código)
- [ ] Definir shield definitivo do ESP32-S3 (GPIOs de relay/start/avail expostos)
- [ ] **Implementar detecção de uso por ficha** (ver [§11](#11-plano--detecção-de-uso-por-ficha-token))

### Arquitetura alvo

```
ESP32-S3
  ├── GPIO startPin (out) ──── H3-7 (START IN)          [pulso de crédito]
  ├── GPIO availPin (in)  ──── H3-4 (AVAIL OUT col)     [pull-up interno ~45kΩ]
  ├── GND               ──── H3-5 (AVAIL EMIT) + H3-2 (COM)
  └── GPIO relayPin (out) ── relé 30A                   [modo Convencional]

Leitura AVAIL OUT: LOW=livre, HIGH=ocupada
Confirmação de sucesso: AVAIL OUT transita de LOW → HIGH após pulso (janela 800ms)
```

---

## 10. Referências

- Manual elétrico: ALPM-39201 (Alliance Laundry Systems)
- Manual programação Quantum: 204370ENR1 (janeiro 2019)
- Schematic AP1/AP2: 807300 (Control Option Wiring Diagram Commercial FLW)

---

## 11. Plano — Detecção de Uso por Ficha (Token)

Objetivo: permitir uso paralelo de ficha física na Speed Queen ao mesmo tempo que nosso
sistema está ativo. Quando alguém insere uma ficha, o IOT detecta a mudança externa do
AVAIL e notifica o backend, que persiste o estado `token_in_use` na máquina e o expõe ao
frontend pelo poll existente de 30s.

### 11.1 Princípio — campo dedicado, `is_in_use` INTOCADO

- **`is_in_use` continua significando só "uso pelo nosso sistema"** (com `UsageHistory`,
  crédito, transação). **Ficha NÃO seta `is_in_use`.**
- Ficha mora num campo próprio e **persistente**: `Machines.token_in_use` (coluna nova).
- O front trata `token_in_use` como "ocupada" só na renderização (ícone/ordem/clique).

**Fonte da verdade = backend.** Quem sabe se uma ocupação (AVAIL=HIGH) é *nossa* ou de
*ficha* é o backend, porque o ESP, após reboot, não lembra se foi o `creditTick` quem ligou.
Regra ao receber sinal de AVAIL ocupado do firmware:

| AVAIL | `is_in_use` (DB) | Conclusão |
|-------|------------------|-----------|
| ocupado | `1` | uso nosso → **ignora** (não marca ficha) |
| ocupado | `0` | **ficha** → `token_in_use = 1` |
| livre | — | `token_in_use = 0` |

### 11.2 Por que NÃO reusar `is_in_use` (a armadilha do gerenciador)

`gerenciarEstadosECreditos` (`gerenciadorMaquina.js`) roda a cada 60s sobre toda máquina
com `is_in_use=1` e busca `getLastMachineUsage()` → `getLastUsageByMachine` retorna o
**último registro de todos os tempos** (`ORDER BY id DESC LIMIT 1`, sem filtro de uso aberto).
Se setássemos `is_in_use=1` para a ficha, o gerenciador casaria a ficha com o registro do
**usuário anterior** e, dependendo do tipo:

- `ConvencionalManualHora` → **desconta crédito do usuário antigo a cada minuto**.
- `ConvencionalAutomaticoCiclo` → cria **transação no nome do usuário antigo** e desliga.
- `IndustrialAutomaticoCiclo` → push de "ciclo finalizado" para o **usuário errado**.

Mantendo a ficha fora do `is_in_use`, a primeira linha do loop
(`if (!machine.is_in_use) continue;`) **pula a máquina-ficha automaticamente** →
`gerenciarEstadosECreditos` **não precisa de NENHUMA alteração** e não há risco de cobrança.

### 11.3 Banco de dados — migration

```sql
ALTER TABLE Machines ADD COLUMN token_in_use TINYINT NOT NULL DEFAULT 0;
```

`getMachinesByBuilding` no model faz `SELECT * FROM Machines` → a coluna nova **já flui para
o front de graça**, sem mexer no `machinesController.js`.

### 11.4 Firmware (`esp32s3.ino` e `esp32c3.ino` — mesma mudança nos dois)

**(a) Transição em tempo real** — em `readAvailTick()`, salvar o estado anterior **antes** de
atualizar e emitir na transição quando `availEnabled && creditState == CR_IDLE`:

```cpp
void readAvailTick() {
  int reading = digitalRead(availPin);
  if (reading != availReading) {
    availReading = reading;
    availLastMs  = millis();
  }
  if (reading != availStable && (millis() - availLastMs) >= AVAIL_DEBOUNCE_MS) {
    int prevStable = availStable;          // salva antes de atualizar
    availStable     = reading;
    availStableAtMs = millis();

    // creditState == CR_IDLE garante que não foi o nosso creditTick quem ativou.
    // O backend ainda cruza com is_in_use antes de marcar ficha (ver §11.1).
    if (availEnabled && isWebSocketConnected && creditState == CR_IDLE) {
      if (prevStable == LOW  && availStable == HIGH) webSocket.sendTXT("TokenInserted");
      else if (prevStable == HIGH && availStable == LOW)  webSocket.sendTXT("TokenFinished");
    }
  }
}
```

**(b) Snapshot na reconexão** — em `WStype_CONNECTED`, logo após enviar `ID:`/`NID:`, reportar
o estado atual do AVAIL (resolve ficha iniciada/encerrada enquanto o WS estava caído):

```cpp
if (availEnabled && creditState == CR_IDLE) {
  webSocket.sendTXT(availLivre() ? "TokenFinished" : "TokenInserted");
}
```

O backend cruza com `is_in_use` (§11.1), então um `TokenInserted` de uma máquina que está em
uso legítimo nosso (`is_in_use=1`) vira no-op — sem falso-positivo.

**Mensagens novas no protocolo WS (ESP32-S3 e ESP32-C3, texto):**

| Mensagem | Quando | Condição no firmware |
|----------|--------|----------------------|
| `"TokenInserted"` | AVAIL LOW→HIGH externo, ou snapshot ocupado no connect | `availEnabled && creditState==CR_IDLE` |
| `"TokenFinished"` | AVAIL HIGH→LOW externo, ou snapshot livre no connect | `availEnabled && creditState==CR_IDLE` |

### 11.5 Backend — `websocket.js`

```js
const machinesModel = require('./models/machineModel');

// Em ws.on('message'), após o bloco OTA:
} else if (messageString === 'TokenInserted') {
  const nodeId = getConnectionNodeId(ws);
  if (nodeId !== 'unknown') {
    // Cruzamento (§11.1): só marca ficha se NÃO houver uso nosso ativo.
    machinesModel.getMachineByIdNodeMcu(nodeId)
      .then((m) => {
        if (m && !m.is_in_use) return machinesModel.setTokenInUse(nodeId, 1);
      })
      .catch((e) => console.error('[WS] TokenInserted falhou:', e && e.message));
  }
} else if (messageString === 'TokenFinished') {
  const nodeId = getConnectionNodeId(ws);
  if (nodeId !== 'unknown') {
    machinesModel.setTokenInUse(nodeId, 0)
      .catch((e) => console.error('[WS] TokenFinished falhou:', e && e.message));
  }
}
```

> **Nota:** `TokenFinished` limpa incondicionalmente. Quando dispara no fim de um uso NOSSO
> (AVAIL livre, `creditState==IDLE`), `token_in_use` já era `0` → no-op inofensivo.

### 11.6 Backend — `machineModel.js` (nova função)

```js
const setTokenInUse = async (idNodemcu, value) => {
  const [result] = await connection.execute(
    'UPDATE Machines SET token_in_use = ? WHERE idNodemcu = ?',
    [value ? 1 : 0, idNodemcu]
  );
  return result.affectedRows > 0;
};
// + exportar setTokenInUse
```

### 11.7 Auditoria de `is_in_use` — o trade-off do campo separado

Como a ficha **não** seta `is_in_use`, todo código que lê `is_in_use` como "ocupada/indisponível"
passa a ver a máquina-ficha como **livre**. Regra para classificar cada uso:

- **Semântica "ocupada fisicamente"** (bloquear ligar, exibir ocupada, contar indisponível)
  → precisa virar `is_in_use || token_in_use`.
- **Semântica "uso NOSSO" / pago** (cobrança, ciclo, reforço, "por mim", crédito, end_time)
  → **deixar como está** (a ficha não é nosso uso; já fica de fora corretamente).

Itens "por mim"/`user_id` e `end_time`/cronômetro já ficam seguros sozinhos: a ficha não tem
`user_id` nem `end_time`, então `isInUseByMe`/`getAvailPhase` já retornam falso/null.

### 11.8 Backend — bloquear novas ativações (`gerenciadorMaquina.js`)

Ponto crítico: hoje `is_in_use` barra religar a máquina. A ficha precisa barrar também,
senão o sistema tentaria pulsar uma máquina já ocupada por ficha. `getMachineById` faz
`SELECT *` → já traz `token_in_use`, sem mudança no model.

**`ligarMaquina` (~linha 212) — ANTES:**
```js
if (machine.is_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina já está ligada!")
}
```
**DEPOIS:**
```js
if (machine.is_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina já está ligada!")
}
if (machine.token_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina em uso por ficha!")
}
```

**`ligarMaquinaIndustrial` (~linha 317) — ANTES:**
```js
if (machine.is_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina já está ligada!")
}
```
**DEPOIS:**
```js
if (machine.is_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina já está ligada!")
}
if (machine.token_in_use) {
    return Utilidades.tratarBadRequest(res, "Máquina em uso por ficha!")
}
```

> No caminho AVAIL o `creditStart` ainda devolveria `CreditBusy`, mas barrar antes com
> mensagem clara é melhor UX e evita pulso desnecessário na máquina.

### 11.9 Backend — o que NÃO mudar (confirmação da auditoria)

| Local | Linha | Por que deixar |
|-------|-------|----------------|
| `gerenciarEstadosECreditos` | 33 | `if (!machine.is_in_use) continue;` — ficha (is_in_use=0) já é pulada → **sem cobrança** |
| `reforçarSinalParaLigar` | 485 | `if (!machine.is_in_use)` — reforço é só de uso nosso; ficha não se reforça |
| `websocket.js` relay-sync | 192 | `cmd = is_in_use ? 0x01 : 0x02` só roda p/ **convencional**; ficha é Speed Queen/AVAIL → não entra |
| `machineModel.getAllMachines` | 21 | `M.is_in_use = 1` no JOIN é sobre uso pago → correto manter |

Opcional (precisão da IA do chat, não bloqueante): `chatController.js:133` e `PromptChat.js`
(`is_in_use ? 'em uso' : 'disponível'`) podem considerar `token_in_use` para o assistente
descrever a máquina-ficha como ocupada.

### 11.10 Frontend — `machines.ts` (interface + helpers)

Adicionar o campo à interface `Machine`:
```ts
token_in_use?: number;  // 1 = em uso por ficha (não via nosso sistema)
```

> **Onde colocar os helpers:** os métodos abaixo são por-componente (cada tela tem os seus).
> `isTokenInUse` é a verificação central; replicar nos componentes que precisarem (ou extrair
> para um util compartilhado, se preferir). `is_in_use` é sempre `0` na ficha — o `&& !is_in_use`
> é redundante mas deixa a intenção explícita.

```ts
isTokenInUse(m: Machine): boolean { return !!m.token_in_use && !m.is_in_use; }
isOccupied(m: Machine): boolean   { return !!m.is_in_use || !!m.token_in_use; }
```

### 11.11 Frontend (A) — `disponibilidade-maquinas.component.ts`

Adicionar o helper `isTokenInUse` (acima) e alterar os métodos. Em cada um, inserir o ramo
da ficha **logo antes** do ramo genérico `if (machine.is_in_use)`.

**`getStatusLabel()`** — antes de `if (machine.is_in_use) return 'Em uso';`:
```ts
    if (this.isInUseByMe(machine)) return 'Em uso por mim';
    if (this.isTokenInUse(machine)) return 'Em uso (ficha)';      // NOVO
    if (this.getAvailPhase(machine) === 'clothes') return 'Roupas dentro';
    if (machine.is_in_use) return 'Em uso';
    return 'Livre';
```

**`getStatusClass()`** — antes de `if (machine.is_in_use) return 'in-use';`:
```ts
    if (this.isTokenInUse(machine)) return 'in-use-token';         // NOVO
```

**`getStatusIcon()`** — antes de `if (machine.is_in_use) return 'autorenew';`:
```ts
    if (this.isTokenInUse(machine)) return 'toll';                 // NOVO
```

**`getSubtitle()`** — antes de `if (machine.is_in_use) return 'Máquina em uso';`:
```ts
    if (this.isTokenInUse(machine)) return 'Máquina em uso por ficha';   // NOVO
```

**`getMachineIcon()`** — antes de `if (machine.is_in_use) return '...EmUso.svg';`:
```ts
    if (this.isTokenInUse(machine)) return '../../../assets/images/maquinaLavaEmUso.svg'; // NOVO
```

**`isAvailable()`** — ANTES:
```ts
    return !machine.is_in_use && !machine.manutencao && !!machine.isConnected;
```
DEPOIS:
```ts
    return !machine.is_in_use && !machine.token_in_use && !machine.manutencao && !!machine.isConnected;
```
(Isso já torna a ficha **não clicável**, pois `isClickable()` depende de `isAvailable()`.)

**`orderMachines()` — rank** — adicionar a ficha como "em uso por outro" (rank 3):
```ts
    if (this.isAvailable(m))                       return 1;
    if (m.agendamento?.isOwner)                    return 2;
    if (this.isTokenInUse(m))                      return 3;  // NOVO (junto com em uso por outro)
    if (m.is_in_use)                               return 3;
```

**`isCycleAnimating()`** — sem mudança: ficha tem `is_in_use=0` e sem `end_time` → já não anima.

**CSS (`disponibilidade-maquinas.component.css`)** — nova classe `in-use-token`
(sugestão: âmbar, similar ao agendamento). Espelhar as regras de `.in-use`/`.agendado`.

### 11.12 Frontend (B) — `admin-panel/machines-control`

Adicionar `isTokenInUse` ao componente e alterar (inserir o ramo da ficha **após** o
`if (machine.is_in_use)`, pois no admin o offline tem prioridade só sobre o livre):

**`getStatusClass()`:**
```ts
    if (machine.is_in_use) return machine.isConnected ? 'in-use' : 'in-use-offline';
    if (this.isTokenInUse(machine)) return 'in-use-token';        // NOVO
```
**`getStatusIcon()`:**
```ts
    if (machine.is_in_use) return 'autorenew';
    if (this.isTokenInUse(machine)) return 'toll';                // NOVO
```
**`getStatusLabel()`:**
```ts
    if (machine.is_in_use) return 'Em uso';
    if (this.isTokenInUse(machine)) return 'Em uso (ficha)';      // NOVO
```
**`getSubtitle()`** — adicionar após o bloco `if (machine.is_in_use) { ... }`:
```ts
    if (this.isTokenInUse(machine)) return 'Em uso por ficha';    // NOVO
```
**`getCardClass()`:**
```ts
    if (machine.is_in_use) return 'uso';
    if (this.isTokenInUse(machine)) return 'uso';                 // NOVO (ou 'uso-token')
```
**`sortedMachines()` — priorityOrder:**
```ts
        if (m.is_in_use) return 0;
        if (m.token_in_use) return 0;                             // NOVO (ocupada no topo)
```

**HTML (`machines-control.component.html`):**
- Badge (após o `*ngIf="machine.is_in_use"`):
```html
<span *ngIf="machine.token_in_use && !machine.is_in_use" class="mc-badge mc-badge-uso">Ficha</span>
```
- Badge "Livre" — adicionar `&& !machine.token_in_use` na condição (l.96):
```html
<span *ngIf="!machine.is_in_use && !machine.token_in_use && (machine.manutencao ?? 0) !== 1 && machine.isConnected"
      class="mc-badge-livre-only">Livre</span>
```
- Botão Ligar (industrial e convencional): adicionar `&& !machine.token_in_use` para não
  oferecer "Ligar" numa máquina ocupada por ficha.

### 11.13 Frontend (C) — `qr-code-scanner` (CRÍTICO)

É onde o usuário cai ao escanear/clicar. Como a ficha tem `is_in_use=0`, sem tratamento a
tela ofereceria **"LIGAR"** e o backend recusaria depois (§11.8). Melhor barrar na própria tela.

A tela já tem o mecanismo `showMachineUnavailable` + `machineUnavailableReason`
(usado p/ `manutencao`/`fora-horario`/`desconectada`). Adicionar o motivo `'ficha'`:

**No `continuarComMaquina()` (ou no ponto onde checa manutenção/horário/desconexão), adicionar:**
```ts
if (machine?.token_in_use && !machine.is_in_use) {
  this.selectedMachine = machine;
  this.showMachineUnavailable = true;
  this.machineUnavailableReason = 'ficha';   // novo motivo
  return;
}
```
- Adicionar o texto do motivo `'ficha'` no modal/HTML de máquina indisponível
  ("Máquina em uso por ficha").
- Defensivo (caso chegue ao banner): no `.html`, onde decide `banner-ligar`/`EM USO`,
  tratar `token_in_use` como ocupada para não renderizar "LIGAR".

### 11.14 Frontend (D) — `status-maquinas`

`is_in_use` é lido como "ocupada" em filtros, classes e no guard de clique. Adicionar
`token_in_use` em todos os pontos de **disponibilidade**:

**`.ts` — filtros de máquinas livres (l.356, 401, 444) — ANTES:**
```ts
... .filter(m => !m.is_in_use && !m.manutencao && m.isConnected) ...
```
DEPOIS:
```ts
... .filter(m => !m.is_in_use && !m.token_in_use && !m.manutencao && m.isConnected) ...
```

**`isMachineAgendada()` (l.382)** — manter; mas garantir que a ficha não apareça como livre:
o tratamento dos filtros/classes acima já cobre. (Ficha não é agendamento.)

**`.html`** — nas classes `free-machine`/`in-use` e no guard de clique
`machine.is_in_use || machine.manutencao || isMachineAgendada(...) ? null : goToQRCode()`,
adicionar `|| machine.token_in_use`. No texto de status, exibir `'Ficha'` quando
`token_in_use && !is_in_use`.

### 11.15 Frontend (E) — `lavanderia-easy-use`

**`refreshFilteredMachines()` (l.218–232)** — os filtros de "iniciar" (livres) precisam excluir
a ficha; o filtro de "encerrar" (`is_in_use && user_id===uid`) é uso nosso → **não mexer**.

ANTES (todas as linhas de "livre"):
```ts
this.filteredMachines = this.machines.filter(m => !m.is_in_use && m.isConnected !== false && !m.manutencao);
```
DEPOIS:
```ts
this.filteredMachines = this.machines.filter(m => !m.is_in_use && !m.token_in_use && m.isConnected !== false && !m.manutencao);
```
**`.html` (l.96–97)** — opcional: mostrar "Em uso" (ficha) em vez de "Disponível" quando
`token_in_use`.

### 11.16 Frontend (F) — `home-screen`

**Contagem de livres (l.170-172) — ANTES:**
```ts
const livres = normalized.filter(m =>
  !m.is_in_use && !m.manutencao && m.isConnected
);
```
DEPOIS:
```ts
const livres = normalized.filter(m =>
  !m.is_in_use && !m.token_in_use && !m.manutencao && m.isConnected
);
```
`maquinaEmUsoPorMim` (usa `user_id`) → **não mexer** (ficha não tem `user_id`).

### 11.17 Frontend — deixar como está (ficha já segura)

| Ponto | Por quê |
|-------|---------|
| `isInUseByMe` / "por mim" | depende de `user_id`; ficha não tem → já retorna falso |
| `getAvailPhase` / cronômetro / `end_time` | ficha não tem `end_time` → retorna `null` |
| `gerenciadorMaquinas.ts` `processMachine` | decisão ligar/desligar é coberta pelo bloqueio do `qr-code-scanner` |
| Modais de agendamento, fluxo de reforço | semântica de uso nosso |

### 11.18 Fluxo completo

```
Ficha inserida
  → Speed Queen: AVAIL OUT LOW → HIGH
  → ESP (readAvailTick, debounce 50ms, creditState=IDLE, availEnabled=true)
  → ESP envia WS "TokenInserted"
  → Backend: getMachineByIdNodeMcu → is_in_use==0 → setTokenInUse(nodeId, 1)
  → DB: Machines.token_in_use = 1   (persiste; sobrevive a restart do dyno)
  → Frontend poll 30s: SELECT * → machine.token_in_use = 1
  → UI (todas as telas): "Em uso (ficha)" / ocupada / não clicável
  → Backend bloqueia tentativa de ligar (§11.8)

Ciclo termina / roupas retiradas
  → AVAIL OUT HIGH → LOW
  → ESP envia WS "TokenFinished"
  → Backend: setTokenInUse(nodeId, 0)
  → Frontend poll 30s: machine.token_in_use = 0 → "Livre"

Reconexão do device (WS caiu durante a ficha)
  → WStype_CONNECTED → ESP envia snapshot ("TokenInserted"/"TokenFinished")
  → Backend cruza com is_in_use → re-sincroniza token_in_use (self-healing)
```

### 11.19 Edge cases conhecidos

- **`TokenFinished` perdido com device offline:** se a ficha encerra com o WS caído, o estado
  fica preso em `1` até a próxima reconexão — o snapshot do connect (§11.4b) corrige.
  Hardening opcional (post-MVP): job lê AVAIL (0x05) das máquinas com `token_in_use=1` e
  limpa as livres.
- **Crédito nosso que falhou mas a máquina ligou tarde** (`CreditFail`, `is_in_use=0`, AVAIL
  sobe depois): será marcada como ficha. Aceitável — a máquina está fisicamente ocupada.

### 11.20 Alternativa descartada — inferir ficha pelo timestamp do `lastUsage`

Ideia considerada: setar `is_in_use=1` na ficha e distinguir "ficha vs uso nosso" comparando
`now` com o `start_time`/`end_time` do `lastUsage` ("se está em uso mas o horário é depois do
último registro, é ficha"). **Descartada** por dois motivos:

1. **Não evita mexer no gerenciador:** com `is_in_use=1`, `gerenciarEstadosECreditos` deixa de
   pular a máquina e cobraria o usuário antigo — seria preciso embutir a comparação de tempo
   dentro dele de qualquer forma.
2. **Colisão com a fase "roupas dentro" das máquinas `usa_avail`:** uma lavagem nossa que
   terminou o ciclo mas aguarda retirada da roupa tem **assinatura idêntica** a uma ficha
   (`is_in_use=1` na nossa, `end_time` no passado, AVAIL ocupado). Nenhum limiar de tempo
   separa os dois — e como **só** máquinas `usa_avail` têm AVAIL, o heurístico falharia
   justamente em 100% do hardware onde a ficha é detectável, podendo classificar a lavagem de
   um morador como ficha e quebrar a liberação automática por retirada de roupa.

A coluna dedicada (`token_in_use`) é ortogonal a timers e a `is_in_use`, então não sofre dessa
ambiguidade.

### 11.21 Checklist de implementação (ordem sugerida)

1. [ ] **DB:** `ALTER TABLE Machines ADD COLUMN token_in_use TINYINT NOT NULL DEFAULT 0;` (§11.3)
2. [ ] **Firmware** `esp32s3.ino` + `esp32c3.ino`: transição em `readAvailTick` (§11.4a) + snapshot no `WStype_CONNECTED` (§11.4b)
3. [ ] **Backend** `machineModel.js`: `setTokenInUse()` + export (§11.6)
4. [ ] **Backend** `websocket.js`: handlers `TokenInserted`/`TokenFinished` com cruzamento `is_in_use` (§11.5)
5. [ ] **Backend** `gerenciadorMaquina.js`: bloquear ligar em `ligarMaquina` e `ligarMaquinaIndustrial` (§11.8)
6. [ ] **Frontend** `machines.ts`: campo `token_in_use` + helper `isTokenInUse` (§11.10)
7. [ ] **Frontend** `disponibilidade-maquinas` (.ts + .css `in-use-token`) (§11.11)
8. [ ] **Frontend** `machines-control` (.ts + .html) (§11.12)
9. [ ] **Frontend** `qr-code-scanner` — motivo `'ficha'` (§11.13)
10. [ ] **Frontend** `status-maquinas` (§11.14)
11. [ ] **Frontend** `lavanderia-easy-use` (§11.15)
12. [ ] **Frontend** `home-screen` (§11.16)
13. [ ] (Opcional) **Backend** `chatController.js` / `PromptChat.js` (§11.9)
14. [ ] **NÃO tocar:** `gerenciarEstadosECreditos`, `reforçarSinalParaLigar`, relay-sync do WS (§11.9, §11.17)

**Teste de fumaça (após implementar):** ligar `availEn=1` no wizard de uma máquina Speed Queen,
inserir ficha física → conferir `TokenInserted` no log do backend, `token_in_use=1` no banco, e
"Em uso (ficha)" no app; encerrar o ciclo → `TokenFinished` → `token_in_use=0` → "Livre".
