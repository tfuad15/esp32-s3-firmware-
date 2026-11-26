#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>

#ifndef RGB_BUILTIN
  #define RGB_BUILTIN 48
#endif

const char* AP_SSID = "ESP32-S3-HUB";
const char* AP_PASS = "Fuuuad15";

AsyncWebServer server(80);
uint8_t* psramBuffer = nullptr;
size_t psramAllocatedMB = 0;

const char index_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ESP32-S3 N16R8 Studio</title>
  <style>
    :root {
      --bg: #090d16;
      --card: rgba(20, 28, 46, 0.85);
      --border: rgba(56, 189, 248, 0.22);
      --accent: #38bdf8;
      --success: #10b981;
      --danger: #ef4444;
      --text: #f1f5f9;
      --muted: #94a3b8;
    }
    * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: radial-gradient(circle at top right, #1e1b4b, var(--bg) 60%);
      color: var(--text);
      margin: 0;
      padding: 14px 14px 80px 14px;
    }
    .header { display: flex; justify-content: space-between; align-items: center; padding: 10px 4px 18px 4px; }
    .title { font-size: 20px; font-weight: 800; background: linear-gradient(90deg, #38bdf8, #a855f7); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    .badge { background: rgba(16, 185, 129, 0.15); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.4); padding: 4px 10px; border-radius: 20px; font-size: 11px; font-weight: 700; display: flex; align-items: center; gap: 6px; }
    .dot { width: 8px; height: 8px; background: var(--success); border-radius: 50%; box-shadow: 0 0 8px var(--success); }

    .tabs { display: flex; gap: 6px; background: rgba(15, 23, 42, 0.9); padding: 5px; border-radius: 12px; border: 1px solid var(--border); margin-bottom: 16px; position: sticky; top: 8px; z-index: 10; }
    .tab { flex: 1; text-align: center; padding: 9px 4px; border-radius: 8px; font-size: 12px; font-weight: 700; color: var(--muted); cursor: pointer; }
    .tab.active { background: linear-gradient(135deg, #0284c7, #4f46e5); color: #fff; }

    .section { display: none; }
    .section.active { display: block; }

    .card { background: var(--card); border: 1px solid var(--border); border-radius: 16px; padding: 16px; margin-bottom: 14px; box-shadow: 0 8px 24px rgba(0,0,0,0.35); }
    .card h3 { margin: 0 0 12px 0; font-size: 14px; text-transform: uppercase; letter-spacing: 0.8px; color: var(--accent); display: flex; justify-content: space-between; align-items: center; }

    .stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .stat-box { background: rgba(9, 13, 22, 0.6); border: 1px solid rgba(255,255,255,0.06); padding: 10px; border-radius: 10px; }
    .stat-label { font-size: 11px; color: var(--muted); }
    .stat-val { font-size: 15px; font-weight: 700; margin-top: 3px; color: #fff; }

    .bar-wrap { background: #0f172a; height: 8px; border-radius: 99px; overflow: hidden; margin-top: 8px; }
    .bar-fill { height: 100%; background: linear-gradient(90deg, #38bdf8, #818cf8); width: 0%; transition: width 0.4s ease; }

    .btn-row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
    button { flex: 1; min-width: 80px; background: linear-gradient(135deg, #0ea5e9, #3b82f6); color: #fff; border: none; padding: 11px 12px; border-radius: 10px; font-weight: 700; font-size: 13px; cursor: pointer; }
    button:active { transform: scale(0.96); }
    button.sec { background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.12); color: var(--text); }
    button.danger { background: linear-gradient(135deg, #ef4444, #b91c1c); }
    button.success { background: linear-gradient(135deg, #10b981, #059669); }

    .pin-grid { display: grid; grid-template-columns: repeat(6, 1fr); gap: 6px; margin-bottom: 12px; }
    .pin-chip { background: rgba(15, 23, 42, 0.9); border: 1px solid rgba(255,255,255,0.12); color: var(--muted); padding: 9px 0; text-align: center; border-radius: 8px; font-size: 12px; font-weight: 700; cursor: pointer; }
    .pin-chip.selected { background: var(--accent); color: #090d16; border-color: #fff; }

    .rgb-presets { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; margin-top: 10px; }
    .swatch { height: 34px; border-radius: 8px; border: 2px solid rgba(255,255,255,0.2); cursor: pointer; }
    input[type="range"] { width: 100%; accent-color: var(--accent); margin: 10px 0; }
    input[type="color"] { width: 100%; height: 42px; border: none; border-radius: 10px; background: none; cursor: pointer; }
    input[type="file"] { width: 100%; background: rgba(0,0,0,0.3); padding: 10px; border-radius: 10px; border: 1px dashed var(--accent); color: var(--muted); font-size: 12px; }

    .console { background: #05080f; border: 1px solid #1e293b; border-radius: 10px; padding: 10px; font-family: monospace; font-size: 12px; color: #34d399; max-height: 160px; overflow-y: auto; white-space: pre-wrap; margin-top: 10px; }
    .toast { position: fixed; bottom: 18px; left: 50%; transform: translateX(-50%) translateY(80px); background: #1e293b; color: #fff; border: 1px solid var(--accent); padding: 10px 18px; border-radius: 30px; font-size: 13px; font-weight: 600; transition: transform 0.25s ease; z-index: 99; }
    .toast.show { transform: translateX(-50%) translateY(0); }
  </style>
</head>
<body>

  <div class="header">
    <div>
      <div class="title">ESP32-S3 N16R8</div>
      <div style="font-size: 11px; color: var(--muted);">16MB Quad Flash &bull; 8MB Octal PSRAM</div>
    </div>
    <div class="badge"><div class="dot"></div> ONLINE</div>
  </div>

  <div class="tabs">
    <div class="tab active" onclick="switchTab('dash', this)">System</div>
    <div class="tab" onclick="switchTab('gpio', this)">GPIO &amp; RGB</div>
    <div class="tab" onclick="switchTab('bus', this)">I2C / WiFi</div>
    <div class="tab" onclick="switchTab('ota', this)">OTA Flash</div>
  </div>

  <div id="dash" class="section active">
    <div class="card">
      <h3>Hardware Telemetry <span id="uptime" style="font-size:11px;color:var(--muted)">0s</span></h3>
      <div class="stat-grid">
        <div class="stat-box"><div class="stat-label">CPU &amp; Temp</div><div class="stat-val" id="cpu">--</div></div>
        <div class="stat-box"><div class="stat-label">Flash Memory</div><div class="stat-val" id="flash">--</div></div>
        <div class="stat-box"><div class="stat-label">Internal SRAM</div><div class="stat-val" id="sram">--</div></div>
        <div class="stat-box"><div class="stat-label">Octal PSRAM</div><div class="stat-val" id="psram">--</div></div>
      </div>
      <div style="margin-top:12px; font-size:11px; color:var(--muted); display:flex; justify-content:space-between;">
        <span>PSRAM Usage</span><span id="psramPct">0%</span>
      </div>
      <div class="bar-wrap"><div class="bar-fill" id="psramBar"></div></div>
      <div class="btn-row">
        <button onclick="allocPsram(4)">Alloc 4MB PSRAM</button>
        <button class="sec" onclick="allocPsram(0)">Free PSRAM</button>
        <button class="sec" onclick="refreshStats()">Refresh</button>
      </div>
    </div>

    <div class="card">
      <h3>Active Radios</h3>
      <div class="stat-grid">
        <div class="stat-box"><div class="stat-label">Wi-Fi AP IP</div><div class="stat-val">192.168.4.1</div></div>
        <div class="stat-box"><div class="stat-label">BLE Beacon</div><div class="stat-val" style="color:var(--success)">Broadcasting</div></div>
      </div>
    </div>
  </div>

  <div id="gpio" class="section">
    <div class="card">
      <h3>Onboard RGB LED (GPIO 48)</h3>
      <input type="color" id="rgbPicker" value="#38bdf8" onchange="sendRGB(this.value)">
      <div class="rgb-presets">
        <div class="swatch" style="background:#ff0000" onclick="sendRGB('#ff0000')"></div>
        <div class="swatch" style="background:#00ff00" onclick="sendRGB('#00ff00')"></div>
        <div class="swatch" style="background:#0066ff" onclick="sendRGB('#0066ff')"></div>
        <div class="swatch" style="background:#a855f7" onclick="sendRGB('#a855f7')"></div>
        <div class="swatch" style="background:#000000" onclick="sendRGB('#000000')"></div>
      </div>
    </div>

    <div class="card">
      <h3>Select Safe GPIO Pin <span id="selPinLabel" style="color:#fff">GPIO 1</span></h3>
      <div class="pin-grid" id="pinGrid"></div>
      <div class="btn-row">
        <button class="success" onclick="setPin('high')">HIGH (3.3V)</button>
        <button class="danger" onclick="setPin('low')">LOW (0V)</button>
        <button class="sec" onclick="setPin('read')">Read &amp; ADC</button>
      </div>
      <div style="margin-top:14px; font-size:12px; color:var(--muted); display:flex; justify-content:space-between;">
        <span>PWM Duty Cycle</span><span id="pwmVal">128 / 255</span>
      </div>
      <input type="range" min="0" max="255" value="128" oninput="document.getElementById('pwmVal').innerText=this.value+' / 255'" onchange="setPWM(this.value)">
      <div class="console" id="gpioLog">Tap a pin above and choose an action...</div>
    </div>
  </div>

  <div id="bus" class="section">
    <div class="card">
      <h3>I2C Bus Scanner (SDA: 8 &bull; SCL: 9)</h3>
      <button onclick="runScan('/i2c', 'i2cLog')">Scan Connected I2C Sensors</button>
      <div class="console" id="i2cLog">Ready to scan addresses 0x01 - 0x7E...</div>
    </div>

    <div class="card">
      <h3>2.4GHz Wi-Fi Spectrum</h3>
      <button onclick="runScan('/wifi', 'wifiLog')">Scan Nearby Access Points</button>
      <div class="console" id="wifiLog">Tap scan to discover local networks...</div>
    </div>
  </div>

  <div id="ota" class="section">
    <div class="card">
      <h3>Wireless Firmware Update (OTA)</h3>
      <p style="font-size:12px; color:var(--muted); line-height:1.5; margin-top:0;">
        Update your ESP32-S3 wirelessly straight from Android Chrome! Select your newly compiled <b>firmware.bin</b> file below.
      </p>
      <input type="file" id="otaFile" accept=".bin">
      <div class="bar-wrap" style="margin-top:12px;"><div class="bar-fill" id="otaBar"></div></div>
      <div class="btn-row" style="margin-top:12px;">
        <button onclick="uploadOTA()">Upload &amp; Flash Firmware.bin</button>
      </div>
      <div class="console" id="otaLog">Waiting for firmware.bin selection...</div>
    </div>
  </div>

  <div class="toast" id="toast">Command Sent</div>

<script>
  const safePins = "1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,47".split(",");
  let activePin = "1";

  const grid = document.getElementById('pinGrid');
  safePins.forEach(p => {
    const d = document.createElement('div');
    d.className = 'pin-chip' + (p === activePin ? ' selected' : '');
    d.innerText = p;
    d.onclick = () => {
      document.querySelectorAll('.pin-chip').forEach(el => el.classList.remove('selected'));
      d.classList.add('selected');
      activePin = p;
      document.getElementById('selPinLabel').innerText = 'GPIO ' + p;
    };
    grid.appendChild(d);
  });

  function switchTab(id, el) {
    document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.getElementById(id).classList.add('active');
    el.classList.add('active');
  }

  function showToast(msg) {
    const t = document.getElementById('toast');
    t.innerText = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 2000);
  }

  async function refreshStats() {
    try {
      const res = await fetch('/stats');
      const data = await res.json();
      document.getElementById('cpu').innerText = data.freq + 'MHz | ' + data.temp + 'C';
      document.getElementById('flash').innerText = data.flash + ' MB';
      document.getElementById('sram').innerText = data.freeHeap + ' / ' + data.totalHeap + ' KB';
      document.getElementById('psram').innerText = data.freePsram + ' / ' + data.totalPsram + ' KB';
      document.getElementById('uptime').innerText = 'Up: ' + data.uptime + 's';
      const usedPct = data.totalPsram > 0 ? Math.round(((data.totalPsram - data.freePsram) / data.totalPsram) * 100) : 0;
      document.getElementById('psramPct').innerText = usedPct + '%';
      document.getElementById('psramBar').style.width = usedPct + '%';
    } catch (e) {}
  }

  async function allocPsram(mb) {
    await fetch('/psram?mb=' + mb);
    showToast(mb > 0 ? 'Allocated ' + mb + 'MB PSRAM' : 'PSRAM Freed');
    refreshStats();
  }

  async function sendRGB(hex) {
    document.getElementById('rgbPicker').value = hex;
    const r = parseInt(hex.substr(1,2), 16);
    const g = parseInt(hex.substr(3,2), 16);
    const b = parseInt(hex.substr(5,2), 16);
    await fetch('/rgb?r=' + r + '&g=' + g + '&b=' + b);
    showToast('RGB LED: ' + r + ', ' + g + ', ' + b);
  }

  async function setPin(act) {
    const r = await fetch('/gpio?p=' + activePin + '&act=' + act);
    const txt = await r.text();
    document.getElementById('gpioLog').innerText = txt;
    showToast('GPIO ' + activePin + ' -> ' + act.toUpperCase());
  }

  async function setPWM(val) {
    const r = await fetch('/pwm?p=' + activePin + '&val=' + val);
    document.getElementById('gpioLog').innerText = await r.text();
  }

  async function runScan(url, logId) {
    document.getElementById(logId).innerText = 'Scanning hardware bus...';
    const r = await fetch(url);
    document.getElementById(logId).innerText = await r.text();
    showToast('Scan Complete');
  }

  function uploadOTA() {
    const fileInput = document.getElementById('otaFile');
    if (!fileInput.files.length) { showToast('Pick a .bin file first!'); return; }
    const file = fileInput.files.item(0);
    const formData = new FormData();
    formData.append('update', file);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const pct = Math.round((e.loaded / e.total) * 100);
        document.getElementById('otaBar').style.width = pct + '%';
        document.getElementById('otaLog').innerText = 'Uploading firmware: ' + pct + '%';
      }
    };
    xhr.onload = () => {
      if (xhr.status === 200) {
        document.getElementById('otaLog').innerText = 'Success! ESP32-S3 is rebooting... Reconnect Wi-Fi in 5 seconds.';
        showToast('OTA Flash Complete!');
      } else {
        document.getElementById('otaLog').innerText = 'OTA Failed: ' + xhr.responseText;
      }
    };
    xhr.send(formData);
  }

  refreshStats();
  setInterval(refreshStats, 3000);
</script>
</body>
</html>
)=====";

void setup() {
  Serial.begin(115200);
  neopixelWrite(RGB_BUILTIN, 0, 24, 48);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  BLEDevice::init("ESP32-S3-N16R8");
  BLEServer *pServer = BLEDevice::createServer();
  BLEDevice::getAdvertising()->start();

  Wire.begin(8, 9);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"freq\":" + String(ESP.getCpuFreqMHz()) + ",";
    json += "\"temp\":" + String(temperatureRead(), 1) + ",";
    json += "\"flash\":" + String(ESP.getFlashChipSize() / (1024 * 1024)) + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap() / 1024) + ",";
    json += "\"totalHeap\":" + String(ESP.getHeapSize() / 1024) + ",";
    json += "\"freePsram\":" + String(ESP.getFreePsram() / 1024) + ",";
    json += "\"totalPsram\":" + String(ESP.getPsramSize() / 1024) + ",";
    json += "\"uptime\":" + String(millis() / 1000);
    json += "}";
    request->send(200, "application/json", json);
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
    request->send(200, "text/plain", "OK");
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
      request->send(200, "text/plain", "[GPIO " + String(p) + "] Output set to HIGH (3.3V)");
    } else if (act == "low") {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);
      request->send(200, "text/plain", "[GPIO " + String(p) + "] Output set to LOW (0.0V)");
    } else {
      pinMode(p, INPUT);
      int dig = digitalRead(p);
      int adc = analogRead(p);
      float volts = (adc / 4095.0) * 3.3;
      request->send(200, "text/plain", "[GPIO " + String(p) + "] Logic: " + String(dig ? "HIGH (1)" : "LOW (0)") + " | ADC: " + String(adc) + "/4095 (~" + String(volts, 2) + "V)");
    }
  });

  server.on("/pwm", HTTP_GET, [](AsyncWebServerRequest *request) {
    int p = request->getParam("p")->value().toInt();
    int val = request->getParam("val")->value().toInt();
    analogWrite(p, val);
    int pct = map(val, 0, 255, 0, 100);
    request->send(200, "text/plain", "[GPIO " + String(p) + "] PWM Duty: " + String(val) + "/255 (" + String(pct) + "%)");
  });

  server.on("/i2c", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "I2C Bus Scan (SDA=8, SCL=9):\n";
    int count = 0;
    for (byte addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        out += "Found device at 0x" + String(addr, HEX) + "\n";
        count++;
      }
    }
    if (count == 0) out += "No I2C devices detected.";
    request->send(200, "text/plain", out);
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    String out = "Discovered " + String(n) + " Wi-Fi networks:\n";
    for (int i = 0; i < n; ++i) {
      out += String(i + 1) + ". " + WiFi.SSID(i) + " [" + String(WiFi.RSSI(i)) + " dBm]\n";
    }
    request->send(200, "text/plain", out);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool shouldReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(shouldReboot ? 200 : 500, "text/plain", shouldReboot ? "OK" : "FAIL");
    response->addHeader("Connection", "close");
    request->send(response);
    if (shouldReboot) {
      delay(500);
      ESP.restart();
    }
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    }
    if (!Update.hasError()) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }
    if (final) {
      if (!Update.end(true)) {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
}

void loop() {
  delay(100);
}