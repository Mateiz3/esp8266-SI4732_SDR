#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <SI4735.h>

// ==========================================
// Hardware Pins & Global Variables
// ==========================================
#define RESET_PIN 2  // D4
#define SDA_PIN 4    // D2
#define SCL_PIN 5    // D1

SI4735 rx;
AsyncWebServer server(80);

const char* ssid = "Si4732_Radio_Hotspot";
const char* password = "password123";

// Radio State
int currentFreq = 9610; 
String currentMode = "FM";
int currentVol = 30;

// RDS State
String rdsName = "";
String rdsText = "";
String rdsPty = "";
int currentPtyCode = -1;
unsigned long lastRdsPoll = 0;

// Scanning State
bool isScanning = false;
unsigned long lastScanStep = 0;

// History Array
struct Station {
  float freq;
  String mode;
  int rssi;
  int snr;
};
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
    "None", "News", "Information", "Sports", "Talk", "Rock", "Classic Rock", 
    "Adult Hits", "Soft Rock", "Top 40", "Country", "Oldies", "Soft", 
    "Nostalgia", "Jazz", "Classical", "R&B", "Soft R&B", "Language", 
    "Religious Music", "Religious Talk", "Personality", "Public", "College", 
    "Unassigned", "Unassigned", "Unassigned", "Unassigned", "Unassigned", 
    "Weather", "Emergency Test", "Emergency!"
  };
  if (pty >= 0 && pty <= 31) return String(ptyNames[pty]);
  return "";
}

void switchToBand(String newBand, int freq) {
  currentMode = newBand;
  currentFreq = freq;
  isScanning = false; 
  rdsName = "";
  rdsText = "";
  rdsPty = "";
  currentPtyCode = -1;
  
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

// ==========================================
// HTML & CSS (The Webpage)
// ==========================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>ESP8266 SDR Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; background-color: #121212; color: #ffffff; padding: 15px;}
    h1 { color: #00ffcc; margin-bottom: 5px; font-size: 1.5rem; }
    .display { background: #222; padding: 15px; border-radius: 10px; border: 2px solid #00ffcc; display: inline-block; min-width: 260px; margin-bottom: 15px;}
    .freq-text { font-size: 3rem; font-family: monospace; font-weight: bold; color: #00ffcc; }
    .mode-container { display: flex; justify-content: center; align-items: center; gap: 10px; margin-bottom: 5px; }
    .rds-box { font-size: 1.2rem; color: #ffeb3b; min-height: 28px; font-weight: bold; }
    .rds-text { font-size: 0.9rem; color: #bbb; min-height: 20px; font-weight: normal; max-width: 260px; margin: 0 auto;}
    .pty-badge { background: #e91e63; color: white; padding: 2px 8px; border-radius: 12px; font-size: 0.8rem; font-weight: bold; display: none; }
    
    .btn { background-color: #444; border: none; color: white; padding: 12px 15px; font-size: 14px; margin: 4px 2px; cursor: pointer; border-radius: 8px; width: 110px;}
    .btn:active { background-color: #00ffcc; color: black; }
    .btn-scan { background-color: #9c27b0; width: 230px; font-weight: bold; margin-top: 10px;}
    .btn-scan.active { background-color: #e91e63; animation: pulse 1.5s infinite; }
    .btn-go { background-color: #008CBA; width: auto; padding: 10px 15px;}
    
    .input-row { display: flex; justify-content: center; align-items: center; gap: 5px; margin-bottom: 15px; }
    select, input[type=number] { padding: 8px; font-size: 16px; border-radius: 5px; border: none; background: #333; color: white;}
    input[type=number] { width: 90px; text-align: center; }
    input[type=range] { width: 100%; margin: 10px 0; }
    
    .panel { margin-top: 15px; background: #222; padding: 15px; border-radius: 10px; display: inline-block; text-align: left; width: 100%; max-width: 320px; box-sizing: border-box; vertical-align: top;}
    .meter-label { display: flex; justify-content: space-between; font-size: 12px; color: #bbb; margin-top: 8px; margin-bottom: 2px;}
    meter { width: 100%; height: 15px; border-radius: 5px; }
    
    .history-list { max-height: 150px; overflow-y: auto; margin-top: 10px; border-top: 1px solid #444; padding-top: 10px;}
    .hist-item { background: #333; margin-bottom: 5px; padding: 8px; border-radius: 5px; cursor: pointer; display: flex; justify-content: space-between; font-size: 14px;}
    .hist-item:hover { background: #00ffcc; color: black; }
    
    @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.7; } 100% { opacity: 1; } }
  </style>
</head>
<body>
  <h1>Portable SDR</h1>
  
  <div class="display">
    <div class="freq-text" id="freq">--.--</div>
    <div class="mode-container">
      <span id="mode" style="font-size: 1.2rem; color: #888;">--</span>
      <span id="rds_pty" class="pty-badge"></span>
    </div>
    <div class="rds-box" id="rds_name"></div>
    <div class="rds-text" id="rds_text"></div>
  </div>

  <div class="input-row">
    <select id="band_sel" onchange="changeBand()">
      <option value="FM">FM</option>
      <option value="MW">MW</option>
      <option value="SW">SW</option>
    </select>
    <input type="number" id="freqInput" step="0.001" placeholder="MHz">
    <button class="btn btn-go" onclick="tuneDirect()">TUNE</button>
  </div>

  <div>
    <button class="btn" onclick="sendCommand('down')"><< DOWN</button>
    <button class="btn" onclick="sendCommand('up')">UP >></button>
  </div>
  <button class="btn btn-scan" id="scanBtn" onclick="sendCommand('scan')">START AUTO-SCAN</button>
  
  <br>
  <div class="panel">
    <h3 style="margin-top:0; text-align:center; border-bottom: 1px solid #444; padding-bottom: 5px;">Telemetry & Audio</h3>
    
    <div class="meter-label"><span>RSSI</span> <span id="rssi_val">0 dBuV</span></div>
    <meter id="rssi_meter" min="0" max="80" low="20" high="45" optimum="60" value="0"></meter>

    <div class="meter-label"><span>SNR</span> <span id="snr_val">0 dB</span></div>
    <meter id="snr_meter" min="0" max="40" low="10" high="25" optimum="35" value="0"></meter>

    <div class="meter-label" style="margin-top: 15px;"><span>Volume</span></div>
    <input type="range" id="vol" min="0" max="63" value="30" onchange="setVol(this.value)">
  </div>

  <div class="panel">
    <h3 style="margin-top:0; text-align:center;">Scan History</h3>
    <div class="history-list" id="historyBox">
      <div style="text-align:center; color:#777; font-size:12px;">No stations found yet.</div>
    </div>
  </div>

<script>
let volDragging = false;
document.getElementById("vol").addEventListener("mousedown", () => volDragging = true);
document.getElementById("vol").addEventListener("mouseup", () => volDragging = false);

function fetchMetrics() {
  fetch('/metrics')
    .then(response => response.json())
    .then(data => {
      document.getElementById("freq").innerText = data.freq.toFixed(3);
      document.getElementById("mode").innerText = data.mode;
      
      // Update Band Dropdown silently if it changed
      let bandSel = document.getElementById("band_sel");
      if(bandSel.value !== data.mode) bandSel.value = data.mode;

      // Update RDS
      document.getElementById("rds_name").innerText = data.rds_name;
      document.getElementById("rds_text").innerText = data.rds_text;
      
      let ptyBadge = document.getElementById("rds_pty");
      if(data.rds_pty && data.rds_pty !== "None") {
        ptyBadge.innerText = data.rds_pty;
        ptyBadge.style.display = "inline-block";
      } else {
        ptyBadge.style.display = "none";
      }

      // Update Meters
      document.getElementById("rssi_meter").value = data.rssi;
      document.getElementById("rssi_val").innerText = data.rssi + " dBuV";
      document.getElementById("snr_meter").value = data.snr;
      document.getElementById("snr_val").innerText = data.snr + " dB";
      
      if (!volDragging) document.getElementById("vol").value = data.vol;

      // Update Scan Button
      let scanBtn = document.getElementById("scanBtn");
      if(data.scanning) {
        scanBtn.innerText = "STOP SCANNING";
        scanBtn.classList.add("active");
      } else {
        scanBtn.innerText = "START AUTO-SCAN";
        scanBtn.classList.remove("active");
      }

      // Update History
      if(data.history.length > 0) {
        let html = "";
        data.history.forEach(h => {
           html += `<div class="hist-item" onclick="tuneDirectVal(${h.f})">
                      <span><strong>${h.f.toFixed(3)}</strong> ${h.m}</span>
                      <span style="color:#aaa;">S:${h.s} R:${h.r}</span>
                    </div>`;
        });
        document.getElementById("historyBox").innerHTML = html;
      }
    })
    .catch(err => console.log(err));
}

function sendCommand(action) { fetch('/action?cmd=' + action).then(() => fetchMetrics()); }
function tuneDirect() {
  let f = document.getElementById("freqInput").value;
  if(f) tuneDirectVal(f);
}
function tuneDirectVal(f) {
  fetch('/tune?val=' + f).then(() => {
    document.getElementById("freqInput").value = ''; 
    fetchMetrics(); 
  });
}
function changeBand() {
  let b = document.getElementById("band_sel").value;
  fetch('/action?cmd=band&val=' + b).then(() => fetchMetrics());
}
function setVol(v) { fetch("/volume?val=" + v); }

window.onload = fetchMetrics;
setInterval(fetchMetrics, 1000);
</script>
</body>
</html>)rawliteral";

// ==========================================
// Setup and Loop
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  Serial.println("\nInitializing Si4732...");
  rx.setup(RESET_PIN, POWER_UP_FM);
  delay(500);
  
  switchToBand("FM", 9610);
  rx.setVolume(currentVol);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/tune", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("val")) {
      float val = request->getParam("val")->value().toFloat();
      if (val >= 64.0 && val <= 108.0) switchToBand("FM", val * 100);
      else if (val >= 0.520 && val <= 1.710) switchToBand("MW", val * 1000);
      else if (val >= 1.711 && val <= 30.0) switchToBand("SW", val * 1000);
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/action", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("cmd")) {
      String cmd = request->getParam("cmd")->value();
      
      if (cmd == "up") { 
        if(currentMode == "FM") currentFreq += 10;
        else if(currentMode == "MW") currentFreq += 10;
        else if(currentMode == "SW") currentFreq += 5;
        rx.setFrequency(currentFreq);
        isScanning = false; rdsName = ""; rdsText = ""; rdsPty = "";
      } 
      else if (cmd == "down") { 
        if(currentMode == "FM") currentFreq -= 10;
        else if(currentMode == "MW") currentFreq -= 10;
        else if(currentMode == "SW") currentFreq -= 5;
        rx.setFrequency(currentFreq);
        isScanning = false; rdsName = ""; rdsText = ""; rdsPty = "";
      } 
      else if (cmd == "band") {
        if (request->hasParam("val")) {
          String val = request->getParam("val")->value();
          if (val == "FM") switchToBand("FM", 9610);
          else if (val == "MW") switchToBand("MW", 810);
          else if (val == "SW") switchToBand("SW", 9600);
        }
      }
      else if (cmd == "scan") {
        isScanning = !isScanning; 
        rdsName = ""; rdsText = ""; rdsPty = "";
      }
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("val")) {
      currentVol = request->getParam("val")->value().toInt();
      rx.setVolume(currentVol);
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request){
    rx.getCurrentReceivedSignalQuality();
    int rssi = rx.getCurrentRSSI();
    int snr = rx.getCurrentSNR();
    
    int actualFreq = rx.getFrequency();
    float displayFreq = (currentMode == "FM") ? (actualFreq / 100.0) : (actualFreq / 1000.0);
    
    String json = "{";
    json += "\"rssi\":" + String(rssi) + ",";
    json += "\"snr\":" + String(snr) + ",";
    json += "\"freq\":" + String(displayFreq, 3) + ",";
    json += "\"mode\":\"" + currentMode + "\",";
    json += "\"vol\":" + String(currentVol) + ",";
    json += "\"scanning\":" + String(isScanning ? "true" : "false") + ",";
    json += "\"rds_name\":\"" + escapeJSON(rdsName) + "\",";
    json += "\"rds_text\":\"" + escapeJSON(rdsText) + "\",";
    json += "\"rds_pty\":\"" + escapeJSON(rdsPty) + "\",";
    
    json += "\"history\":[";
    for(int i=0; i<historyCount; i++) {
      json += "{\"f\":" + String(history[i].freq, 3) + ",\"m\":\"" + history[i].mode + "\",\"r\":" + String(history[i].rssi) + ",\"s\":" + String(history[i].snr) + "}";
      if(i < historyCount - 1) json += ",";
    }
    json += "]}";
    
    request->send(200, "application/json", json);
  });

  server.begin();
}

void loop() {
  // --- Check for RDS Data (Only if in FM and NOT scanning) ---
  if (!isScanning && currentMode == "FM" && (millis() - lastRdsPoll > 40)) {
    lastRdsPoll = millis();
    rx.getRdsStatus();
    if (rx.getRdsReceived() && rx.getRdsSync() && rx.getRdsSyncFound() && !rx.getRdsSyncLost()) {
      char* ps = rx.getRdsStationName();
      if (ps != NULL && strlen(ps) > 0) rdsName = String(ps);
      
      char* pt = rx.getRdsProgramInformation();
      if (pt != NULL && strlen(pt) > 0) rdsText = String(pt);

      int pty = rx.getRdsProgramType();
      if (pty != currentPtyCode && pty > 0) {
        currentPtyCode = pty;
        rdsPty = getPtyName(pty);
      }
    }
  }

  // --- NON-BLOCKING SCAN STATE MACHINE ---
  if (isScanning) {
    if (millis() - lastScanStep >= 1000) { 
      lastScanStep = millis();

      if(currentMode == "FM") {
        currentFreq += 10;
        if (currentFreq > 10800) currentFreq = 8400; 
      }
      else if(currentMode == "MW") {
        currentFreq += 10;
        if (currentFreq > 1710) currentFreq = 520;
      }
      else if(currentMode == "SW") {
        currentFreq += 5;
        if (currentFreq > 30000) currentFreq = 1711;
      }

      rx.setFrequency(currentFreq);
      
      delay(150); 
      
      rx.getCurrentReceivedSignalQuality();
      int r = rx.getCurrentRSSI();
      int s = rx.getCurrentSNR();

      if (r >= 25 && s >= 10) { 
        float dispFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (currentFreq / 1000.0);
        
        bool exists = false;
        for(int i=0; i<historyCount; i++) {
          if(abs(history[i].freq - dispFreq) < 0.05) exists = true;
        }

        if(!exists) {
          if(historyCount < 15) {
            history[historyCount] = {dispFreq, currentMode, r, s};
            historyCount++;
          } else {
            for(int i=1; i<15; i++) history[i-1] = history[i];
            history[14] = {dispFreq, currentMode, r, s};
          }
        }
      }
    }
  }
}