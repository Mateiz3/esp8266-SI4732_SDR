#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <SI4735.h>

// --- THE SSB PATCH ---
#include "patch_init.h" 
const uint16_t size_content = sizeof ssb_patch_content;

#define RESET_PIN 2
#define SDA_PIN 4
#define SCL_PIN 5

SI4735 rx;
AsyncWebServer server(80);

const char* ssid = "Si4732_Radio_Hotspot";
const char* password = "password123";

bool useSerialMode = false;
bool ssbLoaded = false;

// --- Radio & Telemetry State ---
int currentFreq = 9610; 
String currentMode = "FM";   
String currentMod = "FM";    
int currentVol = 30;
int currentBFO = 0; 
bool agcEnabled = true;
int attenuationIdx = 0; // 0 = Max Gain, 37 = Max Attenuation

String rdsName = ""; String rdsText = ""; String rdsPty = ""; String rdsTime = "";
String draftRdsName = ""; unsigned long nameStableTimer = 0;
String draftRdsText = ""; unsigned long textStableTimer = 0;
int currentPtyCode = -1;
unsigned long lastRdsPoll = 0;
unsigned long lastSerialPush = 0;

bool isScanning = false;
unsigned long lastScanStep = 0;

// ==========================================
// Helpers
// ==========================================
String escapeJSON(String input) {
  input.replace("\\", "\\\\"); input.replace("\"", "\\\"");
  input.replace("\n", ""); input.replace("\r", "");
  return input;
}

String getPtyName(int pty) {
  const char* ptyNames[] = {
    "None", "News", "Info", "Sports", "Talk", "Rock", "Classic Rock", "Adult Hits", "Soft Rock", "Top 40", "Country", "Oldies", "Soft", "Nostalgia", "Jazz", "Classical", "R&B", "Soft R&B", "Language", "Religious", "Religious Talk", "Personality", "Public", "College", "Unassigned", "Unassigned", "Unassigned", "Unassigned", "Unassigned", "Weather", "Emergency Test", "Emergency!"
  };
  if (pty >= 0 && pty <= 31) return String(ptyNames[pty]);
  return "";
}

void clearRDS() { 
  rdsName = ""; rdsText = ""; rdsPty = ""; rdsTime = ""; currentPtyCode = -1; 
  draftRdsName = ""; draftRdsText = "";
}

void applyFrequency() {
  clearRDS();
  isScanning = false;

  if (currentMode == "FM") {
    ssbLoaded = false; 
    rx.setFM(6400, 10800, currentFreq, 10);
    rx.setRdsConfig(1, 2, 2, 2, 2); 
  } 
  else {
    if (currentMod == "AM") {
      ssbLoaded = false;
      if (currentMode == "MW") rx.setAM(153, 1710, currentFreq, 9);
      else rx.setAM(1711, 30000, currentFreq, 5);
    } 
    else if (currentMod == "LSB" || currentMod == "USB" || currentMod == "SAM-L" || currentMod == "SAM-U" || currentMod == "CW") {
      if (!ssbLoaded) {
        if (useSerialMode) Serial.println("SYSTEM: Loading SSB Patch... Please wait.");
        rx.loadPatch(ssb_patch_content, size_content); 
        ssbLoaded = true;
      }
      
      // 1. Set the Sideband (CW defaults to USB processing)
      int ssbType = (currentMod == "USB" || currentMod == "SAM-U" || currentMod == "CW") ? 2 : 1;
      if (currentMode == "MW") rx.setSSB(153, 1710, currentFreq, 1, ssbType);
      else rx.setSSB(1711, 30000, currentFreq, 1, ssbType);
      
      // 2. Set the proper Audio Filter Bandwidth
      if (currentMod.startsWith("SAM")) {
        rx.setSSBAudioBandwidth(3); // 4.0 kHz (Wide for Music/AM)
      } else if (currentMod == "CW") {
        rx.setSSBAudioBandwidth(4); // 0.5 kHz (Ultra-Narrow for Morse Code!)
      } else {
        rx.setSSBAudioBandwidth(1); // 2.2 kHz (Standard for SSB Voice)
      }
      
      // 3. Apply the BFO
      rx.setSSBBfo(currentBFO);
    }
  }
  // Apply AGC & RF Gain Settings
  rx.setAutomaticGainControl(agcEnabled ? 0 : 1, attenuationIdx);
}

void tuneUp() {
  if (currentMode == "FM") {
    currentFreq += 10;
    if (currentFreq > 10800) { currentMode = "MW"; currentMod = "AM"; currentFreq = 153; }
  } else if (currentMode == "MW") {
    currentFreq += 9;
    if (currentFreq > 1710) { currentMode = "SW"; currentMod = "AM"; currentFreq = 1711; }
  } else {
    // SW Band: 1 kHz steps for SSB/CW, 5 kHz for AM
    currentFreq += (currentMod == "LSB" || currentMod == "USB" || currentMod == "CW") ? 1 : 5;
    if (currentFreq > 30000) { currentMode = "FM"; currentMod = "FM"; currentFreq = 6400; }
  }
  
  applyFrequency();
}

void tuneDown() {
  if (currentMode == "FM") {
    currentFreq -= 10;
    if (currentFreq < 6400) { currentMode = "SW"; currentMod = "AM"; currentFreq = 30000; }
  } else if (currentMode == "MW") {
    currentFreq -= 9;
    if (currentFreq < 153) { currentMode = "FM"; currentMod = "FM"; currentFreq = 10800; }
  } else {
    // SW Band: 1 kHz steps for SSB/CW, 5 kHz for AM
    currentFreq -= (currentMod == "LSB" || currentMod == "USB" || currentMod == "CW") ? 1 : 5;
    if (currentFreq < 1711) { currentMode = "MW"; currentMod = "AM"; currentFreq = 1710; }
  }
  
  applyFrequency();
}
void tuneDirect(float iF) {
  if (iF >= 153.0 && iF <= 30000.0) { 
    if (iF <= 1710.0) { currentMode = "MW"; currentMod = "AM"; currentFreq = iF; }
    else { currentMode = "SW"; currentMod = "AM"; currentFreq = iF; }
  } else if (iF >= 64.0 && iF <= 108.0) { 
    currentMode = "FM"; currentMod = "FM"; currentFreq = iF * 100;
  } else if (iF >= 0.153 && iF <= 30.0) { 
    if (iF <= 1.710) { currentMode = "MW"; currentMod = "AM"; currentFreq = iF * 1000; }
    else { currentMode = "SW"; currentMod = "AM"; currentFreq = iF * 1000; }
  }
  applyFrequency();
}

void pushSerialTelemetry() {
  if (!useSerialMode) return;
  rx.getCurrentReceivedSignalQuality();
  float displayFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (float)currentFreq;
  String unit = (currentMode == "FM") ? "MHz" : "kHz";
  bool stereo = (currentMode == "FM") ? rx.getCurrentPilot() : false;
  
  Serial.printf("{\"freq\":%.3f,\"unit\":\"%s\",\"mode\":\"%s\",\"mod\":\"%s\",\"vol\":%d,\"bfo\":%d,\"agc\":%s,\"att\":%d,\"rssi\":%d,\"snr\":%d,\"stereo\":%s,\"rds_name\":\"%s\",\"rds_text\":\"%s\",\"rds_pty\":\"%s\",\"rds_time\":\"%s\"}\n", 
                displayFreq, unit.c_str(), currentMode.c_str(), currentMod.c_str(), currentVol, currentBFO, 
                agcEnabled ? "true" : "false", attenuationIdx,
                rx.getCurrentRSSI(), rx.getCurrentSNR(), 
                stereo ? "true" : "false",
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
    .freq-text { font-size: 2.5rem; font-weight: bold; color: #00ffcc; }
    .rds-box { color: #ffeb3b; font-weight: bold; min-height: 24px;}
    .pty-badge { background: #e91e63; color: white; padding: 2px 8px; border-radius: 12px; font-size: 0.8rem; display: none; }
    .btn { background: #444; color: white; padding: 12px; margin: 4px; border-radius: 8px; cursor: pointer; border: none;}
    .panel { background: #222; padding: 15px; border-radius: 10px; margin-top: 15px;}
    select { padding:10px; background:#333; color:#fff; border:none; margin: 2px; }
  </style>
</head>
<body>
  <h1>Portable SDR</h1>
  <div class="display">
    <div class="freq-text" id="freq">--.-- MHz</div>
    <div>
      <span id="mode">--</span> 
      <span id="mod_display" style="color:#aaa; font-size: 0.9em;"></span>
      <span id="stereo_badge" style="background:#008CBA; color: white; padding: 2px 8px; border-radius: 12px; font-size: 0.8rem; display: none;"></span>
      <span id="rds_pty" class="pty-badge"></span>
    </div>
    <div id="rds_time" style="color:#888; font-size:0.8rem; margin-top:3px; min-height:15px;"></div>
    <div class="rds-box" id="rds_name"></div>
    <div id="rds_text" style="color:#bbb; font-size:0.9rem;"></div>
  </div>
  <div>
    <select id="band_sel" onchange="changeBand()">
      <option value="FM">FM Band</option><option value="MW">MW Band</option><option value="SW">SW Band</option>
    </select>
    <select id="mod_sel" onchange="changeMod()">
      <option value="FM">FM</option><option value="AM">AM</option><option value="LSB">LSB</option><option value="USB">USB</option><option value="SAM-L">SAM-L</option><option value="SAM-U">SAM-U</option>
    </select>
    <input type="number" id="freqInput" placeholder="Freq" style="padding:10px; width:80px;">
    <button class="btn" style="background:#008CBA;" onclick="sendTune()">GO</button>
  </div>
  <button class="btn" onclick="sendCommand('down')"><< DOWN</button>
  <button class="btn" onclick="sendCommand('up')">UP >></button>
  <button class="btn" id="scanBtn" style="background:#9c27b0; width:100%; margin-top:10px;" onclick="sendCommand('scan')">START AUTO-SCAN</button>
  
  <div class="panel">
    <h3 style="margin-top:0;">Telemetry & Volume</h3>
    <div style="display:flex; justify-content:center; gap: 10px;">
      <canvas id="rssi_gauge" width="130" height="80"></canvas>
      <canvas id="snr_gauge" width="130" height="80"></canvas>
    </div>
    <input type="range" id="vol" min="0" max="63" style="width:100%; margin-top:15px;" onchange="setVol(this.value)">
  </div>
<script>
let dragging = false;
document.getElementById("vol").onmousedown = () => dragging = true;
document.getElementById("vol").onmouseup = () => dragging = false;

function drawGauge(id, title, val, maxVal, unit) {
  let c = document.getElementById(id); let ctx = c.getContext("2d");
  ctx.clearRect(0, 0, c.width, c.height);
  let cx = c.width/2, cy = c.height - 15, r = 40;
  ctx.beginPath(); ctx.arc(cx, cy, r, Math.PI, 0);
  ctx.lineWidth = 12; ctx.strokeStyle = "#333"; ctx.stroke();
  let pct = Math.min(Math.max(val/maxVal, 0), 1);
  ctx.beginPath(); ctx.arc(cx, cy, r, Math.PI, Math.PI + (pct * Math.PI));
  ctx.strokeStyle = "#00ffcc"; ctx.stroke();
  ctx.fillStyle = "#fff"; ctx.font = "bold 13px Arial"; ctx.textAlign = "center";
  ctx.fillText(val + " " + unit, cx, cy - 10);
  ctx.fillStyle = "#888"; ctx.font = "bold 11px Arial"; ctx.fillText(title, cx, cy + 18);
}

function fetchMetrics() {
  fetch('/metrics').then(r => r.json()).then(data => {
    let fStr = data.unit === "MHz" ? data.freq.toFixed(3) : data.freq.toFixed(0);
    document.getElementById("freq").innerText = fStr + " " + data.unit;
    document.getElementById("mode").innerText = data.mode;
    document.getElementById("mod_display").innerText = (data.mode === "FM") ? "" : "[" + data.mod + "]";
    
    document.getElementById("band_sel").value = data.mode;
    let modSel = document.getElementById("mod_sel");
    let stBadge = document.getElementById("stereo_badge");
    
    if(data.mode === "FM") { 
      modSel.value = "FM"; modSel.disabled = true; 
      stBadge.innerText = data.stereo ? "STEREO" : "MONO";
      stBadge.style.background = data.stereo ? "#008CBA" : "#555";
      stBadge.style.display = "inline-block";
    } else { 
      modSel.disabled = false; stBadge.style.display = "none";
      if (modSel.value === "FM") modSel.value = "AM"; else modSel.value = data.mod; 
    }

    document.getElementById("rds_name").innerText = data.rds_name;
    document.getElementById("rds_text").innerText = data.rds_text;
    document.getElementById("rds_time").innerText = data.rds_time;
    
    let pty = document.getElementById("rds_pty");
    if(data.rds_pty && data.rds_pty !== "None") { pty.innerText = data.rds_pty; pty.style.display="inline-block"; }
    else pty.style.display="none";

    drawGauge("rssi_gauge", "RSSI", data.rssi, 80, "dBuV");
    drawGauge("snr_gauge", "SNR", data.snr, 40, "dB");
    
    if(!dragging) document.getElementById("vol").value = data.vol;
    let sBtn = document.getElementById("scanBtn");
    sBtn.innerText = data.scanning ? "STOP SCANNING" : "START AUTO-SCAN";
    sBtn.style.background = data.scanning ? "#e91e63" : "#9c27b0";
  });
}
function sendCommand(a) { fetch('/action?cmd=' + a).then(fetchMetrics); }
function sendTune() { let f = document.getElementById("freqInput").value; if(f){ fetch('/tune?val='+f).then(fetchMetrics); document.getElementById("freqInput").value='';} }
function changeBand() { fetch('/action?cmd=band&val=' + document.getElementById("band_sel").value).then(fetchMetrics); }
function changeMod() { fetch('/action?cmd=mod&val=' + document.getElementById("mod_sel").value).then(fetchMetrics); }
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
  applyFrequency();
  rx.setVolume(currentVol);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });
  server.on("/tune", HTTP_GET, [](AsyncWebServerRequest *r){ if (r->hasParam("val")) tuneDirect(r->getParam("val")->value().toFloat()); r->send(200, "text/plain", "OK"); });
  server.on("/action", HTTP_GET, [](AsyncWebServerRequest *r){
    if (r->hasParam("cmd")) {
      String cmd = r->getParam("cmd")->value();
      if (cmd == "up") tuneUp(); else if (cmd == "down") tuneDown();
      else if (cmd == "band" && r->hasParam("val")) {
        String val = r->getParam("val")->value();
        if (val == "FM") { currentMode = "FM"; currentMod = "FM"; currentFreq = 9610; }
        else if (val == "MW") { currentMode = "MW"; currentMod = "AM"; currentFreq = 810; }
        else { currentMode = "SW"; currentMod = "AM"; currentFreq = 9600; }
        applyFrequency();
      }
      else if (cmd == "mod" && r->hasParam("val")) {
        String val = r->getParam("val")->value();
        if (currentMode != "FM" && (val == "AM" || val == "LSB" || val == "USB" || val == "SAM-L" || val == "SAM-U")) {
            currentMod = val; applyFrequency();
        }
      }
      else if (cmd == "scan") { isScanning = !isScanning; clearRDS(); }
    }
    r->send(200, "text/plain", "OK");
  });
  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *r){ if (r->hasParam("val")) { currentVol = r->getParam("val")->value().toInt(); rx.setVolume(currentVol); } r->send(200, "text/plain", "OK"); });
  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *r){
    rx.getCurrentReceivedSignalQuality();
    float dispFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (float)currentFreq;
    String unit = (currentMode == "FM") ? "MHz" : "kHz";
    bool stereo = (currentMode == "FM") ? rx.getCurrentPilot() : false;
    String json = "{\"rssi\":" + String(rx.getCurrentRSSI()) + ",\"snr\":" + String(rx.getCurrentSNR()) + ",\"stereo\":" + String(stereo ? "true" : "false") + ",\"freq\":" + String(dispFreq, 3) + ",\"unit\":\"" + unit + "\",\"mode\":\"" + currentMode + "\",\"mod\":\"" + currentMod + "\",\"vol\":" + String(currentVol) + ",\"scanning\":" + String(isScanning ? "true" : "false") + ",\"rds_name\":\"" + escapeJSON(rdsName) + "\",\"rds_text\":\"" + escapeJSON(rdsText) + "\",\"rds_pty\":\"" + escapeJSON(rdsPty) + "\",\"rds_time\":\"" + escapeJSON(rdsTime) + "\"}";
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

    if (!useSerialMode) { useSerialMode = true; WiFi.mode(WIFI_OFF); WiFi.forceSleepBegin(); }

    if (cmd == "UP") { tuneUp(); pushSerialTelemetry(); } 
    else if (cmd == "DOWN") { tuneDown(); pushSerialTelemetry(); }
    else if (cmd.startsWith("TUNE=")) { tuneDirect(cmd.substring(5).toFloat()); pushSerialTelemetry(); }
    else if (cmd.startsWith("BAND=")) {
      String b = cmd.substring(5);
      if (b == "FM") { currentMode = "FM"; currentMod = "FM"; currentFreq = 9610; }
      else if (b == "MW") { currentMode = "MW"; currentMod = "AM"; currentFreq = 810; }
      else { currentMode = "SW"; currentMod = "AM"; currentFreq = 9600; }
      applyFrequency(); pushSerialTelemetry();
    }
    else if (cmd.startsWith("MOD=")) {
      String m = cmd.substring(4);
      // --- CW added to MOD list ---
      if (currentMode != "FM" && (m == "AM" || m == "LSB" || m == "USB" || m == "SAM-L" || m == "SAM-U" || m == "CW")) {
        currentMod = m; currentBFO = 0; applyFrequency(); pushSerialTelemetry();
      }
    }
    else if (cmd.startsWith("VOL=")) { currentVol = cmd.substring(4).toInt(); rx.setVolume(currentVol); pushSerialTelemetry(); }
    else if (cmd.startsWith("BFO=")) { 
      // --- CW added to BFO list ---
      if(currentMod == "LSB" || currentMod == "USB" || currentMod == "SAM-L" || currentMod == "SAM-U" || currentMod == "CW") {
        currentBFO = cmd.substring(4).toInt(); rx.setSSBBfo(currentBFO); pushSerialTelemetry(); 
      }
    }
    else if (cmd.startsWith("AGC=")) { 
      agcEnabled = (cmd.substring(4) == "ON"); 
      rx.setAutomaticGainControl(agcEnabled ? 0 : 1, attenuationIdx);
      pushSerialTelemetry();
    }
    else if (cmd.startsWith("ATT=")) { 
      attenuationIdx = cmd.substring(4).toInt(); 
      if (agcEnabled) { agcEnabled = false; } 
      rx.setAutomaticGainControl(1, attenuationIdx);
      pushSerialTelemetry();
    }
    else if (cmd == "STATUS") { pushSerialTelemetry(); }
  }

  // Background RDS with Stabilizer Filters
  if (!isScanning && currentMode == "FM" && (millis() - lastRdsPoll > 40)) {
    lastRdsPoll = millis();
    rx.getRdsStatus();
    if (rx.getRdsReceived()) {
      bool changed = false;
      
      char* ps = rx.getRdsStationName(); 
      if (ps != NULL && strlen(ps) > 0) { 
        String newName = String(ps);
        if (newName != draftRdsName) { draftRdsName = newName; nameStableTimer = millis(); } 
        else if (millis() - nameStableTimer > 800) { if (rdsName != draftRdsName) { rdsName = draftRdsName; changed = true; } }
      }
      
      char* pt = rx.getRdsProgramInformation(); 
      if (pt != NULL && strlen(pt) > 0) { 
        String newText = String(pt);
        if (newText != draftRdsText) { draftRdsText = newText; textStableTimer = millis(); } 
        else if (millis() - textStableTimer > 1500) { if (rdsText != draftRdsText) { rdsText = draftRdsText; changed = true; } }
      }

      int pty = rx.getRdsProgramType(); 
      if (pty != currentPtyCode && pty > 0) { currentPtyCode = pty; rdsPty = getPtyName(pty); changed = true; }
      
      char* rtTime = rx.getRdsDateTime(); 
      if (rtTime != NULL && strlen(rtTime) > 0) {
        String newTime = String(rtTime);
        if (rdsTime != newTime) {
          bool isGlitch = (newTime.indexOf(" 00:00") != -1) && (rdsTime.length() > 0) && (rdsTime.indexOf(" 23:") == -1);
          if (!isGlitch) { rdsTime = newTime; changed = true; }
        }
      }
      if (changed && useSerialMode) pushSerialTelemetry();
    }
  }

  if (useSerialMode && millis() - lastSerialPush > 1000) { lastSerialPush = millis(); pushSerialTelemetry(); }

  if (isScanning && millis() - lastScanStep >= 1000) { 
    lastScanStep = millis();
    if(currentMode == "FM") { currentFreq += 10; if (currentFreq > 10800) currentFreq = 6400; }
    else if(currentMode == "MW") { currentFreq += 9; if (currentFreq > 1710) currentFreq = 153; }
    else if(currentMode == "SW") { currentFreq += 5; if (currentFreq > 30000) currentFreq = 1711; }
    rx.setFrequency(currentFreq);
    clearRDS(); delay(150); 
    rx.getCurrentReceivedSignalQuality();
    if (rx.getCurrentRSSI() >= 25 && rx.getCurrentSNR() >= 10) { isScanning = false; if (useSerialMode) pushSerialTelemetry(); }
  }
}