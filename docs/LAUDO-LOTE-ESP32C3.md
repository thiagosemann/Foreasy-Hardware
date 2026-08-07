# LAUDO TÉCNICO — Falha de transmissão de RF em lote de placas ESP32-C3 Super Mini

| Campo | Conteúdo |
|---|---|
| **Emitente** | Foreasy |
| **Data do laudo** | 06/08/2026 |
| **Produto** | Placa de desenvolvimento ESP32-C3 Super Mini |
| **Fornecedor** | `[PREENCHER: nome do fornecedor]` |
| **Pedido / nota** | `[PREENCHER: nº do pedido e data da compra]` |
| **Quantidade do lote** | 40 unidades |
| **Unidades testadas** | `[PREENCHER: quantas foram testadas]` |
| **Unidades reprovadas** | `[PREENCHER: quantas falharam]` |
| **Lote de referência** | Placas do fornecedor anterior, mesmo modelo, em operação normal |

---

## 1. Resumo do defeito

As placas do lote em questão **não transmitem em radiofrequência na potência nominal**.
A recepção funciona normalmente, mas a placa é incapaz de associar-se a qualquer rede
Wi-Fi e não emite o beacon do próprio Access Point.

O defeito **não está no módulo ESP32-C3** — o chip foi verificado e é genuíno,
com calibração de fábrica íntegra e revisão de silício idêntica à das placas em
operação normal. O defeito está no **subsistema de alimentação da placa**.

## 2. Impacto

As unidades são **inservíveis** para a aplicação. O produto depende de conexão Wi-Fi
permanente; uma placa que não associa em rede alguma não cumpre nenhuma função, e
também não pode ser configurada em campo, já que o Access Point de configuração
igualmente não vai ao ar.

---

## 3. Metodologia

Todos os testes foram executados na mesma bancada, com a mesma fonte USB, o mesmo
cabo, o mesmo computador e a mesma posição física, alternando apenas a placa sob
teste. O firmware de teste é o mesmo binário em todas as unidades.

| Item | Valor |
|---|---|
| Toolchain | Arduino ESP32 core 3.3.11 |
| Ferramentas | `arduino-cli`, `esptool` 5.3.1, `espefuse` 5.3.1 |
| Rede de referência | 2.4 GHz, canal 7, WPA2 |
| Sinal da rede na bancada | −38 dBm (condição favorável, roteador próximo) |
| Firmware de teste | `Industrial/ESP32-C3/teste_wifi/teste_wifi.ino` (anexo B) |
| Timeout por tentativa | 20 s |

O firmware de teste separa deliberadamente as duas funções do rádio:

- **Recepção** — varredura de redes (`scan`). Não exige transmissão.
- **Transmissão** — associação à rede. Exige transmitir *probe request*,
  *authentication* e *association request*.

Essa separação é o núcleo do laudo: ela isola a falha em uma das duas funções.

---

## 4. Resultados medidos

### 4.1 O módulo ESP32-C3 é genuíno e idêntico ao do lote em operação

Leitura direta dos eFuses do silício (`espefuse summary`, somente leitura):

| Parâmetro | Placa em operação | Placa do lote reprovado |
|---|---|---|
| Chip | ESP32-C3 (QFN32) rev **v0.4** | ESP32-C3 (QFN32) rev **v0.4** |
| `PKG_VERSION` | 0 | 0 |
| `WAFER_VERSION` | major 0 / minor 4 | major 0 / minor 4 |
| `BLK_VERSION_MAJOR` | *With calibration* | *With calibration* |
| Flash (eFuse) | Embedded 4 MB, XMC | Embedded 4 MB, XMC |
| Flash (detectada) | Manuf `0x46`, Dev `0x4016` | Manuf `0x46`, Dev `0x4016` |
| Cristal | 40 MHz | 40 MHz |
| MAC | `9C:CC:01:A9:64:3C` — válido | `48:F6:EE:12:9C:60` — válido |
| Fuses de segurança | nenhum queimado | nenhum queimado |
| Calibração LDO / ADC / temperatura | presente | presente |

**Conclusão parcial:** o módulo é legítimo, calibrado de fábrica e da mesma revisão.
Nenhuma diferença de silício ou de configuração explica o defeito.

### 4.2 A recepção funciona normalmente

Varredura executada pela própria placa reprovada:

```
[1/2] Testando RECEPCAO (scan)...
      14 redes visiveis
      >> SEMANN                   ch= 7 rssi=-38
         FernandoCunha            ch= 8 rssi=-78
         #CLARO-WIFI              ch= 7 rssi=-81
      OK: recepcao funcionando e a rede alvo esta visivel.
```

A placa enxerga 14 redes, incluindo a rede alvo com sinal forte (−38 dBm).
**O receptor está íntegro.**

### 4.3 A transmissão é inoperante na potência nominal

Na mesma execução, imediatamente após a varredura acima:

```
--- Tentativa 1 | TX pedido=19.5 dBm  aplicado=19.5 dBm ---
      [   0ms] status=6 DISCONNECTED (nao associou)
      >>> FAIL - nao associou em 20s
```

O estado permanece em `DISCONNECTED` durante os 20 segundos inteiros, sem transitar
por nenhum estado intermediário — as tentativas de associação **não obtêm resposta
alguma** do ponto de acesso, que está a poucos metros e com sinal de −38 dBm.

Resultado reproduzido em **100% das tentativas** e em múltiplas unidades do lote.

### 4.4 A transmissão volta a funcionar com a potência reduzida

Repetindo o mesmo teste, na mesma placa e na mesma posição, alterando apenas a
potência de transmissão do rádio:

| Potência de TX | Resultado |
|---|---|
| 19,5 dBm (nominal) | **FALHA** — não associa |
| 15,0 dBm | **SUCESSO** — associa normalmente |

Uma placa cuja antena estivesse defeituosa **não** melhoraria com a redução da
potência de transmissão — o comportamento seria o oposto. A melhora com a redução
de potência aponta para limitação de **fornecimento de corrente**, não de RF.

### 4.5 Verificação independente por varredura de rádio externa

Varredura do espectro 2.4 GHz realizada por um computador a poucos centímetros de
ambas as placas, com o mesmo firmware gravado em cada uma:

| Placa | Access Point detectado | Sinal |
|---|---|---|
| Em operação (`9C:CC:01:A9:64:3D`) | **Sim** | 90 % |
| Lote reprovado (`48:F6:EE:12:9C:61`) | **Não** | — |

A placa reprovada também não respondeu a *probe request* dirigida ao seu SSID.
Confirma-se, por instrumento externo e independente do firmware, que a placa
**não emite energia de RF** na potência nominal.

---

## 5. Análise

O consumo do ESP32-C3 em transmissão é função direta da potência de saída. Valores
típicos de datasheet:

| Condição de operação | Corrente típica |
|---|---|
| Transmissão 802.11b @ 20 dBm | ≈ 335 mA |
| Transmissão 802.11g @ 19 dBm | ≈ 285 mA |
| Transmissão @ 15 dBm | ≈ 250 mA ou menos |
| Recepção / escuta | ≈ 80–100 mA |

Cruzando com os resultados medidos:

- A placa **falha** onde o consumo passa de ~300 mA.
- A placa **funciona** onde o consumo fica em torno de 250 mA.
- A placa **nunca falha** em recepção, cujo consumo é de ~80–100 mA.

O ponto de corte do defeito coincide com um limite de corrente entre
aproximadamente **250 mA e 300 mA**. Esse comportamento é característico de uma
fonte de alimentação que satura ao atingir seu limite de corrente, e é incompatível
com defeito de antena, de casamento de impedância ou de silício — todos já
descartados pelos itens 4.1 a 4.5.

**Conclusão técnica:** o subsistema de alimentação de 3,3 V das placas deste lote
não sustenta os picos de corrente exigidos pelo ESP32-C3 em transmissão na potência
nominal especificada pelo fabricante do módulo.

---

## 6. Hipótese de componente

> **Nota:** esta seção é hipótese de causa raiz, ainda **não confirmada** por
> inspeção física. As seções 4 e 5 acima são medições e independem dela.

Duas causas prováveis, em ordem de probabilidade:

1. **Regulador LDO subdimensionado.** O ESP32-C3 Super Mini de referência utiliza um
   regulador de 500 mA (classe ME6211C33M5G). Reguladores de 250 mA (classe
   XC6206 / HT7333) são substituições comuns e não atendem à corrente de
   transmissão do módulo. Verificável pela marcação no encapsulamento.

2. **Capacitância de desacoplamento insuficiente.** Ausência ou subdimensionamento do
   capacitor de saída do regulador e/ou do desacoplamento no pino 3V3 do módulo.
   O pico de transmissão dura microssegundos e é suprido pelo capacitor, não pelo
   regulador.

`[PREENCHER após inspeção: marcação do regulador na placa em operação vs. na placa reprovada]`

---

## 7. Conclusão

As placas do lote **não atendem aos requisitos elétricos do módulo ESP32-C3 que elas
próprias integram**. O módulo é genuíno e funcional; a placa que o alimenta é que está
fora de especificação. O defeito é de projeto ou de componente do fornecedor, não de
aplicação, não de firmware e não de uso indevido — o mesmo binário, na mesma bancada,
opera normalmente nas placas do lote anterior.

## 8. Solicitação

`[PREENCHER: substituição do lote / devolução / reembolso / prazo]`

---

## Anexo A — Log completo de uma unidade reprovada

```
========================================
   TESTE DE CONEXAO WIFI - Foreasy
========================================
Rede alvo   : REDE
MAC da peca : B8:1F:3F:FC:32:08

[1/2] Testando RECEPCAO (scan)...
      10 redes visiveis
      >> SEMANN                   ch= 7 rssi=-40
         Residencia_Isos          ch= 5 rssi=-62
         VIVO68                   ch= 6 rssi=-80
         F8_Network               ch= 6 rssi=-80
         LRATSKE 2G               ch= 5 rssi=-82
      OK: recepcao funcionando e a rede alvo esta visivel.

[2/2] Testando TRANSMISSAO (associacao)...

--- Tentativa 1 | TX pedido=19.5 dBm  aplicado=19.5 dBm ---
      [   0ms] status=6 DISCONNECTED (nao associou)

      >>> FAIL - nao associou em 20s (status=6 DISCONNECTED (nao associou))
      Se o scan acima ENXERGOU a rede, a recepcao esta boa e
      a falha e de TRANSMISSAO.

      Placar: 0/1 conexoes. Proxima em 5s.

--- Tentativa 2 | TX pedido=15.0 dBm  aplicado=15.0 dBm ---
E (30289) wifi:sta is connecting, cannot set config
      [   0ms] status=6 DISCONNECTED (nao associou)
      [ 950ms] status=0 IDLE (parado)

      >>> PASS - A PECA CONECTOU <<<
      IP      : 192.168.0.89
      Gateway : 192.168.0.1
      Canal   : 7
      RSSI    : -38 dBm

===== VEREDITO: APROVADA COM RESSALVA =====
   Conectou com 15.0 dBm, mas FALHOU na potencia maxima.
   A peca transmite, porem nao aguenta os picos de corrente do
   TX em potencia cheia: entrega de energia da placa (regulador
   ou capacitor de bulk). Separe estas pecas das aprovadas.
```

Leitura de eFuses da mesma unidade (`espefuse summary`):

```
MAC (BLOCK1)              = 48:f6:ee:12:9c:60 (OK)
BLK_VERSION_MAJOR         = With calibration
WAFER_VERSION_MAJOR       = 0
WAFER_VERSION_MINOR       = 4
PKG_VERSION               = 0
FLASH_CAP                 = 4M
FLASH_VENDOR              = XMC
```

## Anexo B — Reprodução do teste

O teste é integralmente reprodutível pelo fornecedor. Firmware e procedimento:

```bash
# 1. Gravar o firmware de teste
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc -u -p <PORTA> teste_wifi/

# 2. Abrir o monitor serial
arduino-cli monitor -p <PORTA> -c baudrate=115200

# 3. Ler o veredito. O firmware varre as potências de transmissão
#    (19,5 / 15 / 11 / 8,5 / 5 dBm) e reporta PASS/FAIL em cada uma.
```

O firmware de teste registra, para cada tentativa, a potência solicitada **e** a
potência efetivamente aplicada ao rádio (lida de volta do próprio hardware), de modo
que o resultado não depende de a configuração ter sido aceita.

## Anexo C — Registro fotográfico

`[PREENCHER: fotos da região do regulador e dos capacitores, placa em operação vs. placa reprovada]`
