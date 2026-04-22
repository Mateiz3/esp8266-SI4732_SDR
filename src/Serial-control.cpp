#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <SI4735.h>

#define RESET_PIN 2
#define SDA_PIN 4
#define SCL_PIN 5

SI4735 rx;

int currentFreq = 9610; 
String currentMode = "FM";
int currentVol = 30;

// RDS State
String rdsName = "";
String rdsText = "";
String rdsPty = "";
int currentPtyCode = -1;
unsigned long lastRdsPoll = 0;
unsigned long lastStatusReport = 0;

// Helper: Safely escape RDS text
String escapeJSON(String input) {
  input.replace("\\", "\\\\");
  input.replace("\"", "\\\"");
  input.replace("\n", "");
  input.replace("\r", "");
  return input;
}

// Helper: Translate PTY Code to Genre
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

// ==========================================
// Radio Control Helpers
// ==========================================
void switchToBand(String newBand, int freq) {
  currentMode = newBand;
  currentFreq = freq;
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

void reportStatus() {
  rx.getCurrentReceivedSignalQuality();
  int rssi = rx.getCurrentRSSI();
  int snr = rx.getCurrentSNR();
  
  float displayFreq = (currentMode == "FM") ? (currentFreq / 100.0) : (currentFreq / 1000.0);
  
  Serial.printf("{\"freq\":%.3f,\"mode\":\"%s\",\"vol\":%d,\"rssi\":%d,\"snr\":%d,\"rds_name\":\"%s\",\"rds_text\":\"%s\",\"rds_pty\":\"%s\"}\n", 
                displayFreq, currentMode.c_str(), currentVol, rssi, snr, 
                escapeJSON(rdsName).c_str(), escapeJSON(rdsText).c_str(), escapeJSON(rdsPty).c_str());
}

// ==========================================
// Setup
// ==========================================
void setup() {
  Serial.begin(115200);
  
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);

  Wire.begin(SDA_PIN, SCL_PIN);
  
  Serial.println("SYSTEM: Booting Si4732...");
  rx.setup(RESET_PIN, POWER_UP_FM);
  delay(500);
  
  switchToBand("FM", 9610);
  rx.setVolume(currentVol);

  Serial.println("SYSTEM: READY");
  reportStatus();
}

// ==========================================
// Loop - The Serial Command Parser & RDS
// ==========================================
void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();          
    cmd.toUpperCase();   

    if (cmd.length() == 0) return;

    if (cmd == "UP") {
      if(currentMode == "FM") currentFreq += 10;
      else if(currentMode == "MW") currentFreq += 10;
      else if(currentMode == "SW") currentFreq += 5;
      rx.setFrequency(currentFreq);
      reportStatus();
    } 
    else if (cmd == "DOWN") {
      if(currentMode == "FM") currentFreq -= 10;
      else if(currentMode == "MW") currentFreq -= 10;
      else if(currentMode == "SW") currentFreq -= 5;
      rx.setFrequency(currentFreq);
      reportStatus();
    }
    else if (cmd.startsWith("VOL=")) {
      int newVol = cmd.substring(4).toInt(); 
      if (newVol >= 0 && newVol <= 63) {
        currentVol = newVol;
        rx.setVolume(currentVol);
      }
      reportStatus();
    }
    else if (cmd.startsWith("TUNE=")) {
      float inputFreq = cmd.substring(5).toFloat();
      if (inputFreq >= 64.0 && inputFreq <= 108.0) switchToBand("FM", inputFreq * 100);
      else if (inputFreq >= 0.520 && inputFreq <= 1.710) switchToBand("MW", inputFreq * 1000);
      else if (inputFreq >= 1.711 && inputFreq <= 30.0) switchToBand("SW", inputFreq * 1000);
      reportStatus();
    }
    else if (cmd.startsWith("BAND=")) {
      String newBand = cmd.substring(5);
      if (newBand == "FM") switchToBand("FM", 9610);
      else if (newBand == "MW") switchToBand("MW", 810);
      else if (newBand == "SW") switchToBand("SW", 9600);
      reportStatus();
    }
    else if (cmd == "STATUS") {
      reportStatus(); 
    }
  }

  // Background RDS Polling
  if (currentMode == "FM" && (millis() - lastRdsPoll > 40)) {
    lastRdsPoll = millis();
    if (rx.getRdsReady()) {
      bool changed = false;
      
      char* ps = rx.getRdsStationName();
      if (ps != NULL && strlen(ps) > 0) {
        if (rdsName != String(ps)) { rdsName = String(ps); changed = true; }
      }
      
      char* pt = rx.getRdsProgramInformation();
      if (pt != NULL && strlen(pt) > 0) {
        if (rdsText != String(pt)) { rdsText = String(pt); changed = true; }
      }

      int pty = rx.getRdsProgramType();
      if (pty != currentPtyCode && pty > 0) {
        currentPtyCode = pty;
        rdsPty = getPtyName(pty);
        changed = true;
      }

      if (changed) reportStatus();
    }
  }

  // Background Telemetry Push
  if (millis() - lastStatusReport > 1000) {
    lastStatusReport = millis();
    reportStatus();
  }
}