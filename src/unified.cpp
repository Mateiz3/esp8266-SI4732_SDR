#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <SI4735.h>

#define RESET_PIN 2
#define SDA_PIN 4
#define SCL_PIN 5

SI4735 rx;
AsyncWebServer server(80);

const char* ssid = "Si4732_Radio_Hotspot";
const char* password = "password123";

// --- System Mode Flag ---
bool useSerialMode = false;

// --- Radio & Telemetry State ---
int currentFreq = 9610; 
String currentMode = "FM";
int currentVol = 30;

String rdsName = "";
String rdsText = "";
String rdsPty = "";
String rdsTime = "";
int currentPtyCode = -1;
unsigned long lastRdsPoll = 0;
unsigned long lastSerialPush = 0;

bool isScanning = false;
unsigned long lastScanStep = 0;

struct Station { float freq; String mode; int rssi; int snr; };
Station history[15]; 
int historyCount = 0;

// ==========================================
// Helpers
// ==========================================
String escapeJSON(String input) {
  input.replace("\\", "\\\\");
  input.replace("\"", "\\\"");
  input.replace("\n", "");
  input.replace("\r", "");
  return input;
}

String getPtyName(int pty) {
  const char* ptyNames[] = {
    "None", "News", "Info", "Sports", "Talk", "Rock", "Classic Rock", 
    "Adult Hits", "Soft Rock", "Top 40", "Country", "Oldies", "Soft", 
    "Nostalgia", "Jazz", "Classical", "R&B", "Soft R&B", "Language", 
    "Religious", "Religious Talk", "Personality", "Public", "College", 
    "Unassigned", "Unassigned", "Unassigned", "Unassigned", "Unassigned", 
    "Weather", "Emergency Test", "Emergency!"
  };
  if (pty >= 0 && pty <= 31) return String(ptyNames[pty]);
  return "";
}

void clearRDS() {
  rdsName = "";
  rdsText = "";
  rdsPty = "";
  rdsTime = "";
  currentPtyCode = -1;
}

void switchToBand(String newBand, int freq) {
  currentMode = newBand;
  currentFreq = freq;
  isScanning = false; 
  clearRDS();
  
  if (newBand == "FM") {
    rx.setFM(8400, 10800, currentFreq, 10);
    rx.setRdsConfig(1, 2, 2, 2, 2); 
  } 
  else if (newBand == "MW") {
    rx.setAM(520, 1710, currentFreq, 10);
  } 
  else if (newBand == "SW") {
    rx.setAM(1711, 30000, currentFreq, 5);
  }
}

void pushSerialTelemetry() {
  if (!useSerialMode) return;
  rx.getCurrentReceivedSignalQuality();
  float displayFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (currentFreq / 1000.0);
  
  Serial.printf("{\"freq\":%.3f,\"mode\":\"%s\",\"vol\":%d,\"rssi\":%d,\"snr\":%d,\"rds_name\":\"%s\",\"rds_text\":\"%s\",\"rds_pty\":\"%s\",\"rds_time\":\"%s\"}\n", 
                displayFreq, currentMode.c_str(), currentVol, rx.getCurrentRSSI(), rx.getCurrentSNR(), 
                escapeJSON(rdsName).c_str(), escapeJSON(rdsText).c_str(), escapeJSON(rdsPty).c_str(), escapeJSON(rdsTime).c_str());
}

// ==========================================
// HTML / CSS Webpage 
// ==========================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>ESP8266 SDR</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; background: #121212; color: #fff; padding: 15px;}
    .display { background: #222; padding: 15px; border-radius: 10px; border: 2px solid #00ffcc; margin-bottom: 15px;}
    .freq-text { font-size: 3rem; font-weight: bold; color: #00ffcc; }
    .rds-box { color: #ffeb3b; font-weight: bold; min-height: 24px;}
    .pty-badge { background: #e91e63; color: white; padding: 2px 8px; border-radius: 12px; font-size: 0.8rem; display: none; }
    .btn { background: #444; color: white; padding: 12px; margin: 4px; border-radius: 8px; cursor: pointer; border: none;}
    .panel { background: #222; padding: 15px; border-radius: 10px; margin-top: 15px;}
    meter { width: 100%; height: 15px; }
  </style>
</head>
<body>
  <h1>Portable SDR</h1>
  <div class="display">
    <div class="freq-text" id="freq">--.--</div>
    <div><span id="mode">--</span> <span id="rds_pty" class="pty-badge"></span></div>
    <div id="rds_time" style="color:#888; font-size:0.8rem; margin-top:3px; min-height:15px;"></div>
    <div class="rds-box" id="rds_name"></div>
    <div id="rds_text" style="color:#bbb; font-size:0.9rem;"></div>
  </div>
  <div>
    <select id="band_sel" onchange="changeBand()" style="padding:10px; background:#333; color:#fff; border:none;">
      <option value="FM">FM</option><option value="MW">MW</option><option value="SW">SW</option>
    </select>
    <input type="number" id="freqInput" placeholder="MHz" style="padding:10px; width:80px;">
    <button class="btn" style="background:#008CBA;" onclick="tuneDirect()">GO</button>
  </div>
  <button class="btn" onclick="sendCommand('down')"><< DOWN</button>
  <button class="btn" onclick="sendCommand('up')">UP >></button>
  <button class="btn" id="scanBtn" style="background:#9c27b0; width:100%; margin-top:10px;" onclick="sendCommand('scan')">START AUTO-SCAN</button>
  
  <div class="panel">
    <h3>Telemetry & Volume</h3>
    <div>RSSI: <span id="rssi_val"></span></div><meter id="rssi_meter" max="80"></meter>
    <div>SNR: <span id="snr_val"></span></div><meter id="snr_meter" max="40"></meter>
    <input type="range" id="vol" min="0" max="63" style="width:100%; margin-top:15px;" onchange="setVol(this.value)">
  </div>
<script>
let dragging = false;
document.getElementById("vol").onmousedown = () => dragging = true;
document.getElementById("vol").onmouseup = () => dragging = false;

function fetchMetrics() {
  fetch('/metrics').then(r => r.json()).then(data => {
    document.getElementById("freq").innerText = data.freq.toFixed(3);
    document.getElementById("mode").innerText = data.mode;
    document.getElementById("band_sel").value = data.mode;
    document.getElementById("rds_name").innerText = data.rds_name;
    document.getElementById("rds_text").innerText = data.rds_text;
    document.getElementById("rds_time").innerText = data.rds_time;
    
    let pty = document.getElementById("rds_pty");
    if(data.rds_pty && data.rds_pty !== "None") { pty.innerText = data.rds_pty; pty.style.display="inline-block"; }
    else pty.style.display="none";

    document.getElementById("rssi_meter").value = data.rssi;
    document.getElementById("rssi_val").innerText = data.rssi + " dBuV";
    document.getElementById("snr_meter").value = data.snr;
    document.getElementById("snr_val").innerText = data.snr + " dB";
    
    if(!dragging) document.getElementById("vol").value = data.vol;
    let sBtn = document.getElementById("scanBtn");
    sBtn.innerText = data.scanning ? "STOP SCANNING" : "START AUTO-SCAN";
    sBtn.style.background = data.scanning ? "#e91e63" : "#9c27b0";
  });
}
function sendCommand(a) { fetch('/action?cmd=' + a).then(fetchMetrics); }
function tuneDirect() { let f = document.getElementById("freqInput").value; if(f){ fetch('/tune?val='+f).then(fetchMetrics); document.getElementById("freqInput").value='';} }
function changeBand() { fetch('/action?cmd=band&val=' + document.getElementById("band_sel").value).then(fetchMetrics); }
function setVol(v) { fetch("/volume?val=" + v); }
window.onload = fetchMetrics; setInterval(fetchMetrics, 1000);
</script>
</body></html>)rawliteral";

// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  Serial.println("\nBooting Si4732...");
  rx.setup(RESET_PIN, POWER_UP_FM);
  delay(500);
  switchToBand("FM", 9610);
  rx.setVolume(currentVol);

  // Instantly start as a Wi-Fi Hotspot by default
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });

  server.on("/tune", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("val")) {
      float val = r->getParam("val")->value().toFloat();
      if (val >= 64.0 && val <= 108.0) switchToBand("FM", val * 100);
      else if (val >= 0.520 && val <= 1.710) switchToBand("MW", val * 1000);
      else if (val >= 1.711 && val <= 30.0) switchToBand("SW", val * 1000);
      else clearRDS(); 
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/action", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("cmd")) {
      String cmd = r->getParam("cmd")->value();
      if (cmd == "up") { isScanning=false; if(currentMode=="FM" || currentMode=="MW") currentFreq+=10; else currentFreq+=5; rx.setFrequency(currentFreq); clearRDS(); } 
      else if (cmd == "down") { isScanning=false; if(currentMode=="FM" || currentMode=="MW") currentFreq-=10; else currentFreq-=5; rx.setFrequency(currentFreq); clearRDS(); } 
      else if (cmd == "band" && r->hasParam("val")) {
        String val = r->getParam("val")->value();
        if (val == "FM") switchToBand("FM", 9610); else if (val == "MW") switchToBand("MW", 810); else switchToBand("SW", 9600);
      }
      else if (cmd == "scan") { isScanning = !isScanning; clearRDS(); }
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("val")) { currentVol = r->getParam("val")->value().toInt(); rx.setVolume(currentVol); }
    r->send(200, "text/plain", "OK");
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *r){
    rx.getCurrentReceivedSignalQuality();
    float dispFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (currentFreq / 1000.0);
    String json = "{\"rssi\":" + String(rx.getCurrentRSSI()) + ",\"snr\":" + String(rx.getCurrentSNR()) + 
                  ",\"freq\":" + String(dispFreq, 3) + ",\"mode\":\"" + currentMode + "\",\"vol\":" + String(currentVol) + 
                  ",\"scanning\":" + String(isScanning ? "true" : "false") + 
                  ",\"rds_name\":\"" + escapeJSON(rdsName) + "\",\"rds_text\":\"" + escapeJSON(rdsText) + "\",\"rds_pty\":\"" + escapeJSON(rdsPty) + "\",\"rds_time\":\"" + escapeJSON(rdsTime) + "\"}";
    r->send(200, "application/json", json);
  });

  server.begin();
}

// ==========================================
// Loop 
// ==========================================
void loop() {
  
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();   
    if (cmd.length() == 0) return;

    // --- THE HOT-SWAP MAGIC ---
    if (!useSerialMode) {
      useSerialMode = true;
      WiFi.mode(WIFI_OFF);
      WiFi.forceSleepBegin();
      // Wi-Fi is now dead. We are now a dedicated USB Serial device.
    }

    if (cmd == "UP") { if(currentMode=="FM" || currentMode=="MW") currentFreq+=10; else currentFreq+=5; rx.setFrequency(currentFreq); clearRDS(); pushSerialTelemetry(); } 
    else if (cmd == "DOWN") { if(currentMode=="FM" || currentMode=="MW") currentFreq-=10; else currentFreq-=5; rx.setFrequency(currentFreq); clearRDS(); pushSerialTelemetry(); }
    else if (cmd.startsWith("VOL=")) { currentVol = cmd.substring(4).toInt(); rx.setVolume(currentVol); pushSerialTelemetry(); }
    else if (cmd.startsWith("TUNE=")) {
      float iF = cmd.substring(5).toFloat();
      if (iF >= 64.0 && iF <= 108.0) switchToBand("FM", iF * 100); else if (iF >= 0.520 && iF <= 1.710) switchToBand("MW", iF * 1000); else switchToBand("SW", iF * 1000);
      clearRDS(); pushSerialTelemetry();
    }
    else if (cmd.startsWith("BAND=")) {
      String b = cmd.substring(5);
      if (b == "FM") switchToBand("FM", 9610); else if (b == "MW") switchToBand("MW", 810); else switchToBand("SW", 9600);
      pushSerialTelemetry();
    }
    else if (cmd == "STATUS") { pushSerialTelemetry(); }
  }

  // Background RDS Polling
  if (!isScanning && currentMode == "FM" && (millis() - lastRdsPoll > 40)) {
    lastRdsPoll = millis();
    rx.getRdsStatus();
    if (rx.getRdsReceived()) {
      bool changed = false;
      
      char* ps = rx.getRdsStationName(); 
      if (ps != NULL && strlen(ps) > 0 && rdsName != String(ps)) { rdsName = String(ps); changed = true; }
      
      char* pt = rx.getRdsProgramInformation(); 
      if (pt != NULL && strlen(pt) > 0 && rdsText != String(pt)) { rdsText = String(pt); changed = true; }
      
      int pty = rx.getRdsProgramType(); 
      if (pty != currentPtyCode && pty > 0) { currentPtyCode = pty; rdsPty = getPtyName(pty); changed = true; }
      
      char* rtTime = rx.getRdsTime();
      if (rtTime != NULL && strlen(rtTime) > 0 && rdsTime != String(rtTime)) { rdsTime = String(rtTime); changed = true; }

      if (changed && useSerialMode) pushSerialTelemetry();
    }
  }

  if (useSerialMode && millis() - lastSerialPush > 1000) {
    lastSerialPush = millis();
    pushSerialTelemetry();
  }

  if (isScanning && millis() - lastScanStep >= 1000) { 
    lastScanStep = millis();
    if(currentMode == "FM") { currentFreq += 10; if (currentFreq > 10800) currentFreq = 8400; }
    else if(currentMode == "MW") { currentFreq += 10; if (currentFreq > 1710) currentFreq = 520; }
    else if(currentMode == "SW") { currentFreq += 5; if (currentFreq > 30000) currentFreq = 1711; }
    rx.setFrequency(currentFreq);
    clearRDS(); 
    delay(150); 
    rx.getCurrentReceivedSignalQuality();
    if (rx.getCurrentRSSI() >= 25 && rx.getCurrentSNR() >= 10) { 
      isScanning = false; 
      if (useSerialMode) pushSerialTelemetry(); 
    }
  }
}