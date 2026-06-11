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

-> ℹ️ Os valores abaixo são os **limites de catálogo** (com pull-up externo / fonte +5V).
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

**Lógica de confirmação após pulso:**
1. Antes: AVAIL OUT = LOW (máquina livre) → ok para enviar pulso.
2. Enviar pulso.
3. Máquina aceita → inicia ciclo → AVAIL OUT vai para HIGH.
4. HIGH após ~500ms = sucesso | LOW após ~500ms = máquina não aceitou → retry.

-### 4.5 Ligação validada — sem resistores externos ✅

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

--

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
  no `startPin`, direto no optoacoplador START IN (Speed Queen H3-7), ativo HIGH — mesma
  técnica do `esp32_avail.ino`. WS `0x01` dispara o pulso; `0x02` ignorado.
- **Convencional** (`machineMode=0`): relé ON/OFF no `relayPin`. WS `0x01`=ON, `0x02`=OFF.
  `relayType`: `0`=NA (ON=HIGH) / `1`=NF (ON=LOW).

##### AVAIL OUT (leitura) — ✅ testado e funcionando
- `availPin` em `INPUT_PULLUP` (pull-up interno, **sem resistor externo** — ver [§4.5](#45-ligação-validada--sem-resistores-externos-)) com debounce 50ms (`readAvailTick()` no loop).
- LOW=livre, HIGH=ocupada. Exposto em `/status`, `/info` e no WS `0x05`.
- Leitura validada (mesma técnica do `esp32_avail.ino`). **Ainda só leitura — não controla nada** (fail-safe `availEn` opcional já implementado, ver [Próximos Passos](#9-próximos-passos)).
#### OTA (Over-The-Air)
- Disparado pelo WS `0x04` (ver [§7.4](#74-detalhe--ota-0x04)).
- Baixa o `.bin` por HTTP(S), grava com `Update`, valida SHA256 e reinicia.
- `FW_VERSION` é reportado no WS `0x03` (campo `fw`) para auditoria da frota.
- Storage do binário **nunca** no Heroku — ver `docs/PLANO-NOVA-VERSAO.md` §4.

#### Pinos e servidor (configuráveis, persistidos na NVS, aplicados no boot)
- Pinos default: `relayPin`=GPIO2, `startPin`=GPIO5, `availPin`=GPIO6, `ledPin`=GPIO4 (fixo).
- Servidor WebSocket (`wsHost`/`wsPort`) editável pelo wizard — default `frst-back-...herokuapp.com:80`.

#### Páginas web (somente duas)
- `/config` — wizard step-by-step (Rede → Servidor → Modo → Pinos), self-contained e mobile.
- `/info` — status com auto-refresh 2s (inclui AVAIL, pinos, servidor).
- `/` redireciona para `/config`.
- Endpoints de apoio: `/config-data`, `/save`, `/scan`, `/status`, `/resetwifi` (= `prefs.clear()`), `/restart`.

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

### 7.1 ESP8266 (Modelos 1, 2, 5)

| Byte | Modo Industrial | Modo Convencional |
|------|----------------|-------------------|
| `0x01` | Pulso relay (100ms) | Relay ON |
| `0x02` | Ignorado | Relay OFF |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, machineMode, pulse, fw` | idem |
| `0x06` | Restart remoto — responde `"Restarting"` e reinicia após ~200ms | idem |

> **Não disponível no ESP-01S:** `0x04` (OTA — 1MB de flash não cabe imagem dupla) e
> `0x05` (AVAIL — sem GPIO livre, UART ocupada pelo relay). Ambos só no ESP32-S3.
> Sem sensor de temperatura interno → o `0x03` do ESP8266 **não** tem campo `temp`.

### 7.2 ESP32 (Modelos 3, 4)

| Byte | Modo Convencional | Modo Industrial |
|------|--------------------|-----------------|
| `0x01` | Relay ON (se `relayMode`=Normal) | Pulso relay (`PULSE_MS`=100ms, se `relayMode`=Normal) |
| `0x02` | Relay OFF (se `relayMode`=Normal) | Ignorado |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse` | idem |

Resposta sempre: `"RelayStatus:ON"` ou `"RelayStatus:OFF"`.

### 7.3 ESP32-S3 (firmware principal)

Mesmos bytes do ESP32, com duas diferenças: **não há `relayMode`** (Sempre ON/OFF removido)
e no **Industrial o pulso sai pelo `startPin` (GPIO START IN), não pelo relé**.

| Byte | Modo Convencional | Modo Industrial |
|------|--------------------|-----------------|
| `0x01` | Relay ON | Pulso no START IN (`PULSE_MS`=100ms) |
| `0x02` | Relay OFF | Ignorado |
| `0x03` | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, fw` | idem |
| `0x04` | OTA — ver [§7.4](#74-detalhe--ota-0x04) | idem |
| `0x05` | Status do AVAIL — ver [§7.5](#75-detalhe--avail-0x05) | idem |
| `0x06` | Restart remoto — responde `"Restarting"` e reinicia após ~300ms | idem |

Resposta de `0x01`/`0x02`: `"RelayStatus:ON"` ou `"RelayStatus:OFF"`.

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

- [x] Leitura do AVAIL OUT implementada no ESP32-S3 (`availPin`, INPUT_PULLUP, debounce) — **só leitura, ainda não usada para controle**
-- [x] AVAIL OUT **testado e validado** no ESP32-S3 com pull-up interno, sem resistor externo (ver [§4.5](#45-ligação-validada--sem-resistores-externos-))
- [ ] Configurar PLSNod = 1 na máquina
- [ ] Usar o AVAIL no controle: pulso → confirma AVAIL OUT (LOW→HIGH ~500ms) → sucesso/retry (lógica `availEn` já no firmware; validar em campo)
 [ ] Definir shield definitivo do ESP32-S3 (GPIOs de relay/start/avail expostos)
-### Arquitetura alvo

```
ESP8266
  ├── GPIO (out) → 150Ω → H3-7 (START IN)             [pulso de crédito]
  ├── GPIO (in)  ← pull-up 10kΩ → 3.3V ← H3-4 (AVAIL OUT)  [status]
  ├── H3-3 (+5V) → alimentação do shield
  └── H3-2 (COM) → GND

Leitura AVAIL OUT: LOW=livre, HIGH=ocupada
Confirmação de sucesso: AVAIL OUT transita de LOW → HIGH após pulso
```

---

## 10. Referências

ito]
  ├── GPIO (in)  ← pull-up 10kΩ → 3.3V ← H3-4 (AVAIL OUT)  [status]
  ├── H3-3 (+5V) → alimentação do shield
  └── H3-2 (COM) → GND

Leitura AVAIL OUT: LOW=livre, HIGH=ocupada
Confirmação de sucesso: AVAIL OUT transita de LOW → HIGH após pulso
```

---

## Referências
- Manual elétrico: ALPM-39201 (Alliance Laundry Systems)
- Manual programação Quantum: 204370ENR1 (janeiro 2019)
- Schematic AP1/AP2: 807300 (Control Option Wiring Diagram Commercial FLW)
