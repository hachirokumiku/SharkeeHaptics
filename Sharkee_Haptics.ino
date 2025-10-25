#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h> 
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h> 
#include <ArduinoOTA.h>

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// *** WIFI CREDENTIALS - REPLACE THESE ***
const char* ssid = "WIFI";
const char* password = "PASSWORD";

// This unit listens for the Python router on this port
const unsigned int CLIENT_LISTENER_PORT = 8000;

// ----------------------------------------------------

// Haptic Logic Configuration
bool isMotorRunning = false;
const float MIN_INTENSITY_THRESHOLD = 0.05;

// EEPROM addresses for persistent storage
#define EEPROM_SIZE 16 
#define DEVICE_ID_ADDR 0 
#define RECEIVER_NAME_ADDR 1 
#define BATTERY_LEVEL_PIN A0    // Analog pin for battery monitoring

// VRChat Receiver Mapping - NOW INCLUDES 'head'
const int NUM_RECEIVERS = 11; // Changed from 10
const char* receiverNames[NUM_RECEIVERS] = {
  "head", // New: Head location
  "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
  "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};

// The simple OSC address this unit expects from the Python router
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

// --- Global Objects and State ---
ESP8266WebServer httpServer(80);
WiFiUDP Udp;
Adafruit_DRV2605 drv;
int myDeviceID = -1;
int assignedReceiverIndex = 0;
char incomingPacket[256];

// Helper function to get the mDNS hostname
String getMDNSHostname() {
  return String(receiverNames[assignedReceiverIndex]);
}

// ------------------------------------
// --- EEPROM / Haptic Helpers ---
// ------------------------------------

// Loads the device ID from EEPROM. Returns -1 if invalid.
int loadDeviceID() {
  int id = EEPROM.read(DEVICE_ID_ADDR);
  // MODIFIED: check changed from id < 10 to id <= 10
  if (id >= 0 && id <= 10) return id; 
  return -1;
}

// Saves the device ID to EEPROM.
void saveDeviceID(int id) {
  EEPROM.write(DEVICE_ID_ADDR, id);
  EEPROM.commit();
}

// Loads the assigned receiver index from EEPROM. Returns 0 if invalid.
int loadAssignedReceiverIndex() {
  int idx = EEPROM.read(RECEIVER_NAME_ADDR);
  if (idx >= 0 && idx < NUM_RECEIVERS) return idx; // Updated bounds check
  return 0;
}

// Saves the assigned receiver index to EEPROM.
void saveAssignedReceiverIndex(int idx) {
  EEPROM.write(RECEIVER_NAME_ADDR, idx);
  EEPROM.commit();
}

// Reads the battery level and returns a percentage (0-100).
float getBatteryPercent() {
  // Placeholder formula - calibrate this for your circuit
  const float V_FULL = 4.2;
  const float V_EMPTY = 3.3;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  // Simple voltage calculation (needs external voltage divider calibration)
  float voltage = raw * (3.3 / 1023.0);
  // Convert raw voltage to a 0-100% scale based on defined full/empty values
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0;
  return constrain(percent, 0, 100);
}

// Controls the DRV2605 motor speed using Real-Time Playback mode.
void setMotorSpeed(float intensity) {
  intensity = constrain(intensity, 0.0, 1.0);

  if (intensity >= MIN_INTENSITY_THRESHOLD) {
    if (!isMotorRunning) {
      // Switch to Realtime mode only when needed
      drv.setMode(DRV2605_MODE_REALTIME);
      isMotorRunning = true;
    }
    uint8_t speed = (uint8_t)round(intensity * 255.0);
    drv.setRealtimeValue(speed);

  } else if (isMotorRunning) {
    // Stop the motor and exit Realtime mode when done
    drv.setRealtimeValue(0);
    drv.setMode(0); // Exit Realtime mode to save power/resources
    isMotorRunning = false;
  }
}

// Handles incoming OSC messages from the Python router.
void handleRouterOscInput() {
  if (Udp.parsePacket() != 0) {
    Udp.read(incomingPacket, 256);
    OSCMessage msg;
    msg.fill((uint8_t*)incomingPacket, 256);

    char addressBuffer[64];
    msg.getAddress(addressBuffer);

    // Check if the OSC address matches the expected internal control address
    if (strcmp(addressBuffer, INTERNAL_OSC_ADDRESS) == 0) {
      if (msg.isFloat(0)) {
        float intensity = msg.getFloat(0);
        setMotorSpeed(intensity);
      }
    }
  }
}

// ------------------------------------
// --- Web GUI & Action Handlers ---
// ------------------------------------

// Handles configuration changes (test, set ID, set receiver).
void handleConfigAction() {
  String response = "OK";
  int status_code = 200;

  if (httpServer.method() != HTTP_POST) {
      status_code = 405;
      response = "Method not allowed.";
  } else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "test") {
      // Run a simple LRA effect test (Effect 7: click)
      drv.setMode(DRV2605_MODE_INTTRIG);
      drv.selectLibrary(1);
      drv.setWaveform(0, 7);
      drv.setWaveform(1, 0);
      drv.go();
      delay(200);
      drv.setMode(0);
      isMotorRunning = false;
      response = "Test complete.";
    }
    else if (action == "set_id" && httpServer.hasArg("id")) {
      int newID = httpServer.arg("id").toInt();
      // check changed from newID < 10 to newID <= 10
      if (newID >= 0 && newID <= 10) { 
        saveDeviceID(newID);
        response = "ID updated to " + String(newID) + ". Restarting...";
        httpServer.send(status_code, "text/plain", response);
        delay(100);
        ESP.restart();
      } else {
        status_code = 400;
        // Error message updated to reflect the new range
        response = "Invalid ID (must be 0-10)."; 
      }
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        assignedReceiverIndex = newIndex;
        saveAssignedReceiverIndex(assignedReceiverIndex);

        // CRITICAL: Restart so the new mDNS hostname is registered immediately
        response = "Receiver set to " + String(receiverNames[newIndex]) + ". Restarting to update mDNS...";
        httpServer.send(status_code, "text/plain", response);
        delay(100);
        ESP.restart();
      } else {
        status_code = 400;
        response = "Invalid receiver index.";
      }
    }
    else {
      status_code = 400;
      response = "Invalid action or missing parameter.";
    }
  }

  httpServer.send(status_code, "text/plain", response);
}

// Provides current device status as JSON.
void handleStatusJSON() {
  String json = "{";
  json += "\"deviceID\":" + String(myDeviceID) + ",";
  json += "\"role\":\"Client (mDNS Router Mode)\",";
  json += "\"receiverIndex\":" + String(assignedReceiverIndex) + ",";
  json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
  json += "\"hostname\":\"" + getMDNSHostname() + ".local\",";
  json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
  json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI());
  json += "}";
  httpServer.send(200, "application/json", json);
}

// HTML content for the configuration web page.
const char PROGMEM WEB_PAGE[] = R"raw(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Sharkee Haptics Config</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap');
        body { font-family: 'Inter', sans-serif; background-color: #1a1a2e; color: #e4e4e4; margin: 0; padding: 20px; }
        .container { max-width: 450px; margin: 0 auto; background-color: #272747; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4); }
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
    </style>
</head>
<body>
<div class="container">
    <h1>Sharkee Haptics Client</h1>

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
        <select id="receiver-select">
            </select>
        <button onclick="setReceiver()">Set Receiver Target</button>
    </div>

    <div class="card">
        <div class="card-header">2. Set Device ID (Requires Restart for OTA)</div>
        <label for="device-id-input">New Device ID (0-10):</label>
        <input type="number" id="device-id-input" min="0" max="10" value="0">
        <button onclick="setID()">Set ID & Restart</button>
    </div>

    <div class="card">
        <div class="card-header">3. Test Haptic Motor</div>
        <button onclick="testHaptic()">Run Test Vibration</button>
    </div>
</div>

<script>
    // Note: JS uses title case for display, but the underlying data (and mDNS names) use lowercase defined in the ESP.
    // ADDED "Head" to the list
    const RECEIVER_NAMES = ["Head", "Chest", "UpperArm_L", "UpperArm_R", "Hips", "UpperLeg_L", "UpperLeg_R", "LowerLeg_L", "LowerLeg_R", "Foot_L", "Foot_R"];
    // ADDED "head" to the list
    const RECEIVER_MDNS = ["head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l", "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"];
    
    // NEW CONSTANT: Max Device ID
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
                // Display status
                document.getElementById('display-id').textContent = data.deviceID === -1 ? 'UNSET' : data.deviceID;
                // Use the capitalized name for display
                document.getElementById('display-receiver').textContent = RECEIVER_NAMES[data.receiverIndex];
                document.getElementById('display-battery').textContent = `${data.battery}%`;
                document.getElementById('display-ip').textContent = data.ip;
                document.getElementById('display-hostname').textContent = data.hostname; // Display mDNS
                document.getElementById('role-display').textContent = data.role;


                // Sync the dropdown with the current assigned index
                receiverSelect.value = data.receiverIndex;

                const idDisplay = document.getElementById('display-id');
                idDisplay.style.color = (data.deviceID === -1) ? 'red' : '#FFEB3B';

                const batteryDisplay = document.getElementById('display-battery');
                batteryDisplay.style.color = (data.battery < 20) ? 'red' : '#FFEB3B';

                setTimeout(fetchData, 3000); // Poll every 3 seconds
            })
            .catch(error => {
                document.getElementById('display-id').textContent = 'ERR';
                document.getElementById('display-ip').textContent = 'WIFI DOWN';
                console.error('Error fetching status:', error);
                setTimeout(fetchData, 5000); // Try again in 5 seconds
            });
    }

    function sendAction(action, data = {}) {
        messageElement.className = '';
        messageElement.textContent = 'Processing...';

        const formData = new URLSearchParams();
        formData.append('action', action);
        for (const key in data) {
            formData.append(key, data[key]);
        }

        return fetch('/action', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded'
            },
            body: formData
        })
        .then(response => {
            if (!response.ok) {
                return response.text().then(text => { throw new Error(text || response.statusText); });
            }
            return response.text();
        })
        .then(text => {
            showMessage(text);
            if (action !== 'test' && !text.includes('Restarting')) {
                 fetchData();
            }
        })
        .catch(error => {
            showMessage(`Error: ${error.message}`, 'error');
        });
    }

    function setID() {
        const idInput = document.getElementById('device-id-input').value;
        const id = parseInt(idInput);
        // check changed from id <= 9 to id <= MAX_DEVICE_ID (10)
        if (id >= 0 && id <= MAX_DEVICE_ID) { 
            sendAction('set_id', { id: id });
        } else {
            // Error message updated to reflect the new range
            showMessage("ID must be between 0 and 10.", 'error'); 
        }
    }

    function setReceiver() {
        const index = parseInt(receiverSelect.value);
        if (index >= 0 && index < RECEIVER_NAMES.length) {
            // Note to user: This action will cause the ESP to restart and update its mDNS name.
            sendAction('set_receiver', { index: index });
        }
    }

    function testHaptic() {
        sendAction('test');
    }

    document.addEventListener('DOMContentLoaded', fetchData);
</script>
</body>
</html>
)raw";

void handleRoot() {
  httpServer.send(200, "text/html", WEB_PAGE);
}

// Sets up the mDNS hostname and registers the HTTP service.
void setupMDNS() {
  String hostname = getMDNSHostname();
  if (MDNS.begin(hostname.c_str())) {
    Serial.printf("mDNS responder started with hostname: %s.local\n", hostname.c_str());
    // Explicitly register the web configuration interface on port 80
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error starting mDNS!");
  }
}

// ------------------------------------
// --- Setup Functions ---
// ------------------------------------

void setupWiFiAndNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  // MODIFIED: Increased attempts from 40 (20s) to 120 (60s) for robust connection.
  while (WiFi.status() != WL_CONNECTED && attempts < 120) { 
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
    setupMDNS(); // Start mDNS after connecting to WiFi
  } else {
    Serial.println("\nFailed to connect to WiFi. Check credentials and power cycle.");
  }

  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/status_json", HTTP_GET, handleStatusJSON);
  httpServer.on("/action", HTTP_POST, handleConfigAction);
  httpServer.begin();
  Serial.println("Web GUI (Port 80) started.");

  if(Udp.begin(CLIENT_LISTENER_PORT)) {
    Serial.printf("CLIENT: Listening for Python Router on Port %d.\n", CLIENT_LISTENER_PORT);
  } else {
    Serial.println("CLIENT: Failed to start OSC listener!");
  }

  // Over-The-Air (OTA) Updates
  if (myDeviceID != -1) {
    ArduinoOTA.setHostname(("sharkeehaptics" + String(myDeviceID)).c_str()); // Updated name
  } else {
    ArduinoOTA.setHostname("sharkeehaptics-unassigned"); // Updated name
  }
  ArduinoOTA.begin();
  Serial.println("OTA ready.");
}

void setupHaptic() {
  Serial.print("DRV2605 initialization...");
  if (!drv.begin()) {
    Serial.println("Failed to find DRV2605 chip!");
    while (1) { delay(1); }
  }
  Serial.println("done.");
  drv.selectLibrary(1);
  drv.setMode(DRV2605_MODE_REALTIME);
  drv.setRealtimeValue(0);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- Sharkee Haptics mDNS Router Client ---"); // Updated name

  EEPROM.begin(EEPROM_SIZE);
  myDeviceID = loadDeviceID();
  assignedReceiverIndex = loadAssignedReceiverIndex();

  Serial.printf("Loaded Device ID: %d\n", myDeviceID);
  Serial.printf("Assigned Receiver: %s\n", receiverNames[assignedReceiverIndex]);

  setupWiFiAndNetwork();
  setupHaptic();
}

void loop() {
  handleRouterOscInput();
  httpServer.handleClient();
  ArduinoOTA.handle();
  // Service mDNS queries continuously
  MDNS.update();
}
