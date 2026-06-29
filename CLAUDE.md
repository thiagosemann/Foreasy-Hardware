# Foreasy Hardware — Contexto do Projeto

Produto próprio (hardware + firmware) para automação de lavadoras, substituindo
soluções concorrentes (VMlav, VENDPAGO). **Cada placa roda um firmware dedicado a
um único sistema** — não existe mais seletor `machineMode`: o tipo é definido pelo
firmware gravado.

## Índice
- [1. Os dois sistemas](#1-os-dois-sistemas)
- [2. Modelos de hardware](#2-modelos-de-hardware)
- [3. Lógica comum a todos os firmwares](#3-lógica-comum-a-todos-os-firmwares)
- [4. Específico de cada firmware](#4-específico-de-cada-firmware)
- [5. Protocolo WebSocket](#5-protocolo-websocket)
- [6. Speed Queen Quantum 440G — conector H3](#6-speed-queen-quantum-440g--conector-h3)
- [7. Configuração da máquina (MDC)](#7-configuração-da-máquina-mdc)
- [8. Referências](#8-referências)

---

## 1. Os dois sistemas

| Sistema | O que faz | Hardware |
|---------|-----------|----------|
| **Convencional** | Controle direto de energia: o relé liga/desliga a alimentação da máquina. Funciona com qualquer lavadora. | ESP32 + relé 30A |
| **Industrial** | Integração com **Speed Queen** via conector H3: um **pulso** de 100ms no START IN libera o ciclo; o **AVAIL OUT** informa se a máquina está livre/ocupada. | ESP32-C3 / ESP32-S3 (cabo direto) e ESP8266 (pulso via relé) |

> **Industrial = pulso momentâneo** (dispara o crédito e o pino volta ao repouso).
> **Convencional = relé estável** (fica ON até receber OFF).

---

## 2. Modelos de hardware

### Industrial
| MCU | Acionamento | Observações |
|-----|-------------|-------------|
| **ESP32-S3** | Cabo direto no START IN (`startPin`) | **Não usa relé — exclusivamente cabo.** AVAIL OUT, detecção de ficha, OTA |
| **ESP32-C3** Super Mini | Cabo direto no START IN (`startPin`) | **Não usa relé — exclusivamente cabo.** AVAIL OUT, detecção de ficha, OTA |
| **ESP8266** ESP-01S (Modelos 1 e 5) | Pulso via relé controlado por **Serial 9600** (STC15F104W) | Modelo 5: relé aciona SSR de alta corrente |
| **ESP8266** ESP-01S (Modelo 2) | Pulso via relé no **GPIO0** direto | Serial 115200 livre para debug |

### Convencional
| MCU | Acionamento | Observações |
|-----|-------------|-------------|
| **ESP32** (Modelos 3 e 4) | Relé 30A ON/OFF (`relayPin` GPIO2) | Modelo 4: relé aciona SSR externo |

### Arquivos
- `Industrial/ESP32-S3/esp32s3/esp32s3.ino`
- `Industrial/ESP32-C3/esp32c3/esp32c3.ino`
- `Industrial/ESP8266/industrial_serial/industrial_serial.ino`
- `Industrial/ESP8266/industrial_sem_serial/industrial_sem_serial.ino`
- `Convencional/ESP32/esp32/esp32.ino`
- `Convencional/ESP32-S3-MIDEA/midea_iot/midea_iot.ino` (variante Midea — fora do escopo dos 5 acima)

---

## 3. Lógica comum a todos os firmwares

- **WiFi dual + failover sem restart** (rede 1 / rede 2, `wifiSlot`). Conexão
  não-bloqueante (`wifiTick`, timeout 40s, retry 5s). **Credenciais nunca apagadas
  por falha de conexão.**
- **WebSocket**: backoff exponencial 10s→120s; watchdog WS down >5min → failover;
  watchdog global sem WiFi+WS >8min → failover; detecção de zumbi (sem ping/pong
  >5min → reconecta); app ping 30s; `enableHeartbeat(15000, 3000, 2)`;
  `wsRestartEnabled` (reinicia após 1h sem WS, opcional).
- **Identidade no connect**: envia `ID:<nodeId>` se ligado, `NID:<nodeId>` se desligado.
- **AP de configuração**: ativo 10 min após boot (SSID `<nodeId>-AP`, senha
  `12345678`); depois lean mode (AP desliga).
- **Páginas web**: `/config` (wizard) e `/info` (status com auto-refresh); `/` → `/config`.
- **Protocolo WS**: telemetria `0x03` e restart remoto `0x06` em todos (ver [§5](#5-protocolo-websocket)).
- **`bootCount`** incrementado em RAM, gravado só no `/save` (poupa flash/EEPROM).
- **Identidade de firmware** no `0x03`: `chip` (`esp8266`/`esp32`/`esp32s3`/`esp32c3`)
  e `fw` (semver, ex. `1.0.0`) — para auditoria da frota e seleção do binário de OTA.

---

## 4. Específico de cada firmware

### ESP32-S3 e ESP32-C3 (Industrial, cabo)
- **Não usam relé.** Pulso de `PULSE_MS` (100ms) direto no `startPin` (START IN da
  Speed Queen, H3-7), ativo HIGH. WS `0x01` dispara o pulso; `0x02` ignorado.
- **AVAIL OUT** (`availPin`, `INPUT_PULLUP`, debounce 50ms): **LOW=livre, HIGH=ocupada**.
  Exposto em `/status`, `/info` e no WS `0x05`.
- **Credit fail-safe** (flag `availEn`): após o pulso, confirma a transição LOW→HIGH
  em até 800ms; se não confirmar, repulsa (até 3 tentativas, gap 400ms). Máquina de
  estados `creditTick` (`CR_IDLE/PULSE/CONFIRM/GAP`). Respostas assíncronas:
  `RelayStatus:ON` / `CreditFail` / `CreditBusy`. Modelos sem AVAIL: deixar `availEn` OFF.
- **Detecção de ficha** (só com `availEn`): emite `TokenInserted`/`TokenFinished`
  quando o AVAIL muda fora do nosso pulso (`creditState==CR_IDLE`), além de um snapshot
  na reconexão WS. O backend cruza com `is_in_use` antes de marcar ficha.
- **OTA `0x04`** e **status AVAIL `0x05`**. Armazenamento NVS (`Preferences`),
  temperatura interna, wizard de 3 passos (Rede → Servidor → Pinos).
- Diferenças entre placas:
  - **C3**: `ledPin` GPIO8 (LED azul integrado, **ativo LOW**); strapping GPIO2/8/9.
  - **S3**: `ledPin` GPIO4. ⚠️ o default C++ de `availPin` é 4 (colide com o LED) —
    definir/salvar o pino correto pelo wizard (fallback do wizard = GPIO6).

### ESP8266 — `industrial_serial` (Modelos 1 e 5)
- Pulso via relé acionado por **Serial 9600** → STC15F104W:
  `ON = {0xA0,0x01,0x01,0xA2}`, `OFF = {0xA0,0x01,0x00,0xA1}`.
  Serial 100% dedicada ao relé (sem debug serial; logs só em `/info` com AP ativo).
- `relayInvert` (útil para relé NF). **Sem** OTA/AVAIL/ficha (ESP-01S: 1MB de flash,
  sem GPIO livre).
- **EEPROM v4** (`magic=0xF0EA5E01`): ssid/pass/ssid2/pass2/nodeId, relayInvert,
  bootCount, lastResetReason, wsRestartEnabled (+ campo `machineMode` reservado, fixo
  industrial, mantido só para preservar o layout da EEPROM em campo). Erase de setor
  antes de salvar + 3 commits com verificação.
- Modelo 5: o relé aciona um SSR externo de alta corrente.

### ESP8266 — `industrial_sem_serial` (Modelo 2)
- Igual ao `industrial_serial`, mas o pulso sai por **relé no GPIO0** (`digitalWrite`)
  e a **Serial 115200 fica livre para debug**.
- Flags de debug de EEPROM/flash habilitadas; scan WiFi síncrono; GPIO0 é strapping
  pin (cuidado no boot — firmware força LOW no setup).

### ESP32 — `esp32.ino` (Convencional, Modelos 3 e 4)
- Relé 30A ON/OFF no `relayPin` (GPIO2); LED GPIO4. WS `0x01`=ON, `0x02`=OFF.
- `relayMode`: `0`=Normal (segue WS), `1`=Sempre ON, `2`=Sempre OFF (ignora WS e botões).
- `relayInvert`: `false` = ON=HIGH/OFF=LOW (NA) | `true` = ON=LOW/OFF=HIGH (NF).
- **OTA `0x04`** (adicionado). **Sem** pulso/AVAIL/ficha.
- Armazenamento NVS; temperatura interna; páginas extras além de `/config` e `/info`:
  `/relay`, `/relay/on|off|config`, `/nodeid`, `/savenodeid`, `/wifistatus`, `/wsstatus`.
- Modelo 4: o relé aciona um SSR externo.

---

## 5. Protocolo WebSocket

Mensagens binárias (1 byte de comando); resposta sempre em texto. `0x04` só onde há
flash para imagem dupla; `0x05` só onde há sensor AVAIL.

| Byte | Industrial C3/S3 | Industrial ESP8266 | Convencional ESP32 |
|------|------------------|--------------------|--------------------|
| `0x01` | Pulso START IN (100ms) ¹ | Pulso relé (100ms) | Relé ON (se `relayMode`=0) |
| `0x02` | ignorado | ignorado | Relé OFF (se `relayMode`=0) |
| `0x03` | Telemetria JSON ² | Telemetria JSON ² (sem `temp`) | Telemetria JSON ² |
| `0x04` | OTA | — (sem flash) | OTA |
| `0x05` | Status AVAIL ³ | — | — |
| `0x06` | Restart remoto | Restart remoto | Restart remoto |

¹ Sem `availEn`: responde `RelayStatus:ON` imediatamente. Com `availEn`: resposta
assíncrona via `creditTick` (`RelayStatus:ON` / `CreditBusy` / `CreditFail`).
² `{rssi, ch, heap, block, cpu, uptime, boots, wifiSlot, temp, machineMode, pulse, chip, fw}`
— `machineMode` é fixo (1=industrial, 0=convencional), mantido só por compatibilidade do backend.
³ `{"type":"avail","livre":bool,"raw":-1/0/1,"sinceMs":ms,"availEn":0/1,"machineMode":1}`.

**OTA (`0x04`)**: payload `0x04 + "url|sha256"` (sha opcional, 64 hex; separador `|`,
`\n` ou espaço). Baixa o `.bin` por HTTP(S) → grava com `Update` → valida SHA256 (se
informado) → reinicia. Executa em `otaTick()` (fora do callback WS, pois bloqueia).
Respostas: `OTA:QUEUED → OTA:START → OTA:OK:restart`, ou `OTA:FAIL:<motivo>` / `OTA:BUSY`.
O binário **nunca** fica no Heroku — ver `docs/PLANO-NOVA-VERSAO.md` §4.

**Detecção de ficha (só C3/S3, com `availEn`)** — mensagens de texto:
`TokenInserted` (AVAIL LOW→HIGH externo, ou snapshot ocupado na reconexão) e
`TokenFinished` (HIGH→LOW, ou snapshot livre). O backend cruza com `is_in_use` antes
de classificar como ficha (não toca em `is_in_use`).

---

## 6. Speed Queen Quantum 440G — conector H3

| Pino | Função |
|------|--------|
| H3-7 | START IN — pulso de crédito (optoacoplador H11L1, 100Ω interno) |
| H3-4 | AVAIL COL — saída de status (optoacoplador 4N25) |
| H3-5 | AVAIL EMIT → GND |
| H3-2 | COM — GND de referência |
| H3-3 | +5V externo fornecido pela máquina |

**Ligação validada — sem resistores externos:**
```
startPin ── H3-7 (START IN)        [100Ω interno do H11L1 já limita: ~20mA]
availPin ── H3-4 (AVAIL COL)       [pull-up INTERNO do ESP, ~45kΩ]
GND      ── H3-5 (AVAIL EMIT) + H3-2 (COM)
```
- **AVAIL OUT** ("ON = AVAILABLE"): o transistor conduz quando a máquina está **livre**
  → **LOW = livre, HIGH = ocupada**.
- **START IN**: mínimo 20ms (schematic 807300) / 45ms (ALPM); usamos **100ms** por segurança.

---

## 7. Configuração da máquina (MDC)

- **PLSE** = valor total do ciclo (centavos). **PLSNod = 1** → um pulso libera o ciclo
  completo, extras ignorados (padrão de fábrica é 128, que soma crédito).
- Entrar no MDC: pressione **NORMAL/HOT + START** por 3s → display mostra `Prog`.
  Navegue com PERM PRESS (↑) / DELICATES (↓); START confirma, NORMAL/HOT sai.

---

## 8. Referências

- ALPM-39201 — manual elétrico (Alliance Laundry Systems)
- 204370ENR1 — manual de programação Quantum (jan/2019)
- Schematic 807300 — Control Option Wiring Diagram (Commercial FLW)
