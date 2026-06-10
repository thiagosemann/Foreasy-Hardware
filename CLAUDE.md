# Foreasy Hardware — Contexto do Projeto

## Objetivo
Produto próprio (hardware + firmware) para automação de lavadoras, substituindo soluções concorrentes (VMlav, VENDPAGO):
- **Modo Industrial**: controle direto de energia — relay ativo libera, relay inativo corta
- **Modo Convencional**: integração com lavadoras **Speed Queen** via pulso elétrico no conector H5
- Ler status da máquina (livre/ocupada) via AVAIL OUT (Speed Queen)
- Comunicação com backend via WebSocket

---

## Modelos de Sistema

### Industrial
Controle puro de energia: o backend liga/desliga o relay, que libera ou corta a alimentação da máquina. Não há comunicação com a placa da máquina — funciona com qualquer lavadora.

### Convencional
Integração com a Speed Queen via conector H3/H5: envio de pulso de crédito para liberar ciclo. Leitura de AVAIL OUT para confirmação (em implementação).

---

## Modelos de Hardware

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

## Hardware da Máquina — Speed Queen Quantum 440G

### Conector de pagamento — AP1 Front End Control
> **Atenção sobre nomenclatura:** O schematic 807300 chama esse conector de **H3** (7 pinos). O manual elétrico ALPM-39201 chama de **H5**. Verifique fisicamente qual plug você está usando.

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

> **Componentes confirmados pelo schematic:** START PULSE usa optoacoplador **H11L1**; AVAIL OUT usa optoacoplador **4N25** com coletor (H3-4) e emissor (H3-5) expostos separadamente.

### Especificações elétricas (schematic 807300 + ALPM-39201)
- **START IN**: 3–30mA, **mínimo 20ms** (schematic 807300); ALPM diz 45ms — usar 100ms no firmware é seguro
- **AVAIL OUT**: Vmax 28VDC, Imax 5mA — pull-up externo 10kΩ em H3-4 (COL); H3-5 (EMIT) → GND (H3-2)
- Circuito de pulso: `+5V (H3-3) → 150Ω externo → H3-7 (START IN)` | `H3-2 → GND do shield`
  - Cálculo: ~5V / (100Ω interno + 150Ω externo) ≈ 14mA — dentro de 3–30mA ✓

### Como medir AVAIL OUT com osciloscópio
```
H3-3 (+5V) ── 10kΩ ──┬── CH1 osciloscópio
                      |
                    H3-4 (AVAIL COL — 4N25)
                    H3-5 (AVAIL EMIT) ── H3-2 (COM/GND) ── GND osciloscópio
```

### Polaridade do AVAIL OUT — IMPORTANTE
O schematic 807300 indica **"ON = AVAILABLE"** — o transistor conduz quando a máquina está **livre**:

| Nível (com pull-up 10kΩ → 3.3V) | Estado da máquina |
|----------------------------------|-------------------|
| **LOW (0V)** — transistor ON     | **Livre / disponível** |
| **HIGH (3.3V)** — transistor OFF | **Ocupada / em ciclo** |

**Lógica de confirmação após pulso:**
1. Antes: AVAIL OUT = LOW (máquina livre) → ok para enviar pulso
2. Enviar pulso
3. Máquina aceita → inicia ciclo → AVAIL OUT vai para HIGH
4. HIGH após ~500ms = sucesso | LOW após ~500ms = máquina não aceitou → retry

---

## Hardware do ESP — Shield Serial (Modelos 1 e 5)

**ESP-01S + Shield Relay AZ-Delivery**
- MCU relay: STC15F104W (também chamado 7250M / SOC 121011-Q)
- Relay: SRD-5VDC-SL-C (10A)
- Regulador: AMS1117-3.3 — GPIOs em 3.3V TTL
- **Relay controlado via Serial 9600 baud** (não GPIO direto)
- Modelo 5 (Convencional): relay aciona SSR externo de alta corrente

### Comandos Serial do Relay (STC15F104W)
```cpp
uint8_t ON_CMD[]  = {0xA0, 0x01, 0x01, 0xA2};  // Relay ON
uint8_t OFF_CMD[] = {0xA0, 0x01, 0x00, 0xA1};  // Relay OFF
```

### Limitações do ESP-01S
- UART única (GPIO1/TX, GPIO3/RX) — ocupada pelo relay
- GPIO0 e GPIO2 = strapping pins (boot) — usar com cuidado
- Brownout no acionamento do relay → desabilitado no firmware
- **Pulso mínimo real: ~100ms** (STC15F104W não processa OFF abaixo de 80ms)

---

## Firmware

### Arquivos
| Arquivo | Hardware | Descrição |
|---------|----------|-----------|
| `Foreasy-Hardware-Serial/Foreasy-Hardware-Serial.ino` | Modelos 1 e 5 | ESP-01S + shield serial (STC15F104W) — **firmware principal ESP8266** |
| `Foreasy-Hardware/Foreasy-Hardware.ino` | Modelo 2 | ESP-01S + shield GPIO V1 — relay via GPIO0 direto |
| `Foreasy-Hardware-ESP32/esp32.ino` | Modelos 3 e 4 | ESP32 + shield relé 30A (com ou sem SSR) |

---

### Modelos 1 e 5 — `Foreasy-Hardware-Serial.ino`

**Relay controlado via Serial 9600 baud → STC15F104W (não GPIO)**

- Serial 100% dedicada ao relay — sem debug por Serial; logs só em `/info` enquanto AP ativo
- `machineMode`: `INDUSTRIAL` (pulso 100ms por 0x01) ou `CONVENTIONAL` (0x01=ON / 0x02=OFF)
- `relayInvert`: inverte lógica ON/OFF — útil para relay NC
- **EEPROM v4** (`magic=0xF0EA5E01`): `ssid`, `pass`, `ssid2`, `pass2` (failover), `nodeId`, `machineMode`, `relayInvert`, `bootCount`, `lastResetReason`, `wsRestartEnabled`
- AP: 10 min ativo após boot → lean mode (AP desliga, logs apagados do buffer)
- SSID do AP: `<nodeId>-AP` | Senha: `12345678`
- Failover automático entre rede 1 e rede 2 sem restart
- **Watchdogs sem restart**: WS down >5 min → failover WiFi+WS; sem WiFi+WS >8 min → failover
- `wsRestartEnabled`: restart automático se sem WS por 1 hora (configurável)
- WS backoff exponencial: 10s base → 120s máx
- Scan WiFi assíncrono (não bloqueia o loop)
- Erases setor EEPROM antes de salvar (`ERASE_SECTOR_BEFORE_SAVE=true`) + 3 tentativas de commit com verificação
- Rotas HTTP: `/config`, `/config-data`, `/info`, `/scan`, `/save`, `/diagflash`

### Modelo 2 — `Foreasy-Hardware.ino`

**Relay controlado via GPIO0 direto — Serial 115200 disponível para debug**

- Mesma estrutura de EEPROM v4 e mesmo protocolo WS que o firmware Serial
- Mesmo comportamento `machineMode` (INDUSTRIAL/CONVENTIONAL) e `relayInvert`
- Flags de debug habilitados: `DEBUG_FLASH_INFO=true`, `DEBUG_EEPROM_DUMP=true`, `DEBUG_EEPROM_SECTORINFO=true`
- Scan WiFi **síncrono** (bloqueia o loop durante o scan — diferença do Serial)
- GPIO0 = relay (strapping pin — cuidado no boot); GPIO2 = LED ativo LOW
- `applyRelayPhysical()` usa `digitalWrite` em vez de `Serial.write`
- Logs críticos (`[CRIT]`) enviados via Serial mesmo após AP desligar

### Modelos 3 e 4 — `esp32.ino`

**Relay controlado via GPIO2; LED em GPIO4**

- Usa **Preferences (NVS)** — `prefs.begin("wifi", false)`; credenciais cacheadas em Strings no boot
- **Sem machineMode** — firmware puramente relay ON/OFF (sem pulso Speed Queen)
- `relayMode`: `0`=Normal (segue WS), `1`=Sempre ON, `2`=Sempre OFF
- `relayType`: `0`=NA (ON=HIGH), `1`=NF (ON=LOW) — salvo individualmente via `/relay/config`
- Em modo Sempre ON/OFF: comandos WS e botões locais são ignorados
- **Dual WiFi com failover** (ssid/ssid2) — alterna sem restart, `wifiSlot` rastreado
- **WiFi não-bloqueante**: `wifiTick()` com timeout de 40s, retry a cada 5s — **sem apagar credenciais por falha de conexão**
- **WS backoff exponencial**: 10s base → 120s máx
- **Watchdogs**: WS down >5min → failover; sem WiFi+WS >8min → failover
- `wsRestartEnabled`: restart após 1h sem WS (configurável)
- **WS zombie**: detecta ausência de ping/pong por 5min e reconecta
- App ping a cada 30s; `webSocket.enableHeartbeat(15000, 3000, 2)`
- Ao conectar WS: envia `"ID:<nodeId>"` se relay ON, `"NID:<nodeId>"` se OFF
- Comando `0x03` responde JSON: `rssi, heap, uptime, boots, wifiSlot, temp`
- Lê temperatura interna (`temprature_sens_read()`)
- `bootCount` incrementado em RAM; salvo em Preferences apenas no `/save`
- Scan WiFi **assíncrono** (`WiFi.scanNetworks(true)`)
- `/config-data` endpoint JSON separado da página HTML
- Rotas HTTP: `/config`, `/config-data`, `/info`, `/scan`, `/save`, `/status`, `/relay`, `/relay/on`, `/relay/off`, `/relay/config`, `/nodeid`, `/savenodeid`, `/wifistatus`, `/wsstatus`, `/resetwifi`, `/restart`

---

### Protocolo WebSocket (binário) — ESP8266 (Modelos 1, 2, 5)

| Byte | Modo Industrial | Modo Convencional |
|------|----------------|-------------------|
| 0x01 | Pulso relay (100ms) | Relay ON |
| 0x02 | Ignorado | Relay OFF |
| 0x03 | Responde JSON: `rssi, ch, heap, block, cpu, uptime, boots, wifiSlot` | idem |

### Protocolo WebSocket (binário) — ESP32 (Modelos 3, 4)

| Byte | Ação |
|------|------|
| 0x01 | Relay ON (se relayMode=Normal) |
| 0x02 | Relay OFF (se relayMode=Normal) |

Resposta sempre: `"RelayStatus:ON"` ou `"RelayStatus:OFF"`

---

## Configuração da Máquina Speed Queen (MDC)

### Parâmetros críticos
| Parâmetro | Função | Valor recomendado |
|-----------|--------|-------------------|
| PLSE | Valor de cada pulso (centavos) | = valor total do ciclo |
| PLSNod | Modo de pulso | **1** = pulso único libera ciclo |

- **PLSNod=1**: um pulso libera ciclo completo, extras ignorados ✓
- **PLSNod=128**: múltiplos pulsos somam crédito (padrão fábrica)

### Como entrar no MDC (Manual Mode)
1. Pressione NORMAL/HOT + START simultaneamente por 3 segundos
2. Display mostra `Prog` — navegue com PERM PRESS (↑) / DELICATES (↓)
3. START = confirmar | NORMAL/HOT = sair

---

## Próximos Passos

- [ ] Configurar PLSNod = 1 na máquina
- [ ] Confirmar AVAIL OUT com pull-up 10kΩ → 3.3V
- [ ] Implementar leitura de status via GPIO3 (após Serial.end()) ou placa com mais GPIOs
- [ ] Validar lógica: pulso → confirma AVAIL OUT → sucesso/retry
- [ ] Definir shield definitivo (ESP8266MOD + GPIOs expostos)

### Arquitetura alvo
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

## Referências
- Manual elétrico: ALPM-39201 (Alliance Laundry Systems)
- Manual programação Quantum: 204370ENR1 (janeiro 2019)
- Schematic AP1/AP2: 807300 (Control Option Wiring Diagram Commercial FLW)
