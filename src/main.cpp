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

const char* AP_SSID = "ESP32-S3-N16R8-HUB";
const char* AP_PASS = "12345678";

AsyncWebServer server(80);
uint8_t* psramBuffer = nullptr;
size_t psramAllocatedMB = 0;

const char index_html[] PROGMEM = R"rawliteral(
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
      --accent2: #818cf8;
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
    .header {
      display: flex; justify-content: space-between; align-items: center;
      padding: 10px 4px 18px 4px;
    }
    .title { font-size: 20px; font-weight: 800; background: linear-gradient(90deg, #38bdf8, #a855f7); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    .badge { background: rgba(16, 185, 129, 0.15); color: var(--success); border: 1px solid rgba(16, 185, 129, 0.4); padding: 4px 10px; border-radius: 20px; font-size: 11px; font-weight: 700; display: flex; align-items: center; gap: 6px; }
    .dot { width: 8px; height: 8px; background: var(--success); border-radius: 50%; box-shadow: 0 0 8px var(--success); animation: pulse 1.5s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }

    .tabs { display: flex; gap: 6px; background: rgba(15, 23, 42, 0.9); padding: 5px; border-radius: 12px; border: 1px solid var(--border); margin-bottom: 16px; position: sticky; top: 8px; z-index: 10; backdrop-filter: blur(10px); }
    .tab { flex: 1; text-align: center; padding: 9px 4px; border-radius: 8px; font-size: 12px; font-weight: 700; color: var(--muted); cursor: pointer; transition: 0.2s; }
    .tab.active { background: linear-gradient(135deg, #0284c7, #4f46e5); color: #fff; box-shadow: 0 4px 12px rgba(56, 189, 248, 0.3); }

    .section { display: none; animation: fadeIn 0.25s ease; }
    .section.active { display: block; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: translateY(0); } }

    .card { background: var(--card); border: 1px solid var(--border); border-radius: 16px; padding: 16px; margin-bottom: 14px; box-shadow: 0 8px 24px rgba(0,0,0,0.35); backdrop-filter: blur(12px); }
    .card h3 { margin: 0 0 12px 0; font-size: 14px; text-transform: uppercase; letter-spacing: 0.8px; color: var(--accent); display: flex; justify-content: space-between; align-items: center; }

    .stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .stat-box { background: rgba(9, 13, 22, 0.6); border: 1px solid rgba(255,255,255,0.06); padding: 10px; border-radius: 10px; }
    .stat-label { font-size: 11px; color: var(--muted); }
    .stat-val { font-size: 15px; font-weight: 700; margin-top: 3px; color: #fff; }

    .bar-wrap { background: #0f172a; height: 8px; border-radius: 99px; overflow: hidden; margin-top: 8px; }
    .bar-fill { height: 100%; background: linear-gradient(90deg, #38bdf8, #818cf8); width: 0%; transition: width 0.4s ease; }

    .btn-row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
    button { flex: 1; min-width: 80px; background: linear-gradient(135deg, #0ea5e9, #3b82f6); color: #fff; border: none; padding: 11px 12px; border-radius: 10px; font-weight: 700; font-size: 13px; cursor: pointer; transition: transform 0.1s, opacity 0.2s; }
    button:active { transform: scale(0.96); }
    button.sec { background: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.12); color: var(--text); }
    button.danger { background: linear-gradient(135deg, #ef4444, #b91c1c); }
    button.success { background: linear-gradient(135deg, #10b981, #059669); }

    .pin-grid { display: grid; grid-template-columns: repeat(6, 1fr); gap: 6px; margin-bottom: 12px; }
    .pin-chip { background: rgba(15, 23, 42, 0.9); border: 1px solid rgba(255,255,255,0.12); color: var(--muted); padding: 9px 0; text-align: center; border-radius: 8px; font-size: 12px; font-weight: 700; cursor: pointer; }
    .pin-chip.selected { background: var(--accent); color: #090d16; border-color: #fff; box-shadow: 0 0 12px rgba(56,189,248,0.5); }

    .rgb-presets { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; margin-top: 10px; }
    .swatch { height: 34px; border-radius: 8px; border: 2px solid rgba(255,255,255,0.2); cursor: pointer; }
    input[type="range"] { width: 100%; accent-color: var(--accent); margin: 10px 0; }
    input[type="color"] { width: 100%; height: 42px; border: none; border-radius: 10px; background: none; cursor: pointer; }
    input[type="file"] { width: 100%; background: rgba(0,0,0,0.3); padding: 10px; border-radius: 10px; border: 1px dashed var(--accent); color: var(--muted); font-size: 12px; }

    .console { background: #05080f; border: 1px solid #1e293b; border-radius: 10px; padding: 10px; font-family: monospace; font-size: 12px; color: #34d399; max-height: 160px; overflow-y: auto; white-space: pre-wrap; margin-top: 10px; }
    .toast { position: fixed; bottom: 18px; left: 50%; transform: translateX(-50%) translateY(80px); background: #1e293b; color: #fff; border: 1px solid var(--accent); padding: 10px 18px; border-radius: 30px; font-size: 13px; font-weight: 600; box-shadow: 0 10px 25px rgba(0,0,0,0.6); transition: transform 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275); z-index: 99; }
    .toast.show { transform: translateX(-50%) translateY(0); }
  </style>
</head>
<body>

  <div class="header">
    <div>
      <div class="title">ESP32-S3 N16R8</div>
      <div style="font-size: 11px; color: var(--muted);">16MB Quad Flash • 8MB Octal PSRAM</div>
    </div>
    <div class="badge"><div class="dot"></div> ONLINE</div>
  </div>

  <div class="tabs">
    <div class="tab active" onclick="switchTab('dash', this)">System</div>
    <div class="tab" onclick="switchTab('gpio', this)">GPIO & RGB</div>
    <div class="tab" onclick="switchTab('bus', this)">I2C / WiFi</div>
    <div class="tab" onclick="switchTab('ota', this)">OTA Flash</div>
  </div>

  <div id="dash" class="section active">
    <div class="card">
      <h3>Hardware Telemetry <span id="uptime" style="font-size:11px;color:var(--muted)">0s</span></h3>
      <div class="stat-grid">
        <div class="stat-box"><div class="stat-label">CPU & Temp</div><div class="stat-val" id="cpu">--</div></div>
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
        <button class="sec" onclick="refreshStats()">↻</button>
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
        <button class="sec" onclick="setPin('read')">Read & ADC</button>
      </div>
      <div style="margin-top:14px; font-size:12px; color:var(--muted); display:flex; justify-content:space-between;">
        <span>PWM Duty Cycle</span><span id="pwmVal">128 / 255</span>
      </div>
      <input type="range" min="0" max="255" value="128" oninput="document.getElementById('pwmVal').innerText=this.value+' / 255'" onchange="setPWM(this.value)">
      <div class="console" id="gpioLog">Tap a pin above and choose an action...</div>
    </div>
  </div>

