/*
  Sharkee_Haptics_Final_Expressive_v2.ino
  A fully refined sketch for the DRV2605, featuring:
  1. 10-Step Library Mapping for texture variety.
  2. Amplitude Scaling (Gain) for intensity realism.
  3. Complex Waveform Sequence for the strongest impact.
  4. Refined mapping logic for smoother control.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <Wire.h> 
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

// Haptic Logic 
const float MIN_INTENSITY_THRESHOLD = 0.05f; // Min input to activate haptics
const uint8_t LRA_LIBRARY = 6;              // Library for LRA actuators

// Array of 10 LRA Library 6 Effects (Low to High Intensity Textures)
const uint8_t MAX_EFFECT_ARRAY[] = {
    4,  // Soft Bump 1 (Lightest Tap)
    1,  // Strong Click 1 (Crisp Click)
    11, // Transition Ramp Up Short (Quick press/rub)
    18, // Transition Ramp Down Medium (Fading movement)
    37, // Buzz 1 (Medium Vibrate - Used in sequence)
    43, // Alert 1 (Strong Thump)
    47, // Alert 2 (Sharp Hit)
    49, // Alert 3 (Very Strong Hit - Used in sequence)
    64, // Transition Hum 1 (Sustained Low Rumble)
    66  // Deepest Rumble (Index 9 placeholder)
};
const int NUM_EFFECTS = sizeof(MAX_EFFECT_ARRAY) / sizeof(MAX_EFFECT_ARRAY[0]);

// Constants for the Complex Waveform Sequence (Triggered at Index 9)
const uint8_t FX_IMPACT_HIT = 49;     // Initial, sharpest impact
const uint8_t FX_RESIDUAL_HUM = 37;   // Residual shudder/ring
const uint8_t FX_PAUSE = 0xFF;        // Special effect ID for a pause
const uint8_t SEQUENCE_TRIGGER_INDEX = 9; // Index that triggers the complex sequence
const uint8_t MIN_GAIN = 15;            // Minimum amplitude gain (out of 100)
const uint8_t MAX_GAIN = 100;           // Maximum amplitude gain

// Device Naming and Configuration (EEPROM remains unchanged)
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
float GAMMA = 2.2f; // Gamma retained for EEPROM consistency

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

float getBatteryPercent() {
  const float V_FULL = 4.2f;
  const float V_EMPTY = 3.3f;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  float voltage = raw * (3.3f / 1023.0f);
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------
// --- Haptic Control: Core Logic ---
// ------------------------------------

void setMotorLibraryEffect(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  // 1. CRITICAL: INVERT INTENSITY (0.0=Far/Weak -> 1.0=Near/Strong)
  float invertedIntensity = 1.0f - intensity; 
  
  // --- A. STOP CONDITION ---
  if (invertedIntensity < MIN_INTENSITY_THRESHOLD) {
    drv.stop();
    drv.setMode(0); // Standby
    return;
  }
  
  // --- B. NORMALIZATION & MAPPING ---
  // Normalize intensity above the threshold to 0.0-1.0 range
  float normalizedIntensity = (invertedIntensity - MIN_INTENSITY_THRESHOLD) / (1.0f - MIN_INTENSITY_THRESHOLD);
  normalizedIntensity = constrain(normalizedIntensity, 0.0f, 1.0f);
  
  // Map 0.0-1.0 to array index 0 to NUM_EFFECTS - 1 (which is 0 to 9)
  int effectIndex = (int)roundf(normalizedIntensity * (NUM_EFFECTS - 1));
  
  // --- C. CALCULATE AMPLITUDE GAIN (for realism) ---
  // Scale normalized intensity (0.0-1.0) to gain (MIN_GAIN to MAX_GAIN)
  uint8_t gainRange = MAX_GAIN - MIN_GAIN;
  uint8_t gain_percent = (uint8_t)roundf(normalizedIntensity * gainRange) + MIN_GAIN;
  gain_percent = constrain(gain_percent, MIN_GAIN, MAX_GAIN); 

  // --- D. EXECUTE WAVEFORM ---
  drv.setMode(1); // Internal Trigger/Library Playback Mode
  drv.setAmplitude(gain_percent); // Apply gain to the entire sequence/effect

  if (effectIndex == SEQUENCE_TRIGGER_INDEX) {
    // 10th level: COMPLEX HIT SEQUENCE
    drv.setWaveform(0, FX_IMPACT_HIT);    
    drv.setWaveform(1, FX_PAUSE);         
    drv.setWaveform(2, FX_RESIDUAL_HUM);  
    drv.setWaveform(3, 0); // Terminate
    drv.go();
    
  } else {
    // Levels 0-8: SINGLE EFFECT from the array
    uint8_t effect_to_play = MAX_EFFECT_ARRAY[effectIndex];
    drv.setWaveform(0, effect_to_play); 
    drv.setWaveform(1, 0); // Terminate the sequence
    drv.go();
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
        // Handle both 0-255 and 0-100 integer ranges
        if (val > 1 && val <= 255) {
          intensity = (float)val / 255.0f;
        } else if (val >= 0 && val <= 100) {
          intensity = (float)val / 100.0f;
        }
      }
      setMotorLibraryEffect(intensity); 
    }
  }
}

// ------------------------------------
// --- Web Server Handlers ---
// ------------------------------------

void handleStatusJSON() {
  String json = "{";
  json += "\"role\":\"Client (Max Expressive Haptics)\","; 
  json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
  json += "\"hostname\":\"" + String(receiverNames[assignedReceiverIndex]) + ".local\",";
  json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
  json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"haptic_mode\":\"10-Step Library + Amplitude Scaling\"";
  json += "}";
  httpServer.send(200, "application/json", json);
}

// Helper to run a single waveform test
void runTestWaveform(uint8_t effect, uint8_t gain) {
    drv.setMode(1);
    drv.setAmplitude(gain);
    drv.setWaveform(0, effect);
    drv.setWaveform(1, 0);
    drv.go();
    delay(400); 
    drv.setMode(0); // Standby after test
}

void handleConfigAction() {
  String response = "OK";
  int status_code = 200;
  if (httpServer.method() != HTTP_POST) { status_code = 405; response = "Method not allowed."; } 
  else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "test_all") {
      // Test the first 9 effects with escalating gain
      for(int i = 0; i < NUM_EFFECTS - 1; i++) {
          // Calculate gain for testing, mimicking the internal logic
          float testNormalized = (float)i / (NUM_EFFECTS - 2);
          uint8_t gain = (uint8_t)roundf(testNormalized * (MAX_GAIN - MIN_GAIN)) + MIN_GAIN;
          runTestWaveform(MAX_EFFECT_ARRAY[i], gain);
          delay(100); 
      }
      // Test the complex sequence at max gain (100)
      drv.setMode(1); 
      drv.setAmplitude(MAX_GAIN);
      drv.setWaveform(0, FX_IMPACT_HIT);    
      drv.setWaveform(1, FX_PAUSE);         
      drv.setWaveform(2, FX_RESIDUAL_HUM);  
      drv.setWaveform(3, 0);                
      drv.go();
      delay(800);
      drv.setMode(0);
      
      response = "Tested all 10 levels (9 single + 1 sequence) with amplitude ramping. Switched back to standby.";
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
        response = "Gamma is not used in Library Effects mode.";
        status_code = 200; 
    }
  }
  httpServer.send(status_code, "text/plain", response);
}

void handleConfigPage() {
    String html = "<html><head><title>Haptic Device Config</title>";
    html += "<style>body{font-family:Arial;background-color:#222;color:#eee;} .container{max-width:400px;margin:50px auto;padding:20px;background-color:#333;border-radius:8px;} h2{color:#4CAF50;} label, select, input, button{display:block;width:100%;margin-bottom:10px;padding:8px;border-radius:4px;} select, input{background-color:#444;color:#eee;border:1px solid #555;} button{background-color:#9C27B0;color:white;border:none;cursor:pointer;} .status{margin-top:15px;padding:10px;background-color:#444;border-radius:4px;}</style>";
    html += "</head><body><div class='container'><h2>Max Expressive Haptics Config</h2>";
    html += "<p>Mode: <b>10-Step Library + Amplitude Scaling</b></p>";
    html += "<p>Hostname: <b>" + String(receiverNames[assignedReceiverIndex]) + ".local</b></p>";
    html += "<p>IP Address: <b>" + WiFi.localIP().toString() + "</b></p>";
    html += "<p>Battery: <b>" + String((int)getBatteryPercent()) + "%</b></p>";
    
    // Test Button (Test all 10 library effects)
    html += "<form action='/action' method='post'><input type='hidden' name='action' value='test_all'><button type='submit'>Run Full Ramping Test Sequence</button></form>";

    // Set Receiver Dropdown
    html += "<form action='/action' method='post'><label for='receiver_index'>Set Receiver (Hostname)</label><select id='receiver_index' name='index'>";
    for(int i=0; i<NUM_RECEIVERS; i++) {
        html += "<option value='" + String(i) + "'" + (i == assignedReceiverIndex ? " selected" : "") + ">" + receiverNames[i] + "</option>";
    }
    html += "</select><input type='hidden' name='action' value='set_receiver'><button type='submit'>Change Receiver & Restart</button></form>";

    html += "</div></body></html>";
    httpServer.send(200, "text/html", html);
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
  
  // DRV2605 Configuration: CALIBRATION is essential for LRA
  // Auto-calibrate should be run once per actuator. 
  // For production, you would save these values, but for a dev sketch:
  // drv.setMode(6); // Auto-calibration mode
  // drv.go();
  // while (!drv.isCalibrating());
  // while (drv.isCalibrating());
  // drv.setMode(0); // Back to standby
  
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
  httpServer.handleClient();
  MDNS.update();
}
