/*
 Sharkee_Haptics_Blended_RTP_Gamma.ino
 Waveform/Realtime blended variant: Click at Low, RTP Buzz at High.

 - Contains fixes for 'isBusy' and 'setMotorType' compilation errors.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <ArduinoOTA.h>
#include <Wire.h> 

// *** DEFINITION REQUIRED FOR isBusy FIX ***
#define DRV2605_REG_GO 0x0C

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// *** WIFI CREDENTIALS - REPLACE THESE ***
const char* ssid = "WIFI";
const char* password = "PASSWORD";

// This unit listens for the Python router on this port
const unsigned int CLIENT_LISTENER_PORT = 8000;

// Haptic Logic Configuration (Blended Waveform/RTP)
bool isMotorRunning = false; 
bool isInRealtimeMode = false; 
const float MIN_INTENSITY_THRESHOLD = 0.05f;
const float REALTIME_THRESHOLD = 0.35f; 

const unsigned long REALTIME_TIMEOUT_MS = 500;
unsigned long lastReceivedMs = 0;

bool USE_GAMMA_MAPPING = true;
float GAMMA = 2.2f; 

#define EEPROM_SIZE 32
#define DEVICE_ID_ADDR 0
#define RECEIVER_NAME_ADDR 1
#define GAMMA_ADDR 2     
#define GAMMA_FLAG_ADDR 6  
#define BATTERY_LEVEL_PIN A0 

const int NUM_RECEIVERS = 11; 
const char* receiverNames[NUM_RECEIVERS] = {
 "head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
 "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};

const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

const uint8_t LRA_LIBRARY = 6;
const uint8_t LRA_SUBTLE_CLICK = 1; 

ESP8266WebServer httpServer(80);
WiFiUDP Udp;
Adafruit_DRV2605 drv;
int myDeviceID = -1;
int assignedReceiverIndex = 0;
char incomingPacket[256];

String getMDNSHostname() {
 return String(receiverNames[assignedReceiverIndex]);
}

// ------------------------------------
// --- EEPROM / Haptic Helpers (Unchanged) ---
// ------------------------------------

// [loadDeviceID, saveDeviceID, loadAssignedReceiverIndex, saveAssignedReceiverIndex, saveGammaToEEPROM, loadGammaFromEEPROM, getBatteryPercent - ALL UNCHANGED]

int loadDeviceID() {
 int id = EEPROM.read(DEVICE_ID_ADDR);
 if (id >= 0 && id <= 10) return id;
 return -1;
}

void saveDeviceID(int id) {
 EEPROM.write(DEVICE_ID_ADDR, id);
 EEPROM.commit();
}

int loadAssignedReceiverIndex() {
 int idx = EEPROM.read(RECEIVER_NAME_ADDR);
 if (idx >= 0 && idx < NUM_RECEIVERS) return idx;
 return 0;
}

void saveAssignedReceiverIndex(int idx) {
 EEPROM.write(RECEIVER_NAME_ADDR, idx);
 EEPROM.commit();
}

void saveGammaToEEPROM() {
 EEPROM.put(GAMMA_ADDR, GAMMA);
 EEPROM.write(GAMMA_FLAG_ADDR, 0xA5); 
 EEPROM.commit();
}

void loadGammaFromEEPROM() {
 if (EEPROM.read(GAMMA_FLAG_ADDR) == 0xA5) {
  float stored = 0.0f;
  EEPROM.get(GAMMA_ADDR, stored);
  if (!isnan(stored) && stored >= 0.5f && stored <= 6.0f) {
   GAMMA = stored;
  } else {
   GAMMA = 2.2f;
  }
 } else {
 }
}

float getBatteryPercent() {
 const float V_FULL = 4.2f;
 const float V_EMPTY = 3.3f;
 int raw = analogRead(BATTERY_LEVEL_PIN);
 float voltage = raw * (3.3f / 1023.0f);
 float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
 return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------
// --- Gamma / Perceptual Mapping (Unchanged) ---
// ------------------------------------

float applyGammaMapping(float intensity) {
 intensity = constrain(intensity, 0.0f, 1.0f);
 if (!USE_GAMMA_MAPPING) return intensity;
 return powf(intensity, GAMMA);
}

static inline uint8_t intensityToRealtimeValue(float intensity) {
 float corrected = applyGammaMapping(intensity);
 uint8_t val = (uint8_t)roundf(constrain(corrected, 0.0f, 1.0f) * 255.0f);
 return val;
}

// ------------------------------------
// --- HAPTIC CONTROL (FIXED BLENDED LOGIC) ---
// ------------------------------------

void setMotorBlendedRTP(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  // --- 1. STOP CONDITION ---
  if (intensity < MIN_INTENSITY_THRESHOLD) {
    if (isMotorRunning) {
      drv.setRealtimeValue(0); 
      drv.setMode(0); 
      isMotorRunning = false;
      isInRealtimeMode = false;
    }
    drv.stop();
    return;
  }
  
  float correctedIntensity = applyGammaMapping(intensity);
  uint8_t amplitude = (uint8_t)roundf(correctedIntensity * 255.0f);
  
  lastReceivedMs = millis(); 
  isMotorRunning = true;

  // --- 2. HIGH INTENSITY: Realtime Buzz (Continuous Ramping) ---
  if (intensity >= REALTIME_THRESHOLD) {
    if (!isInRealtimeMode) {
      // Switch from Waveform/Standby to Realtime Mode
      drv.setMode(DRV2605_MODE_REALTIME);
      isInRealtimeMode = true;
      drv.stop(); 
    }
    
    // Set the continuous drive value
    drv.setRealtimeValue(amplitude); 

  // --- 3. LOW INTENSITY: Waveform Click (Non-Ramping) ---
  } else {
    if (isInRealtimeMode) {
      // Switch from Realtime Mode to Waveform/Standby 
      drv.setRealtimeValue(0); 
      drv.setMode(DRV2605_MODE_INTTRIG);
      drv.selectLibrary(LRA_LIBRARY);
      isInRealtimeMode = false;
    }
    
    // Check if the motor is currently playing a waveform using register read.
    // Fixed: Replaced drv.isBusy() with standard register check.
    if (!(drv.readRegister8(DRV2605_REG_GO) & 0x01)) { // <-- FIX APPLIED
      
      // Scale down amplitude for a subtle click at low levels
      uint8_t clickAmplitude = (uint8_t)roundf(correctedIntensity * 255.0f * 0.85f); 
      drv.setRealtimeValue(clickAmplitude); 

      // Play a single click waveform
      drv.setWaveform(0, LRA_SUBTLE_CLICK); 
      drv.setWaveform(1, 0); 
      drv.go();
    }
  }
}

// Stop motor if no updates recently
void checkRealtimeTimeout() {
  if (isMotorRunning && isInRealtimeMode) {
    unsigned long now = millis();
    if (now - lastReceivedMs > REALTIME_TIMEOUT_MS) {
      drv.setRealtimeValue(0);
      drv.setMode(0);
      isMotorRunning = false;
      isInRealtimeMode = false;
    }
  }
}

// Handles incoming OSC messages from the Python router.
void handleRouterOscInput() {
 int packetSize = Udp.parsePacket();
 if (packetSize != 0) {
  int len = Udp.read((uint8_t*)incomingPacket, sizeof(incomingPacket));
  if (len <= 0) return;
  OSCMessage msg;
  msg.fill((uint8_t*)incomingPacket, len);

  char addressBuffer[64];
  msg.getAddress(addressBuffer);

  if (strcmp(addressBuffer, INTERNAL_OSC_ADDRESS) == 0) {
     
   if (msg.isFloat(0)) {
    float intensity = msg.getFloat(0);
    setMotorBlendedRTP(intensity); 
   } else if (msg.isInt(0)) {
    int val = msg.getInt(0);
    float intensity = 0.0f;
    if (val > 1 && val <= 255) {
     intensity = (float)val / 255.0f;
    } else if (val >= 0 && val <= 100) {
     intensity = (float)val / 100.0f;
    }
    setMotorBlendedRTP(intensity); 
   }
  }
 }
}

// ------------------------------------
// --- Web GUI & Action Handlers (Unchanged) ---
// ------------------------------------

// [handleConfigAction, handleStatusJSON, WEB_PAGE - ALL UNCHANGED]
void handleConfigAction() {
 String response = "OK";
 int status_code = 200;

 if (httpServer.method() != HTTP_POST) {
   status_code = 405;
   response = "Method not allowed.";
 } else if (httpServer.hasArg("action")) {
  String action = httpServer.arg("action");

  if (action == "test") {
   if (!isInRealtimeMode) {
    drv.setMode(DRV2605_MODE_REALTIME);
    isInRealtimeMode = true;
   }
   isMotorRunning = true; 
  
   for (int v = 0; v <= 255; v += 25) {
    drv.setRealtimeValue((uint8_t)v);
    delay(30);
   }
   for (int v = 255; v >= 0; v -= 25) {
    drv.setRealtimeValue((uint8_t)v);
    delay(30);
   }
   drv.setRealtimeValue(0);
   drv.setMode(0); 
   isMotorRunning = false; 
   isInRealtimeMode = false;
   response = "Realtime test ramp complete. Switched back to standby.";
  }
  else if (action == "set_id" && httpServer.hasArg("id")) {
   int newID = httpServer.arg("id").toInt();
   if (newID >= 0 && newID <= 10) {
    saveDeviceID(newID);
    response = "ID updated to " + String(newID) + ". Restarting...";
    httpServer.send(status_code, "text/plain", response);
    delay(100);
    ESP.restart();
   } else {
    status_code = 400;
    response = "Invalid ID (must be 0-10).";
   }
  }
  else if (action == "set_receiver" && httpServer.hasArg("index")) {
   int newIndex = httpServer.arg("index").toInt();
   if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
    assignedReceiverIndex = newIndex;
    saveAssignedReceiverIndex(assignedReceiverIndex);

    response = "Receiver set to " + String(receiverNames[newIndex]) + ". Restarting to update mDNS...";
    httpServer.send(status_code, "text/plain", response);
    delay(100);
    ESP.restart();
   } else {
    status_code = 400;
    response = "Invalid receiver index.";
   }
  }
  else if (action == "set_gamma" && httpServer.hasArg("gamma")) {
   float newGamma = httpServer.arg("gamma").toFloat();
   if (newGamma >= 0.5f && newGamma <= 6.0f) {
    GAMMA = newGamma;
    saveGammaToEEPROM();        
    response = "Gamma updated to " + String(GAMMA, 2) + ".";
   } else {
    status_code = 400;
    response = "Invalid gamma (must be between 0.5 and 6.0).";
   }
  }
  else {
   status_code = 400;
   response = "Invalid action or missing parameter.";
  }
 }

 httpServer.send(status_code, "text/plain", response);
}

void handleStatusJSON() {
 String json = "{";
 json += "\"deviceID\":" + String(myDeviceID) + ",";
 json += "\"role\":\"Client (mDNS Router Mode - Blended RTP)\","; 
 json += "\"receiverIndex\":" + String(assignedReceiverIndex) + ",";
 json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
 json += "\"hostname\":\"" + getMDNSHostname() + ".local\",";
 json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
 json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
 json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
 json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
 json += "\"gamma\":" + String(GAMMA, 2);
 json += "}";
 httpServer.send(200, "application/json", json);
}

const char PROGMEM WEB_PAGE[] = R"raw(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Sharkee Haptics Config (Blended RTP - Gamma)</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap');
    body { font-family: 'Inter', sans-serif; background-color: #1a1a2e; color: #e4e4e4; margin: 0; padding: 20px; }
    .container { max-width: 520px; margin: 0 auto; background-color: #272747; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4); }
    h1 { text-align: center; color: #4CAF50; margin-bottom: 20px; }
    .card { background-color: #313156; padding: 15px; border-radius: 8px; margin-bottom: 15px; }
    .card-header { font-weight: bold; margin-bottom: 10px; color: #9C27B0; font-size: 1.1em; }
    label { display: block; margin-bottom: 5px; color: #ccc; }
    input[type="number"], select {
      width: 100%; padding: 10px; margin-bottom: 10px; border-radius: 5px; border: 1px solid #555; background-color: #44446a; color: #fff; box-sizing: border-box;
    }
    button {
      background-color: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 5px; cursor: pointer; width: 100%; transition: background-color 0.3s ease, transform 0.1s ease;
      font-weight: bold;
    }
    button:hover { background-color: #45a049; }
    button:active { transform: scale(0.98); }
    .status-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
    .status-item { background-color: #44446a; padding: 8px; border-radius: 5px; text-align: center; font-size: 0.9em; }
    .status-value { font-weight: bold; font-size: 1.1em; color: #FFEB3B; }
    #message { margin-top: 15px; padding: 10px; border-radius: 5px; text-align: center; font-weight: bold; }
    #message.success { background-color: #1e872e; }
    #message.error { background-color: #c73a3a; }
    .hostname-display { grid-column: 1 / span 2; font-size: 0.8em; color: #aaa; text-align: center; padding-top: 5px; }
    .inline { display: flex; gap: 8px; align-items:center; }
    .small { width: 140px; margin-right: 8px; }
  </style>
</head>
<body>
<div class="container">
  <h1>Sharkee Haptics Client (Blended RTP - Gamma)</h1>

  <div id="status-card" class="card">
    <div class="card-header">Current Status (<span id="role-display">Client (Router Mode)</span>)</div>
    <div class="status-grid">
      <div class="status-item">Device ID:<br><span id="display-id" class="status-value">?</span></div>
      <div class="status-item">Assigned:<br><span id="display-receiver" class="status-value">?</span></div>
      <div class="status-item">Battery:<br><span id="display-battery" class="status-value">?</span></div>
      <div class="status-item">IP Address:<br><span id="display-ip" class="status-value">?</span></div>
      <div class="hostname-display">mDNS Hostname: <span id="display-hostname" class="status-value" style="font-weight: normal; color: #81D4FA;">?</span></div>
    </div>
    <div id="message"></div>
  </div>

  <div class="card">
    <div class="card-header">1. Assign VRChat Receiver (Requires Restart)</div>
    <label for="receiver-select">Receiver Target:</label>
    <select id="receiver-select"></select>
    <button onclick="setReceiver()">Set Receiver Target</button>
  </div>

  <div class="card">
    <div class="card-header">2. Set Device ID (Requires Restart for OTA)</div>
    <label for="device-id-input">New Device ID (0-10):</label>
    <input type="number" id="device-id-input" min="0" max="10" value="0">
    <button onclick="setID()">Set ID & Restart</button>
  </div>

  <div class="card">
    <div class="card-header">3. Test Haptic Motor (Realtime)</div>
    <button onclick="testHaptic()">Run Realtime Test Ramp</button>
  </div>

  <div class="card">
    <div class="card-header">4. Gamma Mapping</div>
    <label for="gamma-input">Gamma (exponent) — higher => more low-end control (typical LRA 1.8–2.6)</label>
    <div class="inline">
      <input id="gamma-input" class="small" type="number" step="0.1" min="0.5" max="6.0" value="2.2">
      <button onclick="setGamma()">Set Gamma</button>
    </div>
    <div style="margin-top:10px; font-size:0.9em; color:#ccc;">Current gamma: <span id="display-gamma">2.2</span></div>
  </div>
</div>

<script>
  const RECEIVER_NAMES = ["Head","Chest","UpperArm_L","UpperArm_R","Hips","UpperLeg_L","UpperLeg_R","LowerLeg_L","LowerLeg_R","Foot_L","Foot_R"];
  const MAX_DEVICE_ID = 10;
  const messageElement = document.getElementById('message');
  const receiverSelect = document.getElementById('receiver-select');

  // Populate receiver options on load
  RECEIVER_NAMES.forEach((name, index) => {
    const option = document.createElement('option');
    option.value = index;
    option.textContent = name;
    receiverSelect.appendChild(option);
  });

  function showMessage(msg, type = 'success') {
    messageElement.textContent = msg;
    messageElement.className = (type === 'success') ? 'success' : 'error';
  }

  function fetchData() {
    fetch('/status_json')
      .then(response => response.json())
      .then(data => {
        document.getElementById('display-id').textContent = data.deviceID === -1 ? 'UNSET' : data.deviceID;
        document.getElementById('display-receiver').textContent = RECEIVER_NAMES[data.receiverIndex];
        document.getElementById('display-battery').textContent = `${data.battery}%`;
        document.getElementById('display-ip').textContent = data.ip;
        document.getElementById('display-hostname').textContent = data.hostname;
        document.getElementById('role-display').textContent = data.role;
        document.getElementById('display-gamma').textContent = parseFloat(data.gamma).toFixed(2);

        receiverSelect.value = data.receiverIndex;
        const idDisplay = document.getElementById('display-id');
        idDisplay.style.color = (data.deviceID === -1) ? 'red' : '#FFEB3B';
        const batteryDisplay = document.getElementById('display-battery');
        batteryDisplay.style.color = (data.battery < 20) ? 'red' : '#FFEB3B';

        setTimeout(fetchData, 3000);
      })
      .catch(error => {
        document.getElementById('display-id').textContent = 'ERR';
        document.getElementById('display-ip').textContent = 'WIFI DOWN';
        console.error('Error fetching status:', error);
        setTimeout(fetchData, 5000);
      });
  }

  function sendAction(action, data = {}) {
    messageElement.className = '';
    messageElement.textContent = 'Processing...';
    const formData = new URLSearchParams();
    formData.append('action', action);
    for (const key in data) formData.append(key, data[key]);
    return fetch('/action', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: formData
    })
    .then(response => {
      if (!response.ok) return response.text().then(text => { throw new Error(text || response.statusText); });
      return response.text();
    })
    .then(text => {
      showMessage(text);
      if (action !== 'test' && !text.includes('Restarting')) fetchData();
    })
    .catch(error => showMessage(`Error: ${error.message}`, 'error'));
  }

  function setID() {
    const idInput = document.getElementById('device-id-input').value;
    const id = parseInt(idInput);
    if (id >= 0 && id <= MAX_DEVICE_ID) sendAction('set_id', { id: id });
    else showMessage("ID must be between 0 and 10.", 'error');
  }

  function setReceiver() {
    const index = parseInt(receiverSelect.value);
    if (index >= 0 && index < RECEIVER_NAMES.length) sendAction('set_receiver', { index: index });
  }

  function testHaptic() {
    sendAction('test');
  }

  function setGamma() {
    const g = parseFloat(document.getElementById('gamma-input').value);
    if (isNaN(g) || g < 0.5 || g > 6.0) {
      showMessage("Gamma must be between 0.5 and 6.0", 'error');
      return;
    }
    sendAction('set_gamma', { gamma: g });
  }

  document.addEventListener('DOMContentLoaded', fetchData);
</script>
</body>
</html>
)raw";

void handleRoot() {
 httpServer.send(200, "text/html", WEB_PAGE);
}

void setupMDNS() {
 String hostname = getMDNSHostname();
 if (MDNS.begin(hostname.c_str())) {
  Serial.printf("mDNS responder started with hostname: %s.local\n", hostname.c_str());
  MDNS.addService("http", "tcp", 80);
 } else {
  Serial.println("Error starting mDNS!");
 }
}

void setupWiFiAndNetwork() {
 WiFi.mode(WIFI_STA);
 WiFi.begin(ssid, password);
 Serial.print("Connecting to WiFi");
 int attempts = 0;
 while (WiFi.status() != WL_CONNECTED && attempts < 120) {
  delay(500);
  Serial.print(".");
  attempts++;
 }
 if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  setupMDNS();
 } else {
  Serial.println("\nFailed to connect to WiFi. Check credentials and power cycle.");
 }

 httpServer.on("/", HTTP_GET, handleRoot);
 httpServer.on("/status_json", HTTP_GET, handleStatusJSON);
 httpServer.on("/action", HTTP_POST, handleConfigAction);
 httpServer.begin();
 Serial.println("Web GUI (Port 80) started.");

 if (Udp.begin(CLIENT_LISTENER_PORT)) {
  Serial.printf("CLIENT: Listening for Python Router on Port %d.\n", CLIENT_LISTENER_PORT);
 } else {
  Serial.println("CLIENT: Failed to start OSC listener!");
 }

 if (myDeviceID != -1) {
  ArduinoOTA.setHostname(("sharkeehaptics" + String(myDeviceID)).c_str());
 } else {
  ArduinoOTA.setHostname("sharkeehaptics-unassigned");
 }
 ArduinoOTA.begin();
 Serial.println("OTA ready.");
}

void setupHaptic() {
 Serial.print("DRV2605 initialization...");
  Wire.begin(); 
 if (!drv.begin()) {
  Serial.println("Failed to find DRV2605 chip!");
  while (1) { delay(1); }
 }
 Serial.println("done.");
 
 // Fixed: Use LRA library (6) selection instead of non-standard setMotorType(1)
 drv.selectLibrary(LRA_LIBRARY); 
 drv.setMode(0); // idle (standby)
}

void setup() {
 Serial.begin(115200);
 delay(100);
 Serial.println("\n--- Sharkee Haptics mDNS Router Client (Blended RTP + Gamma) ---");

 EEPROM.begin(EEPROM_SIZE);
 myDeviceID = loadDeviceID();
 assignedReceiverIndex = loadAssignedReceiverIndex();
 loadGammaFromEEPROM(); 

 Serial.printf("Loaded Device ID: %d\n", myDeviceID);
 Serial.printf("Assigned Receiver: %s\n", receiverNames[assignedReceiverIndex]);
 Serial.printf("Loaded Gamma: %.2f\n", GAMMA);

 setupWiFiAndNetwork();
 setupHaptic();
}

void loop() {
 handleRouterOscInput();
 httpServer.handleClient();
 ArduinoOTA.handle();
 MDNS.update();
 checkRealtimeTimeout();
}