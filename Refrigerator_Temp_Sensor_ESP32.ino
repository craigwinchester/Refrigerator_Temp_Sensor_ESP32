#define TESTING 0  // 1 = no emails sent, 0 = normal operation

#include <TFT_eSPI.h>
#include <SPI.h>
#include "Okuda_A5PL20pt7b.h"
#include <WiFi.h>
#include <ESP_Mail_Client.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>

// ================= Wi-Fi =================
const char* ssid     = "Twonate";
const char* password = "somethingeasy";

// ================= Defaults & Settings =================
// Temperature thresholds (editable via web UI)
float FRIDGE_LOW   = 35.0f;   // Alert if < 35°F (avoid freezing)
float FRIDGE_HIGH  = 46.0f;   // Alert if > 46°F (out of safe range)
float FREEZER_HIGH = 10.0f;   // Alert if freezer > 10°F

// Email settings (editable via web UI except password)
String SMTP_HOST_STR   = "mail.lofi-wines.com";
uint16_t SMTP_PORT_VAL = 465;                  // 465 (SSL) or 587 (STARTTLS)
String AUTHOR_EMAIL_S  = "fridge_monitor@lofi-wines.com";
String RECIPIENT_EMAIL_S = "craig@lofi-wines.com";
// Keep password hardcoded; do not expose in UI
#define AUTHOR_PASSWORD "itsSOcold!!"

// Runtime Testing flag (editable via web UI)
bool testingMode = false;

// ===== DS18B20 (two probes on separate GPIOs) =====
#define ONE_WIRE_FRIDGE 21
#define ONE_WIRE_FREEZER 13
OneWire oneWireFridge(ONE_WIRE_FRIDGE);
OneWire oneWireFreezer(ONE_WIRE_FREEZER);
DallasTemperature fridgeBus(&oneWireFridge);
DallasTemperature freezerBus(&oneWireFreezer);

// ================= Email (ESP_Mail_Client) =================
SMTPSession smtp;

static void addRecipientsFromCSV(SMTP_Message &message, const String &csv) {
  int start = 0;
  int idx = 1;
  while (true) {
    int comma = csv.indexOf(',', start);
    String addr = (comma == -1) ? csv.substring(start) : csv.substring(start, comma);
    addr.trim();
    if (addr.length()) {
      // quick sanity check; optional
      if (addr.indexOf('@') > 0 && addr.indexOf('.') > 0) {
        String name = "Recipient " + String(idx++);
        message.addRecipient(name.c_str(), addr.c_str());  // name + email
        // Alternatively: message.addRecipient(addr.c_str(), addr.c_str());
      }
    }
    if (comma == -1) break;
    start = comma + 1;
  }
}

void sendAlert(String subject, String body) {
  if (testingMode || TESTING) {
    Serial.println("TEST MODE: Email not sent.");
    Serial.println("Subject: " + subject);
    Serial.println("Body: " + body);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skip sendAlert: WiFi not connected.");
    return;
  }

  SMTP_Message message;
  message.sender.name  = "Fridge Monitor";
  message.sender.email = AUTHOR_EMAIL_S.c_str();
  message.subject      = subject;
  message.text.content = body;
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  // multiple recipients supported via CSV in RECIPIENT_EMAIL_S
  addRecipientsFromCSV(message, RECIPIENT_EMAIL_S);

  smtp.callback([](SMTP_Status status) {
    Serial.println(status.info());
    if (status.success()) Serial.println("Email sent successfully.");
    else Serial.println("Email send failed.");
  });

  ESP_Mail_Session session;
  session.server.host_name = SMTP_HOST_STR.c_str();
  session.server.port      = SMTP_PORT_VAL;
  session.login.email      = AUTHOR_EMAIL_S.c_str();
  session.login.password   = AUTHOR_PASSWORD;
  session.login.user_domain = "";
  session.secure.startTLS  = (SMTP_PORT_VAL == 587);

  if (!smtp.connect(&session)) { Serial.println("SMTP connection failed"); return; }
  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.println(String("Send failed: ") + smtp.errorReason());
  }
  smtp.closeSession();
}

// ================= Display =================
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 170
TFT_eSPI tft = TFT_eSPI();  // TFT display
uint16_t LCARS_ORANGE = tft.color565(255, 153, 51);
uint16_t LCARS_PINK   = tft.color565(255, 102, 204);
uint16_t LCARS_BLUE   = tft.color565(102, 204, 255);
uint16_t LCARS_YELLOW = tft.color565(255, 255, 102);
uint16_t LCARS_BLACK  = TFT_BLACK;
int headerHeight = SCREEN_HEIGHT / 3;

void drawLCARSLayout() {
  tft.fillScreen(LCARS_BLACK);
  int radius = 12;
  tft.fillRoundRect(0, 0, SCREEN_WIDTH, headerHeight + 1, radius, LCARS_ORANGE);
  tft.fillTriangle(0, headerHeight, 0, headerHeight - radius, radius, headerHeight, LCARS_ORANGE);
  tft.fillTriangle(SCREEN_WIDTH, headerHeight, SCREEN_WIDTH, headerHeight - radius, SCREEN_WIDTH - radius, headerHeight, LCARS_ORANGE);
  tft.setFreeFont(&Okuda_A5PL20pt7b);
  tft.setTextColor(LCARS_BLACK, LCARS_ORANGE);
  tft.setCursor(10, headerHeight / 2 + 8);
  tft.println("REFRIGERATION SYSTEM");
  tft.drawFastHLine(0, headerHeight, SCREEN_WIDTH, LCARS_BLACK);
  int contentY = headerHeight + 1;
  int contentHeight = SCREEN_HEIGHT - headerHeight - 1;
  tft.fillRoundRect(0, contentY, 40, contentHeight, 10, LCARS_PINK);
}

void drawWiFiIcon() {
  int x = SCREEN_WIDTH - 20, y = 10;
  tft.fillRect(x - 14, y, 20, 20, LCARS_ORANGE);
  if (WiFi.status() == WL_CONNECTED) {
    tft.fillRect(x - 12, y + 10, 3, 3, LCARS_YELLOW);
    tft.fillRect(x - 8,  y + 6,  3, 7, LCARS_YELLOW);
    tft.fillRect(x - 4,  y + 2,  3, 11, LCARS_YELLOW);
  } else {
    tft.drawLine(x - 10, y + 2,  x - 2, y + 10, TFT_RED);
    tft.drawLine(x - 10, y + 10, x - 2, y + 2,  TFT_RED);
  }
}

void drawTemps(float fridgeF, float freezerF) {
  // Clear a generous area
  tft.fillRect(45, 65, 275, 85, LCARS_BLACK);
  tft.setFreeFont(&Okuda_A5PL20pt7b);
  tft.setTextColor(LCARS_YELLOW, LCARS_BLACK);
  tft.setTextPadding(220);
  tft.setCursor(60, 90);
  if (isnan(fridgeF)) tft.println("FRIDGE: --.- F"); else tft.printf("FRIDGE: %.1f F", fridgeF);
  tft.setCursor(60, 130);
  if (isnan(freezerF)) tft.println("FREEZER: --.- F"); else tft.printf("FREEZER: %.1f F", freezerF);
  tft.setTextPadding(0);
}

// ================= Sensors =================
bool setupSensorsDual() {
  pinMode(ONE_WIRE_FRIDGE, INPUT_PULLUP);
  pinMode(ONE_WIRE_FREEZER, INPUT_PULLUP);
  fridgeBus.begin(); freezerBus.begin();
  fridgeBus.setWaitForConversion(true);  fridgeBus.setCheckForConversion(true);
  freezerBus.setWaitForConversion(true); freezerBus.setCheckForConversion(true);
  fridgeBus.setResolution(12); freezerBus.setResolution(12);
  int cf = fridgeBus.getDeviceCount();
  int cz = freezerBus.getDeviceCount();
  Serial.printf("Fridge bus devices (GPIO %d): %d ", ONE_WIRE_FRIDGE, cf);
  Serial.printf("Freezer bus devices (GPIO %d): %d ", ONE_WIRE_FREEZER, cz);
  if (cf < 1) Serial.println("⚠️ No DS18B20 detected on FRIDGE bus.");
  if (cz < 1) Serial.println("⚠️ No DS18B20 detected on FREEZER bus.");
  return (cf > 0) && (cz > 0);
}

float readTempF_ByIndex0(DallasTemperature &bus) {
  bus.requestTemperatures();
  float c = bus.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C || fabsf(c - 85.0f) < 0.01f) return NAN;  // ignore bogus
  return DallasTemperature::toFahrenheit(c);
}

// ================= Alerts =================
void checkFridgeAlert(float f) {
  static bool alertActive = false; static unsigned long lastAlert = 0;
  const unsigned long interval = 15UL * 60UL * 1000UL; // 15 min
  bool out = (!isnan(f)) && (f < FRIDGE_LOW || f > FRIDGE_HIGH);
  if (out) {
    if (!alertActive && (millis() - lastAlert > interval)) {
      sendAlert("🚨 Fridge Temp Alert", "Fridge out of range: " + String(f,1) +
                " F (limits " + String(FRIDGE_LOW) + "-" + String(FRIDGE_HIGH) + ")");
      alertActive = true; lastAlert = millis();
    }
  } else alertActive = false;
}

void checkFreezerAlert(float f) {
  static bool alertActive = false; static unsigned long lastAlert = 0;
  const unsigned long interval = 15UL * 60UL * 1000UL; // 15 min
  bool out = (!isnan(f)) && (f > FREEZER_HIGH);
  if (out) {
    if (!alertActive && (millis() - lastAlert > interval)) {
      sendAlert("🚨 Freezer Temp Alert", "Freezer warm: " + String(f,1) +
                " F (limit <= " + String(FREEZER_HIGH) + ")");
      alertActive = true; lastAlert = millis();
    }
  } else alertActive = false;
}

// ================= Persistence =================
Preferences prefs; // NVS

void loadSettings() {
  prefs.begin("fridge", true);
  FRIDGE_LOW   = prefs.getFloat("fr_lo", FRIDGE_LOW);
  FRIDGE_HIGH  = prefs.getFloat("fr_hi", FRIDGE_HIGH);
  FREEZER_HIGH = prefs.getFloat("fz_hi", FREEZER_HIGH);
  testingMode  = prefs.getBool("testing", testingMode);
  String s;
  s = prefs.getString("smtp_host", SMTP_HOST_STR); SMTP_HOST_STR = s.length()? s : SMTP_HOST_STR;
  SMTP_PORT_VAL = prefs.getUShort("smtp_port", SMTP_PORT_VAL);
  s = prefs.getString("from", AUTHOR_EMAIL_S); AUTHOR_EMAIL_S = s.length()? s : AUTHOR_EMAIL_S;
  s = prefs.getString("to",   RECIPIENT_EMAIL_S); RECIPIENT_EMAIL_S = s.length()? s : RECIPIENT_EMAIL_S;
  prefs.end();
}

void saveSettings() {
  prefs.begin("fridge", false);
  prefs.putFloat("fr_lo", FRIDGE_LOW);
  prefs.putFloat("fr_hi", FRIDGE_HIGH);
  prefs.putFloat("fz_hi", FREEZER_HIGH);
  prefs.putBool ("testing", testingMode);
  prefs.putString("smtp_host", SMTP_HOST_STR);
  prefs.putUShort("smtp_port", SMTP_PORT_VAL);
  prefs.putString("from", AUTHOR_EMAIL_S);
  prefs.putString("to",   RECIPIENT_EMAIL_S);
  prefs.end();
}

// ================= Data Log (RAM ring buffer) =================
struct Sample { uint32_t tSec; float fFridge; float fFreezer; };
const size_t MAX_SAMPLES = 720; // ~12 hours @ 60s/sample
Sample samples[MAX_SAMPLES];
size_t sampleHead = 0, sampleCount = 0;
uint32_t lastSampleSec = 0; // push every 60s

void pushSample(float fFridge, float fFreezer) {
  uint32_t nowSec = millis()/1000;
  if (nowSec - lastSampleSec < 60) return; // every 60 seconds
  lastSampleSec = nowSec;
  Sample s{nowSec, fFridge, fFreezer};
  samples[sampleHead] = s;
  sampleHead = (sampleHead + 1) % MAX_SAMPLES;
  if (sampleCount < MAX_SAMPLES) sampleCount++;
}

// ================= Web Server =================
WebServer server(80);

String htmlIndex() {
  String h = R"HTML(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>FridgeTemp</title>
<style>
 body{background:#000;color:#fff;font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0}
 header{background:#FF9933;padding:12px 16px;color:#000;font-weight:700}
 main{padding:16px}
 .row{display:flex;gap:16px;flex-wrap:wrap}
 .card{background:#111;border-radius:12px;padding:16px;flex:1;min-width:260px}
 .label{color:#FFCC33}
 input,button,select{background:#111;color:#fff;border:1px solid #333;border-radius:8px;padding:8px}
 button{cursor:pointer}
 canvas{width:100%;height:240px;display:block;background:#080808;border-radius:12px}
 small{color:#aaa}
</style>
</head><body>
<header>REFRIGERATION SYSTEM — fridgetemp.local</header>
<main>
  <div class="row">
    <div class="card"><div class="label">Current</div>
      <div id="cur">Loading...</div>
      <small id="status"></small>
    </div>
    <div class="card"><div class="label">Settings</div>
      <form id="f" onsubmit="save(event)">
        <div>Fridge Low (°F): <input type="number" step="0.1" id="fr_lo"></div>
        <div>Fridge High (°F): <input type="number" step="0.1" id="fr_hi"></div>
        <div>Freezer High (°F): <input type="number" step="0.1" id="fz_hi"></div>
        <div>From Email: <input type="email" id="from"></div>
        <div>To Email(s): <input type="text" id="to" placeholder="a@b.com, c@d.com"></div>
        <div>SMTP Host: <input type="text" id="smtp_host"></div>
        <div>SMTP Port: <input type="number" id="smtp_port"></div>
        <div style="display:flex;align-items:center;gap:.5rem;margin-top:8px">
          <input type="checkbox" id="testing">
          <label for="testing">Enable Testing Mode (no emails/alerts)</label>
        </div>
        <button>Save</button>
      </form>
      <small id="testing_badge"></small>
    </div>
  </div>
  <div class="card">
    <div class="label">Last 12 hours</div>
    <canvas id="g"></canvas>
  </div>
</main>
<script>
async function fetchJSON(u){const r=await fetch(u);return r.json();}
function fmt(v){return (v==null||isNaN(v))?"--.-":v.toFixed(1);}
function drawGraph(data){
  const c=document.getElementById('g');
  const d=c.getContext('2d');
  c.width=c.clientWidth; c.height=240;
  const W=c.width, H=c.height;

  const LM=40, RM=10, TM=10, BM=24;

  d.clearRect(0,0,W,H);
  d.fillStyle='#fff';
  d.strokeStyle='#333';
  d.lineWidth=1;
  d.font='12px system-ui,Segoe UI,Roboto,Arial,sans-serif';
  d.textBaseline='middle';

  const n=(data && data.t)? data.t.length : 0;
  if(!n){ return; }

  const t0=data.t[0], t1=data.t[n-1];
  const span=Math.max(1,(t1-t0));

  const all = data.f.concat(data.z).filter(v=>v!=null && isFinite(v));
  let lo = Math.min(...all), hi = Math.max(...all);
  if(!isFinite(lo)||!isFinite(hi)){ lo=0; hi=1; }
  const pad = Math.max(1,(hi-lo)*0.1);
  lo = Math.floor((lo-pad)*2)/2;
  hi = Math.ceil ((hi+pad)*2)/2;

  const xOf = (t)=> LM + ((t - t0)/span) * (W - LM - RM);
  const yOf = (v)=> TM + (1 - (v - lo)/(hi - lo)) * (H - TM - BM);

  d.strokeStyle='#222';
  d.fillStyle='#aaa';
  const yTicks=5;
  for(let i=0;i<=yTicks;i++){
    const v = lo + (i*(hi-lo)/yTicks);
    const y = yOf(v);
    d.beginPath(); d.moveTo(LM,y); d.lineTo(W-RM,y); d.stroke();
    d.textAlign='right';
    d.fillText(v.toFixed(0)+'°', LM-6, y);
  }

  const xTicks=6;
  for(let i=0;i<=xTicks;i++){
    const t = t0 + i*(span/xTicks);
    const x = xOf(t);
    d.strokeStyle='#222';
    d.beginPath(); d.moveTo(x, TM); d.lineTo(x, H-BM); d.stroke();

    const mins = Math.round((t1 - t)/60);
    let label = 'now';
    if (mins>0) {
      if (mins>=60) {
        const h = Math.floor(mins/60), m = mins%60;
        label = h+'h'+(m? (m+'m'):'');
      } else {
        label = mins+'m';
      }
    }
    d.fillStyle='#aaa';
    d.textAlign='center';
    d.textBaseline='top';
    d.fillText(label, x, H-BM+4);
  }

  d.strokeStyle='#555';
  d.lineWidth=1.5;
  d.beginPath(); d.moveTo(LM, TM); d.lineTo(LM, H-BM); d.lineTo(W-RM, H-BM); d.stroke();

  function plot(arr,color){
    d.strokeStyle=color; d.lineWidth=2;
    let started=false;
    d.beginPath();
    for(let i=0;i<n;i++){
      const v = arr[i];
      if(v==null || !isFinite(v)) continue;
      const x=xOf(data.t[i]), y=yOf(v);
      if(!started){ d.moveTo(x,y); started=true; } else { d.lineTo(x,y); }
    }
    if (started) d.stroke();
  }

  plot(data.f,'#FFCC33'); // fridge
  plot(data.z,'#66CCFF'); // freezer
}

async function refresh(){
  const s=await fetchJSON('/api/status');
  document.getElementById('cur').innerText = `Fridge: ${fmt(s.fridge)} °F
Freezer: ${fmt(s.freezer)} °F`;
  document.getElementById('status').innerText = `WiFi: ${s.wifi?'connected':'offline'}  |  Alerts: fridge ${s.fr_lo}-${s.fr_hi} °F, freezer <= ${s.fz_hi} °F`;
  const f=document.getElementById('f');
  f.fr_lo.value=s.fr_lo; f.fr_hi.value=s.fr_hi; f.fz_hi.value=s.fz_hi; f.from.value=s.from; f.to.value=s.to; f.smtp_host.value=s.smtp_host; f.smtp_port.value=s.smtp_port;
  document.getElementById('testing').checked = !!s.testing;
  document.getElementById('testing_badge').innerText = s.testing ? 'TESTING MODE is ON — alerts/emails suppressed.' : '';
  const d=await fetchJSON('/api/data');
  drawGraph(d);
}
async function save(e){
  e.preventDefault();
  const p={
    fr_lo: parseFloat(document.getElementById('fr_lo').value),
    fr_hi: parseFloat(document.getElementById('fr_hi').value),
    fz_hi: parseFloat(document.getElementById('fz_hi').value),
    from:  document.getElementById('from').value,
    to:    document.getElementById('to').value,
    smtp_host: document.getElementById('smtp_host').value,
    smtp_port: parseInt(document.getElementById('smtp_port').value||0),
    testing: document.getElementById('testing').checked
  };
  await fetch('/settings', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(p)});
  alert('Saved');
  refresh();
}
refresh(); setInterval(refresh, 15000);
</script>
</body></html>
)HTML";
  return h;
}

void handleStatus() {
  float fFridge  = readTempF_ByIndex0(fridgeBus);
  float fFreezer = readTempF_ByIndex0(freezerBus);
  String j = "{";
  j += "\"fridge\":" + (isnan(fFridge)? String("null"): String(fFridge,1)) + ",";
  j += "\"freezer\":" + (isnan(fFreezer)? String("null"): String(fFreezer,1)) + ",";
  j += "\"wifi\":" + String(WiFi.status()==WL_CONNECTED?"true":"false") + ",";
  j += "\"fr_lo\":" + String(FRIDGE_LOW,1) + ",\"fr_hi\":" + String(FRIDGE_HIGH,1) + ",\"fz_hi\":" + String(FREEZER_HIGH,1) + ",";
  j += "\"from\":\"" + AUTHOR_EMAIL_S + "\",\"to\":\"" + RECIPIENT_EMAIL_S + "\",";
  j += "\"smtp_host\":\"" + SMTP_HOST_STR + "\",\"smtp_port\":" + String(SMTP_PORT_VAL) + ",";
  j += "\"testing\":" + String(testingMode ? "true" : "false");
  j += "}";
  server.send(200, "application/json", j);
}

void handleData() {
  // Export samples as arrays; nulls for NaN
  String j = "{";
  j += "\"t\":[";
  for (size_t i=0;i<sampleCount;i++){
    size_t idx = (sampleHead + MAX_SAMPLES - sampleCount + i) % MAX_SAMPLES;
    j += String(samples[idx].tSec); if (i+1<sampleCount) j += ",";
  }
  j += "],\"f\":[";
  for (size_t i=0;i<sampleCount;i++){
    size_t idx = (sampleHead + MAX_SAMPLES - sampleCount + i) % MAX_SAMPLES;
    if (isnan(samples[idx].fFridge)) j += "null"; else j += String(samples[idx].fFridge,1);
    if (i+1<sampleCount) j += ",";
  }
  j += "],\"z\":[";
  for (size_t i=0;i<sampleCount;i++){
    size_t idx = (sampleHead + MAX_SAMPLES - sampleCount + i) % MAX_SAMPLES;
    if (isnan(samples[idx].fFreezer)) j += "null"; else j += String(samples[idx].fFreezer,1);
    if (i+1<sampleCount) j += ",";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleRoot(){ server.send(200, "text/html", htmlIndex()); }

void handleSettings() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    // Tiny JSON helpers
    auto getVal = [&](const char* key)->String{
      int k = body.indexOf(String("\"") + key + "\"");
      if (k<0) return String();
      int c = body.indexOf(':', k); if (c<0) return String();
      int q1 = body.indexOf('"', c+1);
      int q2 = body.indexOf('"', q1+1);
      if (q1>=0 && q2>q1) return body.substring(q1+1, q2);
      // number fallback
      int comma = body.indexOf(',', c+1); if (comma<0) comma = body.indexOf('}', c+1);
      return body.substring(c+1, comma);
    };
    auto getBool = [&](const char* key)->int{
      int k = body.indexOf(String("\"") + key + "\"");
      if (k < 0) return -1;
      int c = body.indexOf(':', k); if (c < 0) return -1;
      int p = c+1; while (p < (int)body.length() && (body[p]==' ' || body[p]=='\t')) p++;
      if (body.startsWith("true", p))  return 1;
      if (body.startsWith("false", p)) return 0;
      int comma = body.indexOf(',', p); if (comma<0) comma = body.indexOf('}', p);
      String sub = body.substring(p, comma); sub.trim();
      if (sub == "1") return 1;
      if (sub == "0") return 0;
      return -1;
    };

    String v;
    v = getVal("fr_lo"); if (v.length()) FRIDGE_LOW = v.toFloat();
    v = getVal("fr_hi"); if (v.length()) FRIDGE_HIGH = v.toFloat();
    v = getVal("fz_hi"); if (v.length()) FREEZER_HIGH = v.toFloat();
    v = getVal("from");  if (v.length()) AUTHOR_EMAIL_S = v;
    v = getVal("to");    if (v.length()) RECIPIENT_EMAIL_S = v;
    v = getVal("smtp_host"); if (v.length()) SMTP_HOST_STR = v;
    v = getVal("smtp_port"); if (v.length()) SMTP_PORT_VAL = (uint16_t) v.toInt();
    int b = getBool("testing"); if (b != -1) testingMode = (b == 1);

    saveSettings();
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void startWeb(){
  if (MDNS.begin("fridgetemp")) { Serial.println("mDNS started: http://fridgetemp.local/"); }
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/data", handleData);
  server.on("/settings", handleSettings);
  server.begin();
}

// ================= Setup/Loop =================
void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(1);
  drawLCARSLayout();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int retries = 30; while (WiFi.status() != WL_CONNECTED && retries-- > 0) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status()==WL_CONNECTED? "WiFi connected!":"WiFi connection failed!");
  drawWiFiIcon();

  loadSettings();
  setupSensorsDual();
  startWeb();

  drawTemps(NAN, NAN);
}

void loop() {
  static unsigned long lastWiFi = 0, lastRead = 0;
  server.handleClient();

  if (millis() - lastWiFi > 10000) { drawWiFiIcon(); lastWiFi = millis(); }

  if (millis() - lastRead > 5000) {
    float fFridge  = readTempF_ByIndex0(fridgeBus);
    float fFreezer = readTempF_ByIndex0(freezerBus);
    Serial.printf("Fridge: %.1f F | Freezer: %.1f F\n", fFridge, fFreezer);
    drawTemps(fFridge, fFreezer);
    checkFridgeAlert(fFridge);
    checkFreezerAlert(fFreezer);
    pushSample(fFridge, fFreezer);
    lastRead = millis();
  }
}
