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
- [8. Gravação (build + upload)](#8-gravação-build--upload)
- [9. Referências](#9-referências)

---

## 1. Os dois sistemas

| Sistema | O que faz | Hardware |
|---------|-----------|----------|
| **Convencional** | Controle direto de energia: o relé liga/desliga a alimentação da máquina. Funciona com qualquer lavadora. | ESP32 + relé 30A; ESP8266 + relé 10A acionando módulo 30A |
| **Industrial** | Integração com **Speed Queen** via conector H3: um **pulso** de 100ms no START IN libera o ciclo; o **AVAIL OUT** informa se a máquina está livre/ocupada. | ESP32-C3 / ESP32-S3 (cabo direto) e ESP8266 (pulso via relé) |

> **Industrial = pulso momentâneo** (dispara o crédito e o pino volta ao repouso).
> **Convencional = relé estável** (fica ON até receber OFF).

---

## 2. Modelos de hardware

### Industrial
| MCU | Acionamento | Observações |
|-----|-------------|-------------|
| **ESP32-S3** N16R8 (DevKit) | Cabo direto no START IN (`startPin`) | **Não usa relé — exclusivamente cabo.** AVAIL OUT, detecção de ficha, OTA. Placa da migração que substitui o C3 do lote ruim |
| **ESP32-C3** Super Mini | Cabo direto no START IN (`startPin`) | **Não usa relé — exclusivamente cabo.** AVAIL OUT, detecção de ficha, OTA |
| **ESP8266** ESP-01S (Modelos 1 e 5) | Pulso via relé controlado por **Serial 9600** (STC15F104W) | Modelo 5: relé aciona SSR de alta corrente |
| **ESP8266** ESP-01S (Modelo 2) | Pulso via relé no **GPIO0** direto | Serial 115200 livre para debug |

### Convencional
| MCU | Acionamento | Observações |
|-----|-------------|-------------|
| **ESP32** (Modelos 3 e 4) | Relé 30A ON/OFF (`relayPin` GPIO2) | Modelo 4: relé aciona SSR externo |
| **ESP8266** ESP-01S (Modelo 6) | Relé 10A do shield no **GPIO0** aciona um **módulo de relé 30A** externo | Serial 115200 livre para debug; sem OTA e sem `temp` |

### Arquivos
- `Industrial/ESP32-S3/esp32s3/esp32s3.ino`
- `Industrial/ESP32-C3/esp32c3/esp32c3.ino`
- `Industrial/ESP8266/industrial_serial/industrial_serial.ino`
- `Industrial/ESP8266/industrial_sem_serial/industrial_sem_serial.ino`
- `Convencional/ESP32/esp32/esp32.ino`
- `Convencional/ESP8266/esp8266/esp8266.ino`
- `Convencional/ESP32-S3-MIDEA/midea_iot/midea_iot.ino` (variante Midea — fora do escopo dos 5 acima)

---

## 3. Lógica comum a todos os firmwares

- **WiFi dual + failover sem restart** (rede 1 / rede 2, `wifiSlot`). Conexão
  não-bloqueante (`wifiTick`, timeout 40s, retry 5s). **Credenciais nunca apagadas
  por falha de conexão.**
- **WebSocket**: backoff exponencial 10s→120s; watchdog WS down >5min → failover;
  watchdog global sem WiFi+WS >8min → failover; detecção de zumbi (sem ping/pong
  >5min → reconecta); app ping 30s; `enableHeartbeat(15000, 3000, 2)`;
  `wsRestartEnabled` (reinicia após 30min sem WS, opcional).
- **Identidade no connect**: envia `ID:<nodeId>` se ligado, `NID:<nodeId>` se desligado.
- **AP de configuração**: ativo 10 min após boot (SSID `<nodeId>-AP`, senha
  `12345678`); depois lean mode (AP desliga).
- **Páginas web**: `/config` (wizard) e `/info` (status com auto-refresh); `/` → `/config`.
- **Protocolo WS**: telemetria `0x03` e restart remoto `0x06` em todos (ver [§5](#5-protocolo-websocket)).
- **`bootCount`** incrementado em RAM, gravado só no `/save` (poupa flash/EEPROM).
  Zerado quando o `/save` vem do passo final do wizard (`wizard=1` — no Midea,
  único save do firmware, sempre zera): reinícios do bench/teste durante a
  configuração não devem contar como reinícios em campo.
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
    Potência de TX configurável (NVS `txpower`) e peça virgem nasce em **15 dBm** —
    fail-safe para o lote de placas do laudo, que a 19,5 dBm não sobe nem o próprio AP.
  - **S3 (N16R8)**: `esp32s3.ino` é o porte do C3 1.3.0 — mesma lógica, mesmo protocolo,
    mesmas telas; **corrija bug de lógica nos dois**. Padrões: `startPin` GPIO42,
    `availPin` GPIO40, `ledPin` GPIO48. Os pinos vivem na NVS, então trocar o padrão
    só vale para peça nova — peça já configurada mantém o que tem gravado.
    - **LED** (`ledMode`, NVS): 0=desligado, 1=comum ativo HIGH, 2=comum ativo LOW,
      3=**RGB WS2812** (padrão). O DevKit não tem LED monocromático de usuário, só o
      endereçável — e algumas revisões o trazem no GPIO38 em vez do 48, daí pino e modo
      serem configuráveis no `/admin`. Verde = WS conectado, vermelho = sem WS.
    - **`gpioLivre()`** recusa no `/save` os GPIOs 19/20 (USB nativa) e 22–37 (flash SPI
      + PSRAM octal). Pino errado aí é peça que não dá boot — conserto presencial, nem
      OTA alcança. Livres: 0–18, 21 e 38–48 (0/3/45/46 strapping e 43/44 UART0 passam,
      mas evite).
    - **Sem controle de potência de TX** — nem no `esp32s3.ino`, nem no `esp32s3_ble.ino`.
      Opera sempre na potência cheia do core (19,5 dBm): esta placa sustenta o pico de
      corrente do TX, então o ajuste do C3 aqui não teria caso de uso, e o fallback de
      potência do watchdog dá lugar ao failover de rede. Sobra o campo `txp` no `0x03` e
      no `/status`, que é leitura de volta do rádio e serve só para conferência.
    - **Build — a flash marcada tem de bater com o chip real.** Estes DevKits são
      anunciados como N16R8 e chegam **N8R2**; marcar 16MB numa placa de 8MB grava
      sem erro e a peça entra em pânico em loop no bootloader (`Detected size(8192k)
      smaller than the size in the binary image header`). Confira com
      `esptool --port <COM> flash-id`.
      - **N8R2** (em uso): Flash `8MB` + `8M with spiffs (3MB APP/1.5MB SPIFFS)`
      - **N16R8**: Flash `16MB` + `16M Flash (3MB APP/9.9MB FATFS)`
      - PSRAM Disabled nas duas. A partição precisa de **dois slots de app** ou não há
        OTA `0x04` — `huge_app`, `minimal` e os `no_ota*` têm um só.
    - `Serial.setTxTimeoutMs(0)` sob `ARDUINO_USB_CDC_ON_BOOT`: com a USB nativa
      enumerada e ninguém lendo — peça ligada num carregador — cada `print` seguraria
      o loop.

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

### ESP8266 — `esp8266.ino` (Convencional, Modelo 6)
- Mesma lógica do convencional do ESP32 (`relayMode` 0/1/2, `relayInvert`, `ID:`/`NID:`
  no connect, páginas `/wizard`, `/admin`, `/relay`, `/info`, `/nodeid`,
  `/wifistatus`, `/wsstatus`), portada para ESP-01S.
- O relé de **10A do shield (GPIO0)** aciona um **módulo de relé 30A** externo — a
  carga nunca passa pelo shield.
- **Sem OTA `0x04`** (1MB de flash) e **sem `temp`** no `0x03` (não há sensor interno).
- **EEPROM própria**: `magic=0xF0EA5E0C`, v1 — o mesmo layout do industrial + `relayMode`.
  Magic diferente de propósito: gravar este firmware sobre uma placa industrial cai
  nos defaults e exige reconfiguração pelo AP.
- **GPIO0 é strapping pin**: o setup força LOW (relé desligado, lógica NA) e os
  watchdogs só fazem failover — nunca reiniciam sozinhos. `relayInvert` só com o
  módulo de 30A no contato NC. O auto-restart de 30min sem WS é ignorado com o relé ligado.
- `/relay/config` grava direto na EEPROM **sem reiniciar** (não derruba máquina em uso);
  `/save` continua reiniciando. Páginas em PROGMEM (`send_P`), logs em `/logs` (JSON/texto)
  enquanto o AP estiver ativo; scan Wi-Fi assíncrono com o tipo de criptografia já em texto.

---

## 5. Protocolo WebSocket

Mensagens binárias (1 byte de comando); resposta sempre em texto. `0x04` só onde há
flash para imagem dupla; `0x05` só onde há sensor AVAIL.

| Byte | Industrial C3/S3 | Industrial ESP8266 | Convencional ESP32 | Convencional ESP8266 |
|------|------------------|--------------------|--------------------|----------------------|
| `0x01` | Pulso START IN (100ms) ¹ | Pulso relé (100ms) | Relé ON (se `relayMode`=0) | Relé ON (se `relayMode`=0) |
| `0x02` | ignorado | ignorado | Relé OFF (se `relayMode`=0) | Relé OFF (se `relayMode`=0) |
| `0x03` | Telemetria JSON ² | Telemetria JSON ² (sem `temp`) | Telemetria JSON ² | Telemetria JSON ² (sem `temp`) |
| `0x04` | OTA | — (sem flash) | OTA | — (sem flash) |
| `0x05` | Status AVAIL ³ | — | — | — |
| `0x06` | Restart remoto | Restart remoto | Restart remoto | Restart remoto |

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

## 8. Gravação (build + upload)

Não há `arduino-cli` no PATH; use o binário que vem na Arduino IDE:
`C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`

Compila e grava em um comando só (`-u -p <PORTA>`; sem isso, apenas compila).
**Prefira a linha de comando à IDE**: as opções vão no próprio FQBN, então não é
preciso ficar alternando os menus de Tools a cada modelo de peça — que é como se
grava a imagem errada na placa errada.

| Firmware | FQBN |
|---|---|
| Industrial ESP32-C3 | `esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs` |
| Industrial ESP32-S3 **N8R2** | `esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=default` |
| Industrial ESP32-S3 **N16R8** | `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default` |
| ESP-01S (industrial e convencional) | `esp8266:esp8266:generic:eesz=1M64,baud=115200` |

`arduino-cli board list` mostra a porta. O upload já regrava bootloader e tabela de
partição, então **trocar de esquema de partição não exige nada além de gravar de
novo** — e a NVS fica em `0x9000` em todos os esquemas usados aqui, então a
configuração da peça (nodeId, redes, pinos) sobrevive à troca.

**Três regras que custam uma visita presencial se ignoradas:**

1. **A partição precisa de dois slots de app**, ou não existe OTA `0x04`. `huge_app`,
   `minimal` e os `no_ota*` têm um só. Referência de ocupação (11/08/2026): C3 1.3.0 =
   1.287.151 bytes — **65%** em `min_spiffs` (1,9MB×2), mas **98%** no `default` da
   IDE (1,2MB×2). Daí o C3 ter saído do `default`.
2. **O Flash Size marcado tem de bater com o chip real** (S3 — ver [§4](#4-específico-de-cada-firmware)).
3. **"Erase All Flash Before Sketch Upload" desligado.** Ligar apaga a NVS junto.
   Pelo mesmo motivo, nunca use "Burn Bootloader" para tirar uma peça de loop de
   reset: não resolve (o upload normal já regrava o bootloader) e apaga a config.

**Alimentação em produção.** As peças são alimentadas por **fonte externa USB** — não
pelos +5V da máquina (H3-3). No **S3, que tem duas USB-C, use a `COM`**: as duas
alimentam (mesmo trilho de 5V), mas o conector `USB` liga D+/D− direto nos
**GPIO19/20 do próprio ESP32-S3**, e fonte USB costuma curto-circuitar D+ com D−
para sinalizar "sou carregador" — seriam dois pinos do MCU presos a um curto
externo, permanentemente. No `COM` as linhas de dados terminam no chip da ponte
USB-UART e o ESP32 só vê o 5V. Com `USB CDC On Boot: Disabled` a porta nativa não
serve para nada mesmo.

Não é risco iminente: o **C3 Super Mini tem uma única USB-C, e ela é a nativa** — a
frota de C3 roda nessa condição e funciona. É preferir o caminho isolado onde há
escolha. Efeito colateral do `COM`: a ponte fica energizada e é ela que comanda o
auto-reset (EN/GPIO0); sem host USB as linhas ficam em repouso e o boot é normal,
mas se aparecer **reset esporádico só nas peças S3**, olhe aí primeiro (clones de
CH340 podem dar glitch no DTR ao energizar).

---

## 9. Referências

- ALPM-39201 — manual elétrico (Alliance Laundry Systems)
- 204370ENR1 — manual de programação Quantum (jan/2019)
- Schematic 807300 — Control Option Wiring Diagram (Commercial FLW)

**Plataforma H13** (mais nova que o Quantum 440G da §6 — porta de pagamento
multiplexada por software, opção `SErPAy`): ver
[docs/DIAGNOSTICO-AVAIL-SPEED-QUEEN.md](docs/DIAGNOSTICO-AVAIL-SPEED-QUEEN.md).

- D518600ENR3 — manual de programação, secador DV6000WE (jan/2024)
- 807870ENR5 — manual de programação, lavadora frontal (mai/2025)
- 205160ENR3 — manual de programação, lavadora TV6000WN (2023)
- D514575ENR11 — manual de programação, secador plataforma antiga (nov/2025)
