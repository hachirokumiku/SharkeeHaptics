/*
 Turnkey_Sharkee_Haptics_Blended_Click_Rumble.ino
 Logic: Quick, low-intensity tap plays a single Waveform Click.
     Sustained or high intensity drives a smooth RTP Rumble.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <Wire.h> 
// *** FIX: EEPROM Library Added ***
#include <EEPROM.h> 

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// *** ⚠️ REQUIRED: REPLACE THESE PLACEHOLDERS WITH YOUR WIFI CREDENTIALS ⚠️ ***
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Network
const unsigned int CLIENT_LISTENER_PORT = 8000;
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

// Haptic Logic (Blended Configuration)
bool isMotorRunning = false; 
bool isInRealtimeMode = false; 
const float MIN_INTENSITY_THRESHOLD = 0.05f; 
// NEW THRESHOLD: Separates the low-level click from the continuous rumble
const float WAVEFORM_RTP_THRESHOLD = 0.35f; 

const unsigned long REALTIME_TIMEOUT_MS = 500; 
unsigned long lastReceivedMs = 0;

// DRV2605 Specifics
const uint8_t LRA_LIBRARY = 6;
// Waveform 1 is typically a sharp click in the LRA library.
const uint8_t LRA_SHARP_CLICK = 1; 

// Perceptual Mapping (Gamma)
bool USE_GAMMA_MAPPING = true;
float GAMMA = 2.2f; 

// Device Naming and Configuration (Used for Web/mDNS)
#define EEPROM_SIZE 32
#define RECEIVER_NAME_ADDR 1
#define GAMMA_ADDR 2     
#define GAMMA_FLAG_ADDR 6  
#define BATTERY_LEVEL_PIN A0 

const int NUM_RECEIVERS = 11; 
const char* receiverNames[NUM_RECEIVERS] = {
 "head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
 "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};
int assignedReceiverIndex = 0;

// ------------------------------------
// --- Global Objects ---
// ------------------------------------
ESP8266WebServer httpServer(80);
WiFiUDP Udp;
Adafruit_DRV2605 drv;
char incomingPacket[256];

// ------------------------------------
// --- Helper Functions ---
// ------------------------------------

float applyGammaMapping(float intensity) {
 intensity = constrain(intensity, 0.0f, 1.0f);
 if (!USE_GAMMA_MAPPING) return intensity;
 return powf(intensity, GAMMA);
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
// --- Haptic Control: The Blended Logic ---
// ------------------------------------

void setMotorBlendedRTP(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  // CRITICAL: INVERT INTENSITY DIRECTION (0.0=Far/Weak to 1.0=Near/Strong)
  intensity = 1.0f - intensity; 
  
  // --- 1. STOP CONDITION ---
  if (intensity < MIN_INTENSITY_THRESHOLD) {
    if (isMotorRunning) {
      // Ensure motor is stopped from either RTP or Waveform modes
      if (isInRealtimeMode) drv.setRealtimeValue(0); 
      drv.setMode(0); // Switch to Standby
      isMotorRunning = false;
      isInRealtimeMode = false;
    }
    drv.stop();
    return;
  }
  
  // --- 2. WAVEFORM CLICK MODE (Low Intensity / Quick Tap) ---
  // If intensity is above min threshold but below the rumble threshold
  if (intensity < WAVEFORM_RTP_THRESHOLD) {
    if (isInRealtimeMode) {
      // Must stop RTP before switching to Waveform mode
      drv.setRealtimeValue(0);
      drv.setMode(1); // Set to Library/Internal Trigger Mode
      isInRealtimeMode = false;
    }
    
    // Play a single click effect
    drv.setWaveform(0, LRA_SHARP_CLICK); 
    drv.setWaveform(1, 0); // End sequence
    drv.go();
    
    isMotorRunning = true;
  }
  
  // --- 3. HD RUMBLE: Realtime Buzz (Sustained / High Intensity) ---
  else { // intensity >= WAVEFORM_RTP_THRESHOLD
    if (!isInRealtimeMode) {
      // Switch to Realtime Mode once
      drv.setMode(DRV2605_MODE_REALTIME);
      isInRealtimeMode = true;
      drv.stop(); // Stop any pending waveform sequence
    }
    
    // Calculate amplitude with Gamma correction
    float correctedIntensity = applyGammaMapping(intensity);
    uint8_t amplitude = (uint8_t)roundf(correctedIntensity * 255.0f); 
    
    // Set the continuous drive value for the HD Rumble
    drv.setRealtimeValue(amplitude); 
    
    isMotorRunning = true;
  }

  lastReceivedMs = millis(); 
}

// Stop motor if no updates recently
void checkRealtimeTimeout() {
  // Only check for timeout if we are currently in the continuous RTP mode
  if (isMotorRunning && isInRealtimeMode) {
    unsigned long now = millis();
    if (now - lastReceivedMs > REALTIME_TIMEOUT_MS) {
      // Gracefully stop the RTP mode
      drv.setRealtimeValue(0);
      drv.setMode(0); // Standby
      isMotorRunning = false;
      isInRealtimeMode = false;
    }
  }
}

// ------------------------------------
// --- Network & Communication ---
// ------------------------------------

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
   float intensity = 0.0f;
   
   if (msg.isFloat(0)) {
    intensity = msg.getFloat(0);
   } else if (msg.isInt(0)) {
    int val = msg.getInt(0);
    if (val > 1 && val <= 255) {
     intensity = (float)val / 255.0f;
    } else if (val >= 0 && val <= 100) {
     intensity = (float)val / 100.0f;
    }
   }
   // Use the new blended logic
   setMotorBlendedRTP(intensity); 
  }
 }
}

// ------------------------------------
// --- Web Server Handlers (Unchanged from previous turnkey) ---
// ------------------------------------

void handleStatusJSON() {
 String json = "{";
 json += "\"role\":\"Client (Blended Click/Rumble Mode)\","; 
 json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
 json += "\"hostname\":\"" + String(receiverNames[assignedReceiverIndex]) + ".local\",";
 json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
 json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
 json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
 json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
 json += "\"gamma\":" + String(GAMMA, 2);
 json += "}";
 httpServer.send(200, "application/json", json);
}

void handleConfigPage() {
    String html = "<html><head><title>Haptic Device Config</title>";
    html += "<style>body{font-family:Arial;background-color:#222;color:#eee;} .container{max-width:400px;margin:50px auto;padding:20px;background-color:#333;border-radius:8px;} h2{color:#4CAF50;} label, select, input, button{display:block;width:100%;margin-bottom:10px;padding:8px;border-radius:4px;} select, input{background-color:#444;color:#eee;border:1px solid #555;} button{background-color:#9C27B0;color:white;border:none;cursor:pointer;} .status{margin-top:15px;padding:10px;background-color:#444;border-radius:4px;}</style>";
    html += "</head><body><div class='container'><h2>Blended Haptic Configuration</h2>";
    html += "<p>Hostname: <b>" + String(receiverNames[assignedReceiverIndex]) + ".local</b></p>";
    html += "<p>IP Address: <b>" + WiFi.localIP().toString() + "</b></p>";
    html += "<p>Gamma Correction: <b>" + String(GAMMA, 2) + "</b></p>";
    html += "<p>Battery: <b>" + String((int)getBatteryPercent()) + "%</b></p>";
    html += "<p>Mode: <b>Click (Low) to Rumble (High)</b></p>";
    
    // Test Button (Re-uses the RTP ramp for a full system check)
    html += "<form action='/action' method='post'><input type='hidden' name='action' value='test'><button type='submit'>Run Rumble Test</button></form>";

    // Set Receiver Dropdown
    html += "<form action='/action' method='post'><label for='receiver_index'>Set Receiver (Hostname)</label><select id='receiver_index' name='index'>";
    for(int i=0; i<NUM_RECEIVERS; i++) {
        html += "<option value='" + String(i) + "'" + (i == assignedReceiverIndex ? " selected" : "") + ">" + receiverNames[i] + "</option>";
    }
    html += "</select><input type='hidden' name='action' value='set_receiver'><button type='submit'>Change Receiver & Restart</button></form>";

    // Gamma Input
    html += "<form action='/action' method='post'><label for='gamma'>Set Gamma (0.5 to 6.0)</label><input type='number' step='0.1' min='0.5' max='6.0' id='gamma' name='gamma' value='" + String(GAMMA, 2) + "'><input type='hidden' name='action' value='set_gamma'><button type='submit'>Save Gamma</button></form>";

    html += "</div></body></html>";
    httpServer.send(200, "text/html", html);
}

void handleConfigAction() {
  // Simple action handler for the web page
  String response = "OK";
  int status_code = 200;
  if (httpServer.method() != HTTP_POST) { status_code = 405; response = "Method not allowed."; } 
  else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "test") {
      // Test sequence: ramp up and down (RTP is best for demonstrating range)
      if (!isInRealtimeMode) { drv.setMode(DRV2605_MODE_REALTIME); isInRealtimeMode = true; }
      isMotorRunning = true; 
      for (int v = 0; v <= 255; v += 25) { drv.setRealtimeValue((uint8_t)v); delay(30); }
      for (int v = 255; v >= 0; v -= 25) { drv.setRealtimeValue((uint8_t)v); delay(30); }
      drv.setRealtimeValue(0); drv.setMode(0); isMotorRunning = false; isInRealtimeMode = false;
      response = "Rumble test ramp complete. Switched back to standby.";
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        EEPROM.write(RECEIVER_NAME_ADDR, newIndex); EEPROM.commit();
        response = "Receiver set to " + String(receiverNames[newIndex]) + ". Restarting...";
        httpServer.send(status_code, "text/plain", response); delay(100); ESP.restart();
      } else { status_code = 400; response = "Invalid receiver index."; }
    }
    else if (action == "set_gamma" && httpServer.hasArg("gamma")) {
      float newGamma = httpServer.arg("gamma").toFloat();
      if (newGamma >= 0.5f && newGamma <= 6.0f) {
        GAMMA = newGamma;
        EEPROM.put(GAMMA_ADDR, GAMMA); EEPROM.write(GAMMA_FLAG_ADDR, 0xA5); EEPROM.commit();
        response = "Gamma updated to " + String(GAMMA, 2) + ".";
      } else { status_code = 400; response = "Invalid gamma (must be between 0.5 and 6.0)."; }
    }
  }
  httpServer.send(status_code, "text/plain", response);
}

// ------------------------------------
// --- Setup and Loop ---
// ------------------------------------

void setup() {
  Serial.begin(115200);
  Wire.begin(); 

  // Initialize EEPROM and load settings
  EEPROM.begin(EEPROM_SIZE);
  assignedReceiverIndex = EEPROM.read(RECEIVER_NAME_ADDR);
  if (assignedReceiverIndex < 0 || assignedReceiverIndex >= NUM_RECEIVERS) {
    assignedReceiverIndex = 0; 
  }
  
  if (EEPROM.read(GAMMA_FLAG_ADDR) == 0xA5) {
      EEPROM.get(GAMMA_ADDR, GAMMA);
      if (isnan(GAMMA) || GAMMA < 0.5f || GAMMA > 6.0f) GAMMA = 2.2f;
  }
  
  // DRV2605 Initialization
  if (!drv.begin()) {
    Serial.println("DRV2605 not found. Check wiring.");
    while (1) delay(100);
  }
  
  // Configure DRV2605 for LRA motor and Blended operation
  drv.useLRA(); 
  drv.selectLibrary(LRA_LIBRARY); 
  drv.setMode(0); // Start in Standby mode

  // Wi-Fi Connection
  Serial.printf("Connecting to %s...", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // mDNS Setup
  String hostname = String(receiverNames[assignedReceiverIndex]);
  if (MDNS.begin(hostname.c_str())) {
    Serial.printf("mDNS responder started: %s.local\n", hostname.c_str());
  }

  // OSC Setup
  Udp.begin(CLIENT_LISTENER_PORT);
  Serial.printf("Listening for OSC on UDP port %u\n", CLIENT_LISTENER_PORT);

  // Web Server Setup
  httpServer.on("/", handleConfigPage);
  httpServer.on("/status", handleStatusJSON);
  httpServer.on("/action", HTTP_POST, handleConfigAction);
  httpServer.begin();
}

void loop() {
  handleRouterOscInput(); 
  checkRealtimeTimeout();
  httpServer.handleClient();
  MDNS.update();
}
