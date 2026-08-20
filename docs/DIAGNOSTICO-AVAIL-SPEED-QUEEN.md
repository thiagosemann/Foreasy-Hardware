# Speed Queen plataforma H13 — AVAIL OUT, programação e diagnóstico

> Levantamento feito a partir dos manuais de programação da Alliance Laundry para
> investigar 3 máquinas em campo (2 secadores + 1 lavadora) em que o **AVAIL OUT não
> indica corretamente se a máquina está em uso**.
>
> **Atenção:** o [CLAUDE.md §6](../CLAUDE.md) documenta o **Quantum 440G, conector H3**.
> Estas máquinas são de uma **plataforma mais nova, conector H13**, com opções de
> programação que não existem no 440G. Este documento cobre a plataforma H13.

| Campo | Conteúdo |
|---|---|
| **Data** | 18/08/2026 |
| **Máquinas** | 2 secadores (DV6000WE) + 1 lavadora |
| **Sintoma** | Pino AVAIL não distingue livre/ocupada |
| **Start pulse** | ✅ Funciona nas 3 |
| **AtyPE** | Não aparece no menu em 2 secadores; = `0` e correto na lavadora |
| **Status** | Diagnóstico em campo pendente |

## Índice
- [1. Manuais e plataformas](#1-manuais-e-plataformas)
- [2. O bloco de vend no menu Prog](#2-o-bloco-de-vend-no-menu-prog)
- [3. SErPAy — configuração da porta H13](#3-serpay--configuração-da-porta-h13)
- [4. AtyPE — Programmable Output Type](#4-atype--programmable-output-type)
- [5. Entrada no Manual Mode](#5-entrada-no-manual-mode)
- [6. Testes de diagnóstico](#6-testes-de-diagnóstico)
- [7. Leitura dos bitmaps d 29 e d 32](#7-leitura-dos-bitmaps-d-29-e-d-32)
- [8. Diagnóstico do caso em campo](#8-diagnóstico-do-caso-em-campo)
- [9. Pendências](#9-pendências)
- [10. Referências](#10-referências)

---

## 1. Manuais e plataformas

| Part No. | Data | Máquina | Observação |
|---|---|---|---|
| `D518600ENR3` | jan/2024 | Secador (DV6000WE) | Tem `SErPAy`/H13. Painel HIGH/MED/LOW TEMP + DELICATE |
| `807870ENR5` | mai/2025 | Lavadora frontal | Tem `SErPAy`/H13 + teste `d 23` |
| `205160ENR3` | 2023 | Lavadora TV6000WN | Mesmas opções e mesmos códigos da 807870 |
| `D514575ENR11` | nov/2025 | Secador (outra família) | **Não tem** `SErPAy`. Plataforma antiga, porta fixa |

> A lista de modelos do `D518600ENR3` não traz literalmente "DV6000WE" — a nomenclatura
> comercial de exportação difere dos códigos de fábrica (HDEN…, SDEN…, STGN…). O part
> number e o desenho do painel batem, então é o manual correto.

**Diferença estrutural entre as plataformas:** na antiga (`D514575`) a porta de pagamento
é fixa e o start pulse sempre existe. Na nova (H13) uma única porta física é multiplexada
por software entre atualização de firmware, leitor de cartão serial e start pulse — e é
isso que o `SErPAy` escolhe.

---

## 2. O bloco de vend no menu Prog

A ordem é **idêntica** no secador e na lavadora:

```
dEn 2  →  SErPAy  →  CArdEn  →  PULSE  →  AtyPE  →  dFtCyC
                                  ├─ PLSdEn   (Start Pulse Value)
                                  ├─ PLS En   (Start Pulse Enable)
                                  └─ PLSMod   (secador) / PLSNod (lavadora)
```

Valores que o Foreasy precisa:

| Opção | Deve estar | Padrão de fábrica | Por quê |
|---|---|---|---|
| `SErPAy` | **`S PLSE`** | `CArd` | Único valor em que o H13 aceita Start Pulse |
| `CArdEn` | `oFF` | `oFF` | Só liga junto com `SErPAy` = `CArd` |
| `PLS En` | `on` | `on` | Habilita o recurso de Start Pulse |
| `PLSMod` / `PLSNod` | `SingL` / `SingLE` | idem | Um pulso libera o ciclo inteiro |
| `AtyPE` | `0` | `0` | Sinal ativo em Ready Mode → **LOW = livre** |

> **Sobre o padrão de fábrica do `SErPAy`:** os manuais se contradizem. A tabela de opções
> diz `CArd`; o corpo do texto do secador diz `SEriaL`; a NOTA logo abaixo diz `Card`.
> Nos três casos o Start Pulse fica **inibido** — ou seja, **máquina nova de fábrica não
> aceita o pulso do Foreasy até o `SErPAy` ser mudado para `S PLSE`.**

Opção relacionada, fora do bloco: **`CErr 3` — Vend Header Present Error** (padrão `on`).
Se habilitada e o vend header estiver desconectado, o controle **ignora as entradas de crédito**.

---

## 3. SErPAy — configuração da porta H13

| Valor | O que o H13 faz | O que fica inibido |
|---|---|---|
| `SEriaL` | Aceita atualização de firmware por link serial | Serial Card e **Start Pulse** |
| `CArd` | Leitor de cartão serial (3rd-party ou Serial FOB). Requer `CArdEn` = `on` | Firmware Update e **Start Pulse** |
| **`S PLSE`** | **Aceita Start Pulses de dispositivo a pulso de terceiros** ← Foreasy | Toda comunicação serial |
| `oFF` | Porta desligada | Tudo acima |

> Quando um valor é selecionado, **os outros são inibidos no controle**. É por isso que
> `PULSE` e `AtyPE` podem sumir da lista do `Prog`: sem a porta configurada para pulso,
> o bloco não é montado.
>
> Depois de mudar o `SErPAy`, **saia do `Prog` e entre de novo** — a lista é remontada na entrada.

---

## 4. AtyPE — Programmable Output Type

Define **quando** o controle ativa a saída programável (o nosso AVAIL OUT).

| Valor | Nome | Modos em que o sinal fica ATIVO |
|---|---|---|
| **`0`** ★ | Available | **Ready Mode apenas** |
| `1` | Available at Start | Ready, Start |
| `2` | Available with End of Cycle | Ready, End of Cycle |
| `3` | Available with Start and End of Cycle | Ready, End of Cycle, Start |
| `4` | Vend is not satisfied | Ready, Partial Vend |
| `5` | Vend not satisfied with End of Cycle | Ready, Partial Vend, End of Cycle |
| `6` | Available with Vend | **Secador:** Ready, Start · **Lavadora:** Ready, Partial Vend, Start |
| `7` | Available with Vend and End of Cycle | Ready, End of Cycle, Partial Vend, Start |
| `8` | **Machine is In Use** | **Secador:** Run Mode · **Lavadora:** Door Locking, Door/Lid Unlocking, Overflow, Pause, Run |
| `9` | Cycle is Complete | End of Cycle |
| `10` | Lucky Cycle | 10 s ao entrar em Start Mode |
| `11` | Break In Alarm | 15 s junto com o tom de alarme |
| `12` | End of Cycle is Entered | 3 s ao entrar em End of Cycle |
| `13` | Motor Running | Enquanto o motor gira |
| `14` | Machine Error Present | Machine Error Mode |

★ padrão de fábrica e valor usado em produção.

### Semântica de `AtyPE = 0` (o que usamos)

`0` **não é** "livre/ocupada" — é "está em Ready ou não". O pino fica **inativo** também em
Partial Vend, Start, End of Cycle, Lockout, Shutdown, Out of Order, low-power e erro.
Combinado com a fiação (open-collector + pull-up interno do ESP), dá:

```
Ready Mode        → transistor conduz → availPin LOW  → livre
Qualquer outro    → transistor aberto → availPin HIGH → ocupada
```

### Alternativa registrada, não adotada

`AtyPE = 8` ("Machine is In Use") é semanticamente mais próximo de `is_in_use`, mas
**inverte a polaridade**: o sinal fica ativo enquanto roda, então `LOW = ocupada`.
Exigiria uma flag `availInvert` no NVS do C3/S3. **Não foi adotado** — o `0` está
atendendo em produção.

---

## 5. Entrada no Manual Mode

| | Secador (`D518600`) | Lavadora (`807870` / `205160`) |
|---|---|---|
| **Entrar** | Desconectar o bullet connector (fios *branco/preto* ↔ *vermelho/azul*), depois segurar `HIGH TEMP` + apertar `MED TEMP` | Abrir o bullet connector por ≥1 s e fechar, depois segurar `REGULAR` + apertar `NORMAL` |
| **Display** | `rAPid` | `rAPid` |
| **Navegar ∧ / ∨** | `LOW TEMP` / `DELICATES` | `DELICATE` / `HOT` |
| **Entrar** | `START` | `START` |
| **Voltar / sair** | `MED TEMP` (`<`) | `WARM` (`<`) |
| **Timeout** | 5 min para começar | 4 min 15 s sem tecla |

Grupos do Manual Mode: `rAPid`, `Prog`, `AUdit`, `rESEt`, `diAg`, `WiFi`.

> Reconectar o bullet connector ao terminar — sem ele a máquina não opera normalmente.

---

## 6. Testes de diagnóstico

Entrar em `diAg` → `START` → rolar com ∧ / ∨ → `START` para ler.

### Comuns às duas máquinas

| Teste | O que é | Display |
|---|---|---|
| `d 1` | **Versão do software do controle** | Secador `XXX.YYY` · Lavadora `S --` |
| `d 12` | **Vend Header Present** (jumper) | `CHCL` presente · `CHoP` ausente |
| `d 13` | **Start Pulse** — contador | `St xx` (incrementa a cada pulso recebido) |
| `d 29` | Machine Configuration #1 | `A xxx` — bitmap, ver §7 |
| `d 32` | Machine Configuration #4 | `d xxx` — bitmap, ver §7 |
| `d 47` | Card Reader Communication | `noinit` / `noCnFg` / `CoMxxx` |

`d 30`, `d 31` e `d 33` **não são usados** nesses modelos.

### Só no secador

`d 2` I/O Board SW · `d 8` Service Door · `d 9` Coin Vault · `d 10`/`d 11` Coin Drop ·
`d 14` Dryer-on Temp · `d 15` Door Switch · `d 19` Thermistor.

### Só na lavadora

`d 3` Drive Board SW · `d 7` Water Level Trim · `d 15` Door Switch · `d 16` Door Lock ·
`d 17`/`d 18` Fill/Drain Time · `d 22` Out of Balance · `d 24` Water Purge ·
`d 25` Water Leak · `d 26` Water Level · `d 27` DC Bus · `d 28` AC Mains.

#### ⭐ `d 23` — External Output Test (só lavadora)

**O teste mais valioso para o nosso problema.** Permite ligar e desligar as saídas
externas na mão:

1. Máquina em **Ready ou Start Mode** (o teste não entra em outros modos).
2. Entrar no `d 23` → aparece `ES 1`.
3. Rolar com ∧ / ∨ entre as saídas.
4. `START` energiza a saída selecionada → display muda para `ES 1on`.
5. `START` de novo ou `WARM` desliga.

Com isso dá para medir no `availPin` do ESP e separar em 2 minutos "fiação errada" de
"controle não gera a saída".

> **O secador não tem esse teste.** Nele o jeito é usar o próprio `AtyPE = 0` como
> gerador: parado em Ready → `availPin` LOW; rodando um ciclo → `availPin` HIGH.

---

## 7. Leitura dos bitmaps d 29 e d 32

### Regra prática

> **Divida o valor por 8 e olhe o resto.**
> Resto **4, 5, 6 ou 7** → o item está **presente**.
> Resto **0, 1, 2 ou 3** → **ausente**.

Vale para os quatro casos (o bit de peso 4 é sempre o que interessa).

### `d 29` — Machine Configuration #1

| Peso | Secador | Lavadora |
|---|---|---|
| 32 | — | Life Test Jumper |
| 16 | — | Comm Board "B" Header |
| 8 | Network Board | Comm Board "A" Header |
| **4** | **Vend Connection** | **Vend Connection** |
| 2 | Coin Drop #2 | Coin Drop #2 |
| 1 | Coin Drop #1 | Coin Drop #1 |

No secador só existem 8 valores: `0` e `8` → vend **ausente**; `5 6 7 13 14 15` → vend **presente**.

### `d 32` — Machine Configuration #4

| Peso | Secador | Lavadora |
|---|---|---|
| 128 | — | Pump (off) / Gravity Drain (on) |
| 64 | — | Heater habilitado |
| **4** | **Payment System Present** | **Payment System Present** |
| 2 | — | Energy Cycle |
| 1 | 120 VAC Supply | 240 V Supply |

Secador: só `0`, `1`, `4`, `5`. `0`/`1` → sem sistema de pagamento; `4`/`5` → com.

> `d 32` reflete a **configuração de fábrica do controle**, não algo ajustável pelo
> teclado. Se der "sem sistema de pagamento", é caso de assistência Speed Queen.

---

## 8. Diagnóstico do caso em campo

### O que o start pulse funcionando nas 3 já elimina

| Hipótese | Status | Por quê |
|---|---|---|
| Porta H13 desconfigurada | ❌ **Eliminada** | 3 dos 4 valores de `SErPAy` inibem o Start Pulse. Se o pulso chega, já está em `S PLSE` |
| Vend header ausente (causa única) | ❌ **Eliminada** | Com o header aberto o controle ignoraria as entradas de crédito (`CErr 3`) |
| **Par AVAIL mal ligado** | ✅ **Principal** | START IN e AVAIL OUT são pinos diferentes do mesmo conector — o pulso prova só o START IN |
| `AtyPE` ausente = firmware antigo | ⚠️ **Em aberto** | Bloco de vend responde mas a opção não aparece. O `d 1` responde |

### Hipótese principal — pinagem H3 vs H13

Fiação validada do Foreasy (Quantum 440G, **H3**):

| Pino H3 | Função | Vai para |
|---|---|---|
| H3-7 | START IN | `startPin` (100 Ω interno do H11L1) |
| H3-4 | AVAIL COL | `availPin` (pull-up interno do ESP, ~45 kΩ) |
| H3-5 | AVAIL EMIT | GND |
| H3-2 | COM | GND |

Estas três máquinas usam **H13**. Se o chicote foi montado com a pinagem do H3, o
START IN pode acertar por coincidência — que é exatamente o que se observa — e o par
AVAIL COL / AVAIL EMIT cair em pinos errados.

Dois pontos que não aparecem em manual de programação e costumam ser a causa:

- **GND do ESP tem que estar no COM da máquina.** Sem referência comum o pino flutua
  e a leitura não significa nada.
- **O AVAIL é open-collector.** Só funciona com o pull-up interno do ESP, que o
  firmware já configura (`INPUT_PULLUP`, debounce 50 ms).

### Sequência recomendada

1. **Lavadora, `d 23`** — força a saída e mede no `availPin`. Programação já confirmada correta, então isola fiação vs controle.
2. **Secadores, `d 1`** — compara a versão de software com um secador em que o `AtyPE` aparece.
3. **`d 12`, `d 29`, `d 32`** nas três — documenta o estado do vend header e da configuração.
4. **`Prog` nos secadores** — anotar exatamente onde a lista pula: aparece `SErPAy`? aparece `PULSE`? vai de `PULSE` direto para `dFtCyC`?

### Verificação pelo firmware (sem multímetro)

- WS `0x05` → `{"type":"avail","livre":bool,"raw":-1/0/1,"sinceMs":ms,"availEn":0/1}`
- Páginas `/status` e `/info` do ESP
- `availEn` precisa estar **on**, senão não há `TokenInserted` / `TokenFinished`

> Se o `raw` ficar travado enquanto a máquina muda de estado, o pino não está recebendo
> o sinal — é hardware, não firmware.

---

## 9. Pendências

- [ ] Ficha de campo preenchida nas 3 máquinas (`d 1`, `d 12`, `d 29`, `d 32`, `d 23`)
- [ ] **Diagrama elétrico / manual de instalação** dessas máquinas — os manuais de programação **não trazem fiação**. Sem ele não dá para confirmar a pinagem do H13
- [ ] Confirmar qual lavadora é (frontal `807870` ou TV6000WN `205160`) pelo nº de série
- [ ] Se confirmada a divergência H3/H13: atualizar o [CLAUDE.md §6](../CLAUDE.md) com uma seção separada para a plataforma H13

---

## 10. Referências

| Documento | Seção | Página |
|---|---|---|
| `D518600ENR3` (secador) | `SErPAy` | 36 |
| | `AtyPE` | 38 |
| | Manual Mode | 18 |
| | Diagnósticos | 63–68 |
| | Config #1 / #4 | 67 / 68 |
| `807870ENR5` (lavadora frontal) | `SErPAy` | 48 |
| | `AtyPE` | 50 |
| | Manual Mode | 23 |
| | Diagnósticos (quick ref) | 99–100 |
| | Descrições dos testes | 102–104 |
| | Config #1 / #4 | 104–106 / 107–108 |
| `205160ENR3` (TV6000WN) | `SErPAy` / `AtyPE` | 40 / 42 |
| | Diagnósticos | 75–76 |

Roteiro de campo (versão para celular/impressão):
<https://claude.ai/code/artifact/5c347927-9084-453e-8260-2975ab8d7034>
