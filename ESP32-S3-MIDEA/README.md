# Foreasy MIDEA IoT — ESP32-S3

Firmware que vincula **um ESP32-S3 a uma máquina MIDEA Smart** (lavadora/secadora
residencial, convencional). O ESP fala com a máquina na rede local via protocolo
MIDEA V3 criptografado e expõe o status do ciclo (energia, fase, tempo restante).

> Implementa em C++ (mbedtls) o mesmo protocolo da lib Python `midea-local`.
> Detalhes do protocolo: [../Midea-WiFi/VETORES-TESTE.md](../Midea-WiFi/VETORES-TESTE.md)
> e [../Midea-WiFi/PROTOCOLO.md](../Midea-WiFi/PROTOCOLO.md).

## Como usar

### 1. Obter os dados da máquina (1x por máquina)
Nesta pasta, com o PC na mesma rede da máquina, rode **`rodar.bat`** (duplo clique).
Ele acha o Python, instala as dependências (`requirements.txt`), descobre a máquina,
pede o login do SmartHome uma vez e salva tudo em `devices.json`:
- **Device ID** (ex: `150633095703018`)
- **IP** (ex: `192.168.0.20`)
- **Token** (128 hex) e **Key** (64 hex) — obtidos do MIDEA Cloud uma vez

Arquivos do script (nesta pasta): `rodar.bat`, `get_device_info.py`, `requirements.txt`.

### 2. Flash + configuração
1. Abra `ESP32-S3-MIDEA/midea_iot/midea_iot.ino` no Arduino IDE → flash no ESP32-S3.
2. Conecte no AP **`<nodeId>-AP`** (senha `12345678`) → acesse `192.168.4.1`.
3. Wizard:
   - **Passo 1 — Rede:** WiFi da lavanderia (a mesma da máquina), rede 2 (failover) e Node ID.
   - **Passo 2 — Máquina MIDEA:** cole Device ID, Token, Key e Tipo (`DB`).
     O **IP é opcional** — deixe vazio que o ESP descobre sozinho (ver abaixo).
4. Salvar → o ESP reinicia, conecta no WiFi e na máquina.

> **IP não é obrigatório.** O ESP acha a máquina pelo **Device ID** via broadcast UDP
> (porta 6445): cada máquina ligada responde e o IP é o endereço de origem. Se o IP
> mudar (DHCP) ou a conexão cair, o ESP re-descobre automaticamente. O `mdIp` salvo é
> só um cache/atalho. **O obrigatório é o Device ID** (estável, nunca muda).

### 3. Acompanhar
Acesse **`/info`** (atualiza a cada 3s): energia, em uso, **tempo restante**, fase,
programa, e o status da conexão TCP com a máquina. JSON cru em **`/status`**.

Teste de controle: `/power?on=1` liga, `/power?on=0` desliga (enquanto na rede).

## Rede AP

| Item | Valor |
|------|-------|
| SSID | `<nodeId>-AP` (ex: `MIDEA-001-AP`) |
| Senha | `12345678` |
| IP | `192.168.4.1` |
| Tempo ativo | 10 min após boot, depois desliga |

## Armazenamento (NVS, namespace `midea`)

| Chave | Conteúdo |
|-------|----------|
| `nodeid` | identificação Foreasy do ESP |
| `ssid` / `pass` | WiFi rede 1 |
| `ssid2` / `pass2` | WiFi rede 2 (failover) |
| `mdId` | device_id MIDEA (string decimal → uint64) — **identificador obrigatório** |
| `mdIp` | IP da máquina (opcional — cache; descoberto pelo device_id se vazio) |
| `mdToken` | token V3 (128 hex) |
| `mdKey` | key V3 (64 hex) |
| `mdType` | tipo do device (0xDB) |
| `bootCount` | contador de boots |

## Limitações conhecidas

- **WiFi da máquina morre quando ela desliga** (painel apaga pós-ciclo). Logo o ESP
  só lê status enquanto a máquina está ligada/rodando. "Desconectado" = desligada.
- **Não dá para ligar a máquina via WiFi quando ela está totalmente off** (não está na
  rede). Para liberar energia, ainda é necessário o relay (modelo atual do Foreasy).
- **Protocolo V3** assume tipo `0xDB`. Outros tipos (ar-condicionado etc.) usam offsets
  de status diferentes — ajustar `parsePacket5A5A` conforme o device.
- Integração com o backend (WebSocket Heroku) ainda não incluída — este firmware foca em
  **conectar e acompanhar**. O reporte ao backend é o próximo passo.

## Próximos passos

- [ ] Validar o handshake V3 e a leitura no hardware (comparar `/status` com o `explorar.bat`).
- [x] Descoberta por broadcast (acha o IP pelo device_id; re-descobre se mudar — implementado).
- [ ] Reporte do status ao backend via WebSocket (estilo `esp32c3.ino`: `MideaStatus:<json>`).
- [ ] Mapear números de `program`/`mode`/`progress` do modelo (via `explorar.bat`).
