// ============================================================================
// Foreasy — AVAIL OUT + Pulso START IN (Speed Queen, conector H3/AP1)
//
// Protótipo de bancada: AP próprio + servidor HTTP. Sem WiFi STA, sem WS.
// Conecte no AP e acesse http://192.168.4.1
//
// LIGAÇÃO — AVAIL OUT:
//   GPIO4 ──── H3-4 (AVAIL COL — coletor 4N25)   [pull-up interno do ESP32]
//   GND ESP32 ── H3-5 (AVAIL EMIT) ── H3-2 (COM)
//
// Sem resistor externo: usa o pull-up interno do GPIO (~45kΩ). O 4N25 só
// precisa puxar a linha pra GND quando conduz — corrente bem dentro do
// Imax (5mA) do AVAIL OUT.
//
// LIGAÇÃO — START IN:
//   GPIO5 ──── H3-7 (START IN)   [sem resistor externo]
//   GND ESP32 ── H3-2 (COM)   [mesmo nó do AVAIL]
//
// Sem resistor externo: o 100Ω interno do H11L1 já limita a corrente.
// (3.3V - ~1.3V do LED) / 100Ω ≈ 20mA — dentro da faixa 3-30mA do H11L1
// e dentro do limite seguro do GPIO do ESP32.
//
// LÓGICA AVAIL OUT ("ON = AVAILABLE" no schematic 807300):
//   LOW  = máquina LIVRE
//   HIGH = máquina OCUPADA
//
// AP: SSID FOREASY-AVAIL-AP | Senha 12345678 | IP 192.168.4.1
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>

// Identidade do firmware (exposta em /status — protótipo de bancada)
#define FW_VERSION "1.0.0"
#define FW_CHIP    "esp32s3"

WebServer server(80);

const int      AVAIL_PIN   = 4;
const int      START_PIN   = 5;
const uint32_t PULSE_MS    = 100;
const uint32_t DEBOUNCE_MS = 50;

IPAddress apIP(192,168,4,1);

int      lastStableState = -1;
int      lastReading     = -1;
uint32_t lastChangeMs    = 0;
uint32_t lastChangeAtMs  = 0;

bool     pulseActive = false;
uint32_t pulseEndMs  = 0;

// ================= AVAIL / PULSE =================
void readAvailTick() {
  int reading = digitalRead(AVAIL_PIN);

  if (reading != lastReading) {
    lastReading  = reading;
    lastChangeMs = millis();
  }

  if (reading != lastStableState && (millis() - lastChangeMs) >= DEBOUNCE_MS) {
    lastStableState = reading;
    lastChangeAtMs  = millis();
  }
}

void startPulse() {
  if (pulseActive) return;
  pulseActive = true;
  pulseEndMs  = millis() + PULSE_MS;
  digitalWrite(START_PIN, HIGH);
}

void pulseTick() {
  if (pulseActive && (int32_t)(millis() - pulseEndMs) >= 0) {
    pulseActive = false;
    digitalWrite(START_PIN, LOW);
  }
}

// ================= AP =================
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FOREASY-AVAIL-AP", "12345678");
  delay(200);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  Serial.printf("AP: FOREASY-AVAIL-AP | IP: %s\n", WiFi.softAPIP().toString().c_str());
}

// ================= HTTP =================
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foreasy — AVAIL Test</title>
<style>
:root{--bg:#0a0e0b;--cd:#111814;--bd:#1e3028;--ac:#00e676;--ac2:#009e55;--tx:#d4f5e0;--mu:#557060;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:ui-monospace,'Cascadia Code','SF Mono',monospace;font-size:13px}
header{background:var(--cd);border-bottom:1px solid var(--bd);padding:14px 20px}
.logo{color:var(--ac);font-size:18px;font-weight:700;letter-spacing:3px}
.sub{color:var(--mu);font-size:10px;letter-spacing:1px;margin-top:2px}
main{max-width:420px;margin:0 auto;padding:18px 16px 32px}
.state{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:18px;margin-bottom:12px;text-align:center}
.state-lbl{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase;margin-bottom:8px}
.state-val{font-size:28px;font-weight:700}
.it{background:var(--cd);border:1px solid var(--bd);border-radius:3px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}
.lbl{color:var(--mu);font-size:10px;letter-spacing:1px;text-transform:uppercase}
.vl{color:var(--tx);font-size:13px;font-weight:600}
.btn{display:block;width:100%;margin-top:14px;padding:14px;border:none;border-radius:3px;font-family:inherit;font-size:14px;font-weight:700;letter-spacing:2px;cursor:pointer;text-transform:uppercase;background:var(--ac);color:#000}
.btn:hover{background:var(--ac2);color:var(--tx)}
.btn:active{transform:scale(.98)}
</style></head><body>
<header>
  <div class="logo">FOREASY</div>
  <div class="sub">avail out + start in — protótipo</div>
</header>
<main>
  <div class="state">
    <div class="state-lbl">Status da máquina</div>
    <div class="state-val" id="availState">…</div>
  </div>
  <div class="it"><span class="lbl">AVAIL OUT (raw)</span><span class="vl" id="availRaw">…</span></div>
  <div class="it"><span class="lbl">Há quanto tempo</span><span class="vl" id="sinceChange">…</span></div>
  <button class="btn" id="btnPulse">Disparar pulso (100ms)</button>
</main>
<script>
function qs(id){return document.getElementById(id);}
function upd(){
  fetch('/status').then(r=>r.json()).then(j=>{
    qs('availState').textContent = j.livre ? 'LIVRE' : 'OCUPADA';
    qs('availState').style.color = j.livre ? '#00e676' : '#f87171';
    qs('availRaw').textContent   = j.availRaw===0 ? 'LOW' : 'HIGH';
    qs('sinceChange').textContent = (j.sinceChangeMs/1000).toFixed(1)+'s';
  });
}
qs('btnPulse').onclick=()=>{
  fetch('/pulse').then(()=>upd());
};
setInterval(upd,500); upd();
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleStatus() {
  bool livre = (lastStableState == LOW);
  String json = "{";
  json += "\"availRaw\":"      + String(lastStableState) + ",";
  json += "\"livre\":"         + String(livre ? "true" : "false") + ",";
  json += "\"pulseActive\":"   + String(pulseActive ? "true" : "false") + ",";
  json += "\"sinceChangeMs\":" + String(millis() - lastChangeAtMs) + ",";
  json += "\"chip\":\""        + String(FW_CHIP) + "\",";
  json += "\"fw\":\""          + String(FW_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handlePulse() {
  startPulse();
  server.send(200, "text/plain", "Pulso disparado");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(AVAIL_PIN, INPUT_PULLUP);
  pinMode(START_PIN, OUTPUT);
  digitalWrite(START_PIN, LOW);

  lastReading     = digitalRead(AVAIL_PIN);
  lastStableState = lastReading;
  lastChangeMs    = millis();
  lastChangeAtMs  = millis();

  setupAP();

  server.on("/",        handleRoot);
  server.on("/status",  HTTP_GET, handleStatus);
  server.on("/pulse",   HTTP_GET, handlePulse);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server iniciado. Acesse http://192.168.4.1");
}

void loop() {
  server.handleClient();
  readAvailTick();
  pulseTick();
}
