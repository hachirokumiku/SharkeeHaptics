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
const char* ssid = "SHAW-0DBC_RPT2G";
const char* password = "amaze6533energy";

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
  int motorValue = (int)(intensity * 127);

  if (intensity < MIN_INTENSITY_THRESHOLD) {
    if (isMotorRunning) {
      drv.setRealtimeValue(0);
      isMotorRunning = false;
    }
  } else {
    drv.setRealtimeValue(motorValue);
    isMotorRunning = true;
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
      if (newID >= 0 && newID <= 10) {
        myDeviceID = newID;
        saveDeviceID(newID);
        response = "Device ID set to " + String(newID);
      } else {
        status_code = 400;
        response = "Invalid device ID.";
      }
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        assignedReceiverIndex = newIndex;
        saveAssignedReceiverIndex(newIndex);
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
        body { font-family: Arial, sans-serif; background: #1e1e1e; color: #fff; padding: 20px; }
        h1 { color: #00d4ff; }
        .info { background: #2e2e2e; padding: 15px; margin: 10px 0; border-radius: 5px; }
        .controls { background: #3e3e3e; padding: 15px; margin: 10px 0; border-radius: 5px; }
        button { background: #00d4ff; color: #000; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; margin: 5px; }
        button:hover { background: #00a3cc; }
        select { padding: 5px; margin: 5px; }
        input { padding: 5px; margin: 5px; }
    </style>
</head>
<body>
    <h1>Sharkee Haptics Configuration</h1>
    <div class="info" id="info">Loading...</div>
    <div class="controls">
        <h2>Actions</h2>
        <button onclick="testHaptic()">Test Haptic</button><br>
        <label>Device ID (0-10):</label>
        <input type="number" id="deviceID" min="0" max="10" value="0">
        <button onclick="setDeviceID()">Set Device ID</button><br>
        <label>Receiver:</label>
        <select id="receiverSelect"></select>
        <button onclick="setReceiver()">Set Receiver</button>
    </div>

    <script>
    function fetchData() {
        fetch('/status_json')
            .then(res => res.json())
            .then(data => {
                document.getElementById('info').innerHTML = 
                    '<strong>Device ID:</strong> ' + data.deviceID + '<br>' +
                    '<strong>Role:</strong> ' + data.role + '<br>' +
                    '<strong>Receiver:</strong> ' + data.receiverName + ' (Index: ' + data.receiverIndex + ')<br>' +
                    '<strong>Hostname:</strong> ' + data.hostname + '<br>' +
                    '<strong>Listening Port:</strong> ' + data.listeningOn + '<br>' +
                    '<strong>Battery:</strong> ' + data.battery + '%<br>' +
                    '<strong>IP Address:</strong> ' + data.ip + '<br>' +
                    '<strong>WiFi RSSI:</strong> ' + data.rssi + ' dBm';
                
                document.getElementById('deviceID').value = data.deviceID;
                
                let select = document.getElementById('receiverSelect');
                select.innerHTML = '';
                const receivers = ['head', 'chest', 'upperarm_l', 'upperarm_r', 'hips', 'upperleg_l', 
                                   'upperleg_r', 'lowerleg_l', 'lowerleg_r', 'foot_l', 'foot_r'];
                receivers.forEach((name, index) => {
                    let option = document.createElement('option');
                    option.value = index;
                    option.text = name;
                    if (index === data.receiverIndex) option.selected = true;
                    select.appendChild(option);
                });
            });
    }

    function sendAction(action, params = {}) {
        let formData = new FormData();
        formData.append('action', action);
        for (let key in params) {
            formData.append(key, params[key]);
        }
        fetch('/action', { method: 'POST', body: formData })
            .then(res => res.text())
            .then(text => {
                alert(text);
                setTimeout(fetchData, 500);
            });
    }

    function setDeviceID() {
        let id = document.getElementById('deviceID').value;
        sendAction('set_id', { id: id });
    }

    function setReceiver() {
        let index = document.getElementById('receiverSelect').value;
        if (confirm('Setting receiver will restart the device. Continue?')) {
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

  Udp.begin(CLIENT_LISTENER_PORT);
  Serial.printf("UDP Listener started on port %d\n", CLIENT_LISTENER_PORT);

  // Initialize ArduinoOTA for wireless firmware updates
  ArduinoOTA.setHostname(getMDNSHostname().c_str());
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Update Starting...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update Complete!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA initialized.");
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
