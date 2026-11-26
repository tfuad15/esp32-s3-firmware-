#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>

#ifndef RGB_BUILTIN
  #define RGB_BUILTIN 48
#endif

// Wi-Fi Access Point Credentials (Connect from your Android Wi-Fi settings)
const char* AP_SSID = "ESP32-S3-N16R8-HUB";
const char* AP_PASS = "12345678";

AsyncWebServer server(80);
uint8_t* psramBuffer = nullptr;
size_t psramAllocatedMB = 0;

// Safe user GPIOs on ESP32-S3-N16R8 (Excludes Flash/PSRAM pins 26-37 and USB 19-20)
const int safePins[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47};
const int numSafePins = sizeof(safePins) / sizeof(safePins[0]);

// Embedded Mobile-Friendly Web UI
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 N16R8 Control Hub</title>
  <style>
    body { font-family: system-ui, sans-serif; background: #121212; color: #eee; margin: 0; padding: 15px; }
    h2 { color: #00e5ff; margin-top: 0; }
    .card { background: #1e1e1e; border-radius: 10px; padding: 15px; margin-bottom: 15px; border: 1px solid #333; }
    button { background: #00e5ff; color: #000; border: none; padding: 10px 14px; border-radius: 6px; font-weight: bold; margin: 4px 2px; cursor: pointer; }
    button.off { background: #444; color: #fff; }
    input, select { background: #2c2c2c; color: #fff; border: 1px solid #555; padding: 8px; border-radius: 6px; margin: 4px 0; }
    pre { background: #000; padding: 10px; border-radius: 6px; overflow-x: auto; color: #0f0; font-size: 13px; }
  </style>
</head>
<body>
  <h2>ESP32-S3 N16R8 Master Hub</h2>

  <div class="card">
    <h3>1. System & 8MB PSRAM</h3>
    <pre id="sys">Loading hardware stats...</pre>
    <button onclick="api('/psram?mb=4')">Alloc 4MB PSRAM</button>
    <button class="off" onclick="api('/psram?mb=0')">Free PSRAM</button>
    <button onclick="loadSys()">Refresh</button>
  </div>

  <div class="card">
    <h3>2. Onboard RGB LED (GPIO 48)</h3>
    <input type="color" id="rgb" value="#00e5ff" onchange="setRGB(this.value)">
    <button onclick="setRGB('#ff0000')">Red</button>
    <button onclick="setRGB('#00ff00')">Green</button>
    <button onclick="setRGB('#0000ff')">Blue</button>
    <button class="off" onclick="setRGB('#000000')">Off</button>
  </div>

  <div class="card">
    <h3>3. GPIO Digital / PWM / ADC</h3>
    Pin: <select id="pin"></select><br>
    <button onclick="gpio('high')">HIGH (3.3V)</button>
    <button class="off" onclick="gpio('low')">LOW (0V)</button>
    <button onclick="gpio('read')">Read Digital & ADC</button><br>
    PWM Duty (0-255): <input type="range" min="0" max="255" value="128" onchange="pwm(this.value)">
    <pre id="gpioOut">Ready.</pre>
  </div>

  <div class="card">
    <h3>4. I2C Bus Scanner (SDA=8, SCL=9)</h3>
    <button onclick="scan('/i2c', 'i2cOut')">Scan I2C Devices</button>
    <pre id="i2cOut">Connect sensors to GPIO 8 (SDA) & GPIO 9 (SCL)</pre>
  </div>

  <div class="card">
    <h3>5. Wi-Fi Spectrum Scanner</h3>
    <button onclick="scan('/wifi', 'wifiOut')">Scan Nearby Networks</button>
    <pre id="wifiOut">Tap scan to list 2.4GHz networks...</pre>
  </div>

<script>
  const pins =;
  const sel = document.getElementById('pin');
  pins.forEach(p => { let o = document.createElement('option'); o.value = p; o.text = 'GPIO ' + p; sel.add(o); });

  async function api(url) { let r = await fetch(url); return await r.text(); }
  async function loadSys() { document.getElementById('sys').innerText = await api('/sys'); }
  function setRGB(hex) {
    let r = parseInt(hex.substr(1,2),16), g = parseInt(hex.substr(3,2),16), b = parseInt(hex.substr(5,2),16);
    api(`/rgb?r=${r}&g=${g}&b=${b}`);
  }
  async function gpio(act) {
    let p = sel.value;
    document.getElementById('gpioOut').innerText = await api(`/gpio?p=${p}&act=${act}`);
  }
  async function pwm(val) {
    let p = sel.value;
    document.getElementById('gpioOut').innerText = await api(`/pwm?p=${p}&val=${val}`);
  }
  async function scan(url, target) {
    document.getElementById(target).innerText = 'Scanning...';
    document.getElementById(target).innerText = await api(url);
  }
  loadSys();
</script>
</body>
</html>
)rawliteral";

String getSystemInfo() {
  String s = "";
  s += "Chip: " + String(ESP.getChipModel()) + " (2 Cores @ " + String(ESP.getCpuFreqMHz()) + " MHz)\n";
  s += "Internal Temp: " + String(temperatureRead(), 1) + " C\n";
  s += "Flash Size: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB\n";
  s += "Free Heap (SRAM): " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  if (psramFound()) {
    s += "PSRAM Total: " + String(ESP.getPsramSize() / (1024 * 1024)) + " MB\n";
    s += "PSRAM Free:  " + String(ESP.getFreePsram() / 1024) + " KB (Test Alloc: " + String(psramAllocatedMB) + " MB)\n";
  } else {
    s += "PSRAM: NOT DETECTED\n";
  }
  s += "BLE Status: Advertising as 'ESP32-S3-N16R8'\n";
  s += "AP IP Address: " + WiFi.softAPIP().toString();
  return s;
}

void setup() {
  Serial.begin(115200);
  neopixelWrite(RGB_BUILTIN, 0, 16, 32);

  // 1. Start Wi-Fi Access Point + STA mode for scanning
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  // 2. Start Bluetooth Low Energy (BLE) Beacon
  BLEDevice::init("ESP32-S3-N16R8");
  BLEServer *pServer = BLEDevice::createServer();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->start();

  // 3. Initialize Default I2C Bus
  Wire.begin(8, 9);

  // 4. Web Server Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/sys", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", getSystemInfo());
  });

  server.on("/psram", HTTP_GET, [](AsyncWebServerRequest *request) {
    int mb = request->hasParam("mb") ? request->getParam("mb")->value().toInt() : 0;
    if (psramBuffer != nullptr) {
      heap_caps_free(psramBuffer);
      psramBuffer = nullptr;
      psramAllocatedMB = 0;
    }
    if (mb > 0 && psramFound()) {
      psramBuffer = (uint8_t*)heap_caps_malloc(mb * 1024 * 1024, MALLOC_CAP_SPIRAM);
      if (psramBuffer) psramAllocatedMB = mb;
    }
    request->send(200, "text/plain", getSystemInfo());
  });

  server.on("/rgb", HTTP_GET, [](AsyncWebServerRequest *request) {
    int r = request->getParam("r")->value().toInt();
    int g = request->getParam("g")->value().toInt();
    int b = request->getParam("b")->value().toInt();
    neopixelWrite(RGB_BUILTIN, r, g, b);
    request->send(200, "text/plain", "OK");
  });

  server.on("/gpio", HTTP_GET, [](AsyncWebServerRequest *request) {
    int p = request->getParam("p")->value().toInt();
    String act = request->getParam("act")->value();
    if (act == "high") {
      pinMode(p, OUTPUT);
      digitalWrite(p, HIGH);
      request->send(200, "text/plain", "GPIO " + String(p) + " set HIGH (3.3V)");
    } else if (act == "low") {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);
      request->send(200, "text/plain", "GPIO " + String(p) + " set LOW (0V)");
    } else {
      pinMode(p, INPUT);
      int dig = digitalRead(p);
      int adc = analogRead(p);
      request->send(200, "text/plain", "GPIO " + String(p) + " -> Digital: " + String(dig) + " | Raw ADC (0-4095): " + String(adc));
    }
  });

  server.on("/pwm", HTTP_GET, [](AsyncWebServerRequest *request) {
    int p = request->getParam("p")->value().toInt();
    int val = request->getParam("val")->value().toInt();
    analogWrite(p, val);
    request->send(200, "text/plain", "GPIO " + String(p) + " PWM set to " + String(val) + "/255");
  });

  server.on("/i2c", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "I2C Scan Results (SDA=8, SCL=9):\n";
    int count = 0;
    for (byte addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        out += "- Device found at 0x" + String(addr, HEX) + "\n";
        count++;
      }
    }
    if (count == 0) out += "No I2C devices found.";
    request->send(200, "text/plain", out);
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    String out = "Found " + String(n) + " networks:\n";
    for (int i = 0; i < n; ++i) {
      out += String(i + 1) + ": " + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)\n";
    }
    request->send(200, "text/plain", out);
  });

  server.begin();
}

void loop() {
  // AsyncWebServer handles everything in the background
  delay(100);
}
