# Plano Técnico — Nova Versão de Hardware/Firmware Foreasy

> Documento de referência (análise, não decisão final). Base: ESP32-S3 + backend Node no Heroku.
> Cobre: (1) OTA, (2) dois nodeIDs por device, (3) migração MQTT, (4) storage do `.bin`.

---

## Contexto atual (resumo do que existe)

- Firmware ESP32-S3 conecta via **WebSocket** em `frst-back-02b607761078.herokuapp.com:80`, path `/`.
- Handshake: envia `ID:<nodeId>` (relay ON) ou `NID:<nodeId>` (relay OFF) no connect.
- Protocolo binário: `0x01`=ON, `0x02`=OFF, `0x03`=pede telemetria JSON. Resposta `RelayStatus:ON/OFF`.
- Backend (`src/websocket.js`): `connections[]` em memória, **1 socket = 1 nodeId = 1 machineId**.
- Já existe muito código defensivo: debounce de reconexão, telemetria em lote, timer de desconexão de 10min, watchdogs.

---

## 1. OTA (Over-The-Air)

### 1.1 Restrições de hardware (decidem tudo)

| Chip | Flash | OTA viável? | Observação |
|------|-------|-------------|------------|
| ESP-01S (ESP8266) | 1 MB | ❌ Não | Sem espaço para imagem nova ao lado da antiga |
| ESP32 | 4 MB | ⚠️ Sim, apertado | Precisa partição OTA |
| **ESP32-S3** | 8–16 MB | ✅ Sim | **Alvo do OTA** — partição dupla folgada |

> **Conclusão:** OTA é recurso do **hardware novo (ESP32-S3)**. Não tente retrofitar na frota ESP-01S.

### 1.2 Particionamento (ESP32-S3)
- Arduino IDE → Tools → Partition Scheme → escolher um esquema **com OTA** (duas partições `app0`/`app1` + `otadata`).
- Ex.: "8M Flash (3MB APP x2 / 1.5MB SPIFFS)".

### 1.3 Storage do binário — **NÃO no Heroku**
O filesystem do dyno é efêmero (apagado a cada deploy/restart ~24h). Use object storage externo (ver seção 4). O backend guarda só **metadados** no MySQL.

### 1.4 Modelo de dados (MySQL) — sugestão
```sql
CREATE TABLE firmware_releases (
  id            INT AUTO_INCREMENT PRIMARY KEY,
  chip          VARCHAR(20)  NOT NULL,   -- 'esp32s3', 'esp32', 'esp8266-serial'...
  model         VARCHAR(40)  NOT NULL,   -- 'industrial-30a', 'convencional-ssr'...
  version       VARCHAR(20)  NOT NULL,   -- semver: '2.1.0'
  url           VARCHAR(500) NOT NULL,   -- URL do .bin no object storage
  sha256        CHAR(64)     NOT NULL,   -- integridade
  size_bytes    INT          NOT NULL,
  mandatory     TINYINT(1)   DEFAULT 0,  -- força update
  notes         TEXT,
  created_at    DATETIME     DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_chip_model_ver (chip, model, version)
);
```
> **Crítico:** o binário é namespaced por `chip` + `model`. Gravar `.bin` de chip errado **brica** o device. O firmware deve mandar `chip`+`model` ao consultar.

### 1.5 Fluxo OTA (recomendado: push + fallback no boot)

**Push (comando dedicado):** novo comando `0x04` no canal de controle (WS hoje, MQTT depois), carregando versão+URL+hash. Como o protocolo WS atual é byte cru, o payload do `0x04` viraria um pequeno JSON/TLV em frame de texto separado, ou bytes estruturados.

**Fallback boot:** no boot e a cada N horas, device faz `GET /firmware/check?chip=esp32s3&model=industrial-30a&current=2.0.1`. Backend responde se há versão maior.

**Execução no device:**
```cpp
// ESP32-S3 — esboço
#include <HTTPClient.h>
#include <Update.h>

bool doOTA(const String& url, const String& expectedSha256) {
  WiFiClientSecure client;       // HTTPS — S3 aguenta tranquilo
  client.setInsecure();          // ou pinar CA/cert da sua origem
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  int len = http.getSize();
  if (!Update.begin(len)) { http.end(); return false; }
  // Update.setMD5(expectedMd5);  // se usar MD5; p/ SHA256 valide manualmente

  size_t written = Update.writeStream(*http.getStreamPtr());
  if (written != (size_t)len) { http.end(); return false; }
  if (!Update.end(true)) { http.end(); return false; }

  http.end();
  ESP.restart();                 // boot na partição nova
  return true;
}
```

### 1.6 Segurança
- **Mínimo:** HTTPS + verificação de hash (SHA256) antes de `Update.end()`.
- **Ideal (ESP32-S3):** **Signed OTA** — assina o `.bin` com chave privada; bootloader/Update rejeita imagem sem assinatura válida. Protege contra binário malicioso mesmo se a URL vazar.
- Telemetria deve reportar `fw_version` (somar ao JSON do `0x03`) pra você auditar a frota.

### 1.7 Endpoints novos no backend
- `GET /firmware/check?chip=&model=&current=` → `{ update: true, version, url, sha256, size, mandatory }`
- `POST /firmware/releases` (admin) → registra nova release após upload no storage.
- (Opcional) `POST /firmware/report` → device confirma sucesso/falha do flash.

---

## 2. Dois nodeIDs por hardware (duas máquinas)

### 2.1 O que precisa mudar

**Firmware ESP32-S3:**
- `relayPin` singular → `relayPinA` / `relayPinB` (dois GPIOs).
- Estado singular (`relayLogicalOn`, `relayMode`, `relayType`) → arrays `[2]`.
- `nodeId` singular → `nodeIdA` / `nodeIdB` (dois campos no Preferences/NVS).
- Telemetria e comandos passam a indicar **qual nó**.

**Backend (`src/websocket.js`):** hoje assume 1:1. Precisa rework:
- `connections[]`: cada conexão passa a ter `nodes: [{ nodeId, machineId }]` em vez de `nodeId`/`machineId` singular.
- `getConnectionNodeId(ws)` → `getConnectionNodeIds(ws)`.
- `hasActiveConnection(nodeId)` → procurar em todas as conexões cujo array `nodes` contenha o nodeId.
- `forceReconnection(nodeId)` → idem.
- Sync de relay no connect (linhas ~177–192): rodar **por nó**.
- `persistDisconnect`: registrar desconexão **por nó/máquina**.

### 2.2 Caminho A — duas conexões separadas (mais simples no backend)
- Device abre **dois `WebSocketsClient`**, cada um registra seu nodeId.
- Backend quase não muda (já lida com N conexões).
- ❌ Custo no device: 2× heartbeat, 2× backoff, 2× RAM de socket. Funciona no S3, mas é o caminho pesado.

### 2.3 Caminho B — uma conexão, dois nós lógicos (recomendado)
- O protocolo binário cru **não tem endereçamento** → estender para 2 bytes: `[nodeIndex, cmd]`.

| Bytes | Significado |
|-------|-------------|
| `0x00 0x01` | nó A → Relay ON |
| `0x00 0x02` | nó A → Relay OFF |
| `0x01 0x01` | nó B → Relay ON |
| `0x01 0x03` | nó B → pede telemetria |

- Handshake: `ID:<nodeA>,<nodeB>` (ou duas linhas `ID:` na mesma conexão).
- Telemetria JSON ganha `"node": "<nodeId>"` pra o backend saber a origem.
- ✅ Uma conexão, um heartbeat, mais leve. Custo: projetar o framing.

### 2.4 Banco de dados
Sem mudança estrutural: continuam **duas linhas de máquina**, cada uma com seu `id_nodemcu`, apenas apontando para o mesmo device físico. (Se quiser rastrear o device físico, adicione um `device_uid` opcional nas duas linhas.)

### 2.5 Por que isso é trivial em MQTT
Endereçamento multi-nó é nativo: `foreasy/<nodeA>/cmd` e `foreasy/<nodeB>/cmd`. Sem inventar framing de bytes. **Este é um dos argumentos mais fortes a favor do MQTT.**

---

## 3. Migração WebSocket → MQTT

### 3.1 Por que considerar — o WS atual já "emula" MQTT na unha

| Hack no `websocket.js` hoje | Recurso nativo MQTT |
|---|---|
| Timer 10min + `persistDisconnect` p/ detectar queda | **LWT (Last Will & Testament)** — broker publica "offline" instantâneo |
| `reconnectionDebounce` + telemetria em lote (sobreviver à tempestade pós-restart do dyno) | Broker gerencia conexões; backend não segura socket |
| Sync de relay relendo DB e mandando `0x01/0x02` no connect | **Retained message** — device pega último estado desejado ao reconectar |
| `ws.send()` fire-and-forget (comando de relay pode sumir) | **QoS 1** — entrega garantida do comando |
| `connections[]` em memória (não escala entre dynos) | Backend stateless; múltiplos dynos publicam no mesmo broker |

> **Risco de negócio nº1 resolvido pelo QoS 1:** hoje o `0x01` é fire-and-forget. Se some, o cliente pagou e a máquina não liga. MQTT QoS 1/2 garante entrega.

### 3.2 Custos honestos
- Precisa de **broker** (não dá pra embutir no Node do Heroku como o WS).
- `websocket.js` vira **serviço cliente MQTT** (publisher de comandos + subscriber de telemetria).
- TLS no ESP8266 antigo é pesado (BearSSL/heap). No ESP32-S3 é tranquilo.
- Migração tem que ser **gradual** (conviver WS + MQTT).

### 3.3 Estratégia de convivência (transição)
```
Frota antiga (ESP8266/ESP32)  ── WebSocket ──┐
                                              ├──→ Backend (mesma lógica de negócio)
Hardware novo (ESP32-S3)      ── MQTT ────────┘
```
- Mantém `websocket.js` rodando pra frota legada.
- Novo `mqttService.js` assina telemetria e publica comandos.
- Ambos chamam os mesmos models (`machineModel`, `usageHistoryModel`, etc.).
- ESP32-S3 nasce só em MQTT.

### 3.4 Esquema de tópicos (proposta, broker-agnóstico)
```
foreasy/<nodeId>/cmd        # backend → device  (QoS 1): {"relay":"on"} | {"ota":{...}}
foreasy/<nodeId>/state      # device → backend  (retained): {"relay":"on","fw":"2.1.0"}
foreasy/<nodeId>/telemetry  # device → backend  (QoS 0): {"rssi":-60,"heap":...,"temp":...}
foreasy/<nodeId>/status     # LWT (retained):    "online" | "offline"
```
- **LWT** configurado no connect: tópico `status` = `offline`, retained. Ao conectar, device publica `online`.
- **Comando de relay** com QoS 1 + retained → reconexão reaplica estado sem o backend reler o DB.
- Device com 2 nós assina os 2 `cmd` e publica nos 2 `state/telemetry`.

### 3.5 Opções de broker

| Broker | Prós | Contras |
|--------|------|---------|
| **HiveMQ Cloud / EMQX Cloud** | MQTT puro, free tier ~100 conexões, migração direta | Você gerencia lógica de shadow/OTA |
| **AWS IoT Core** | **Device Shadow** (estado desejado/reportado = substitui sync de relay) + **IoT Jobs** (orquestra OTA) + regras | Lock-in AWS, curva maior, custo por msg |
| **Mosquitto self-hosted (VPS)** | Controle total, barato | Você opera/escala/segura o broker |

> Se for **AWS IoT Core**: o item 1 (OTA) ganha **IoT Jobs** pronto, e o sync de relay vira **Device Shadow** (desired vs reported) — elimina código. Em troca de lock-in.

### 3.6 Backend — esboço do serviço MQTT
```js
// mqttService.js (esboço)
const mqtt = require('mqtt');
const client = mqtt.connect(process.env.MQTT_URL, {
  username: process.env.MQTT_USER,
  password: process.env.MQTT_PASS,
});

client.on('connect', () => {
  client.subscribe('foreasy/+/telemetry', { qos: 0 });
  client.subscribe('foreasy/+/state',     { qos: 1 });
  client.subscribe('foreasy/+/status',    { qos: 1 });
});

client.on('message', (topic, payload) => {
  const [, nodeId, kind] = topic.split('/');
  if (kind === 'status')    handleStatus(nodeId, payload.toString());  // LWT online/offline
  if (kind === 'telemetry') bufferTelemetry(nodeId, JSON.parse(payload));
  if (kind === 'state')     reconcileState(nodeId, JSON.parse(payload));
});

// comando de relay (substitui ws.send(Buffer.from([0x01])))
function sendRelay(nodeId, on) {
  client.publish(`foreasy/${nodeId}/cmd`,
    JSON.stringify({ relay: on ? 'on' : 'off' }),
    { qos: 1, retain: true });
}
```

---

## 4. Storage do `.bin`

### 4.1 Por que não no Heroku
Filesystem efêmero: pasta apagada a cada deploy e restart (~24h). **Inviável.**

### 4.2 Opções

| Opção | Custo | Quando usar |
|-------|-------|-------------|
| **Cloudflare R2** | grátis até 10GB, **sem egress fee** | Recomendado p/ binários (downloads frequentes da frota) |
| Backblaze B2 | barato, egress generoso | Alternativa ao R2 |
| AWS S3 | barato + egress pago | Se for AWS IoT Core (integra) |
| GitHub Releases | grátis | Simples, versionável por tag; URL pública estável |

### 4.3 Fluxo de publicação
1. CI/build gera `firmware_esp32s3_industrial-30a_2.1.0.bin`.
2. Calcula SHA256.
3. Upload no object storage (R2/S3/Release).
4. `POST /firmware/releases` registra metadados no MySQL (seção 1.4).
5. (Opcional) dispara `0x04` push para os devices do modelo.

### 4.4 Nomenclatura sugerida
```
firmware/<chip>/<model>/<version>/firmware.bin
ex: firmware/esp32s3/industrial-30a/2.1.0/firmware.bin
```

---

## Convergência dos 3 itens

- **Se for MQTT:** item 2 (dois nodeIDs) fica trivial (tópicos), e com **AWS IoT Core** o item 1 (OTA) ganha **IoT Jobs** + sync de relay vira **Device Shadow**.
- **Se ficar em WS:** item 2 exige framing de bytes manual; item 1 (OTA) exige comando `0x04` próprio. Funciona, só dá mais trabalho artesanal.
- **Independente da escolha:** o `.bin` **nunca** mora no Heroku; storage externo + metadados no MySQL.

## Próximos passos sugeridos (quando decidir avançar)
- [ ] Definir broker (HiveMQ/EMQX vs AWS IoT vs seguir WS).
- [ ] Definir object storage (R2 recomendado).
- [ ] Criar tabela `firmware_releases` + endpoints `/firmware/*`.
- [ ] Firmware S3: campos `nodeIdA/nodeIdB`, relay duplo, `fw_version` na telemetria, rotina `doOTA()`.
- [ ] Particionar S3 com esquema OTA + (ideal) Signed OTA.
- [ ] Se MQTT: `mqttService.js` + esquema de tópicos + LWT, convivendo com `websocket.js` legado.
