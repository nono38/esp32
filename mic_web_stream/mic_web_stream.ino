/*
 * ============================================================
 *  DUAL MICRO — T-Call SIM800L (ESP32)
 *  Broches I2S corrigées pour éviter les conflits de boot
 *
 *  CÂBLAGE MICRO ANALOGIQUE (électret eMagTech) :
 *    Fil rouge +  →  résistance 4,7kΩ  →  3.3V
 *    Fil rouge +  →  condensateur 100nF  →  GPIO34
 *    Fil noir  −  →  GND
 *
 *  CÂBLAGE MICRO NUMÉRIQUE (INMP441) :
 *    VCC  →  3.3V
 *    GND  →  GND (même broche que L/R)
 *    SCK  →  GPIO13   (était GPIO14 — broche de boot, retiré)
 *    WS   →  GPIO33   (était GPIO15 — broche de boot, retiré)
 *    SD   →  GPIO35   (input only, pas de conflit)
 *    L/R  →  GND
 *
 *  BROCHES RÉSERVÉES T-CALL — NE PAS UTILISER :
 *    GPIO 4  = MODEM_PWKEY
 *    GPIO 5  = MODEM_RST
 *    GPIO 23 = MODEM_POWER_ON
 *    GPIO 26 = MODEM_RX
 *    GPIO 27 = MODEM_TX
 *    GPIO 21 = I2C_SDA
 *    GPIO 22 = I2C_SCL
 *
 *  BIBLIOTHÈQUES :
 *    - ESP Async WebServer  (mathieucarbou)
 *    - Async TCP            (mathieucarbou)
 *
 *  RÉGLAGES Arduino IDE :
 *    Board             : ESP32 Dev Module
 *    Upload Speed      : 921600
 *    CPU Frequency     : 240MHz
 *    Flash Size        : 4MB
 *    Partition Scheme  : Default 4MB with spiffs
 *    PSRAM             : Disabled
 *    Erase Flash       : All Flash Contents (une seule fois)
 * ============================================================
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "driver/i2s.h"

// ─── À MODIFIER ───────────────────────────────────────────
const char* WIFI_SSID = "arg";
const char* WIFI_PASS = "a1b2c3d4e5f6g7h8i9j0";
// ──────────────────────────────────────────────────────────

// ── Micro analogique GPIO34 ───────────────────────────────
#define MIC_ANALOG_PIN 34
#define ANALOG_RATE 8000
#define ANALOG_OVERSAMPLE 4

// ── Micro numérique INMP441 I2S ───────────────────────────
// Broches choisies pour éviter tout conflit de boot ESP32
// GPIO13 : sûr au boot
// GPIO33 : sûr au boot (input/output, pas strapping)
// GPIO35 : input only, sûr au boot
#define I2S_SCK_PIN 13  // SCK — sûr au boot
#define I2S_WS_PIN 33   // WS  — sûr au boot
#define I2S_SD_PIN 35   // SD  — input only, sûr au boot

#define I2S_RATE 16000
#define CHUNK_SAMPLES 512

// Type de micro actif
enum MicType { MIC_ANALOG,
               MIC_DIGITAL };
static volatile MicType activeMic = MIC_ANALOG;
static volatile bool streaming = false;

AsyncWebServer server(80);

// Config I2S 32 bits pour INMP441 24bit
i2s_config_t i2s_rx_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = I2S_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = 0,
  .dma_buf_count = 8,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};

i2s_pin_config_t i2s_rx_pins = {
  .bck_io_num = I2S_SCK_PIN,
  .ws_io_num = I2S_WS_PIN,
  .data_out_num = I2S_PIN_NO_CHANGE,
  .data_in_num = I2S_SD_PIN
};

// ── Page HTML ──────────────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dual Micro</title>
<style>
  * { box-sizing:border-box; margin:0; padding:0; }
  body {
    font-family:system-ui,sans-serif; background:#0f0f0f; color:#e0e0e0;
    display:flex; flex-direction:column; align-items:center;
    justify-content:center; min-height:100vh; gap:20px; padding:20px;
  }
  h1 { font-size:1.4rem; font-weight:500; color:#fff; }
  .mic-btns { display:flex; gap:12px; width:100%; max-width:480px; }
  .mic-btn {
    flex:1; padding:14px 0; border:2px solid #333; border-radius:10px;
    background:#1a1a1a; color:#888; font-size:0.9rem; cursor:pointer;
    transition:all 0.2s; text-align:center; line-height:1.6;
  }
  .mic-btn:hover { border-color:#555; color:#ccc; }
  .mic-btn.active-analog  { border-color:#1D9E75; color:#1D9E75; background:#0a2018; }
  .mic-btn.active-digital { border-color:#378ADD; color:#378ADD; background:#08172a; }
  .mic-btn.disabled-btn   { opacity:0.4; cursor:not-allowed; }
  .mic-label { font-size:0.75rem; color:#555; text-align:center; width:100%; max-width:480px; }
  #conn-indicator { display:flex; align-items:center; gap:8px; font-size:0.82rem; color:#888; transition:color 0.3s; }
  #conn-dot { width:9px; height:9px; border-radius:50%; background:#444; transition:background 0.3s; flex-shrink:0; }
  #conn-indicator.connecting #conn-dot { background:#ff9800; animation:pulse 0.7s ease-in-out infinite alternate; }
  #conn-indicator.connected  #conn-dot { background:#4caf50; }
  #conn-indicator.connected            { color:#4caf50; }
  #conn-indicator.error      #conn-dot { background:#f44336; }
  #conn-indicator.error                { color:#f44336; }
  @keyframes pulse { from{opacity:1} to{opacity:0.3} }
  #status { font-size:0.85rem; color:#666; min-height:1.2em; }
  #canvas-wrap { width:100%; max-width:480px; height:80px; background:#1a1a1a; border-radius:12px; overflow:hidden; }
  canvas { width:100%; height:100%; display:block; }
  .controls { display:flex; flex-direction:column; align-items:center; gap:12px; width:100%; max-width:480px; }
  .row { display:flex; align-items:center; gap:10px; width:100%; }
  .row label { font-size:0.85rem; color:#888; min-width:70px; }
  input[type=range] { flex:1; accent-color:#1db954; }
  .val { font-size:0.85rem; color:#aaa; min-width:40px; text-align:right; }
  .listen-btn {
    padding:12px 48px; border:none; border-radius:8px;
    font-size:1rem; cursor:pointer; background:#1db954; color:#fff;
    transition:background 0.2s,transform 0.1s;
  }
  .listen-btn:hover  { background:#1aa34a; }
  .listen-btn:active { transform:scale(0.97); }
  .listen-btn.stop   { background:#e53935; }
  .listen-btn.stop:hover { background:#c62828; }
  .listen-btn:disabled { background:#555; cursor:not-allowed; transform:none; }
  #vu { width:100%; max-width:480px; height:10px; background:#1a1a1a; border-radius:5px; overflow:hidden; }
  #vu-bar { height:100%; width:0%; border-radius:5px; background:#1db954; transition:width 0.08s,background 0.2s; }
</style>
</head>
<body>
<h1>Dual Micro — T-Call SIM800L</h1>

<div class="mic-btns">
  <button class="mic-btn active-analog" id="btn-analog" onclick="selectMic('analog')">
    Ecoute le micro<br>analogique<br><small style="font-size:10px;opacity:0.7">electret — GPIO34 — 8kHz</small>
  </button>
  <button class="mic-btn" id="btn-digital" onclick="selectMic('digital')">
    Ecoute le micro<br>numerique<br><small style="font-size:10px;opacity:0.7">INMP441 I2S — 16kHz 24bit</small>
  </button>
</div>

<div class="mic-label" id="mic-label">Micro analogique actif — electret 8kHz ADC 12bit</div>

<div id="conn-indicator"><div id="conn-dot"></div><span id="conn-text">Non connecte</span></div>
<div id="status"></div>
<div id="canvas-wrap"><canvas id="cv"></canvas></div>
<div id="vu"><div id="vu-bar"></div></div>

<div class="controls">
  <div class="row">
    <label>Volume</label>
    <input type="range" id="vol" min="0" max="6" step="0.1" value="2">
    <span class="val" id="vol-val">x2.0</span>
  </div>
  <div class="row">
    <label>Seuil silence</label>
    <input type="range" id="gate" min="0" max="0.3" step="0.005" value="0.015">
    <span class="val" id="gate-val">0.015</span>
  </div>
</div>

<button class="listen-btn" id="listen-btn" onclick="toggle()">Ecouter</button>

<script>
let ctx,gain,proc,reader,running=false,gateVal=0.015;
let selectedMic='analog', currentSampleRate=8000;

const listenBtn=document.getElementById('listen-btn'),
      status=document.getElementById('status'),
      connInd=document.getElementById('conn-indicator'),
      connTxt=document.getElementById('conn-text'),
      micLabel=document.getElementById('mic-label'),
      volIn=document.getElementById('vol'),volVal=document.getElementById('vol-val'),
      gateIn=document.getElementById('gate'),gateValEl=document.getElementById('gate-val'),
      canvas=document.getElementById('cv'),cCtx=canvas.getContext('2d'),
      vuBar=document.getElementById('vu-bar');
canvas.width=480; canvas.height=80;

volIn.oninput=()=>{volVal.textContent='x'+parseFloat(volIn.value).toFixed(1);if(gain)gain.gain.value=parseFloat(volIn.value);};
gateIn.oninput=()=>{gateVal=parseFloat(gateIn.value);gateValEl.textContent=gateVal.toFixed(3);};

function selectMic(type) {
  if (running) return;
  selectedMic = type;
  document.getElementById('btn-analog').className  = 'mic-btn'+(type==='analog'  ? ' active-analog'  : '');
  document.getElementById('btn-digital').className = 'mic-btn'+(type==='digital' ? ' active-digital' : '');
  if (type==='analog') {
    micLabel.textContent='Micro analogique actif — electret 8kHz ADC 12bit';
    currentSampleRate=8000;
  } else {
    micLabel.textContent='Micro numerique actif — INMP441 I2S 16kHz 24bit';
    currentSampleRate=16000;
  }
  fetch('/select?mic='+type);
}

function setConn(s,t){connInd.className=s;connTxt.textContent=t;}

async function toggle(){if(!running)await start();else stop();}

async function start(){
  running=true; listenBtn.disabled=true; listenBtn.textContent='Connexion...';
  document.getElementById('btn-analog').classList.add('disabled-btn');
  document.getElementById('btn-digital').classList.add('disabled-btn');
  setConn('connecting','Ping du serveur...'); status.textContent='';
  try{
    const r=await fetch('/ping',{signal:AbortSignal.timeout(4000)});
    const d=await r.json();
    if(d.status!=='ready')throw new Error('Serveur non pret');
    setConn('connecting','Serveur OK...');
    await new Promise(res=>setTimeout(res,300));
  }catch(e){
    setConn('error','Serveur inaccessible'); status.textContent=e.message;
    listenBtn.disabled=false; listenBtn.textContent='Ecouter'; running=false;
    document.getElementById('btn-analog').classList.remove('disabled-btn');
    document.getElementById('btn-digital').classList.remove('disabled-btn');
    return;
  }
  ctx=new(window.AudioContext||window.webkitAudioContext)({sampleRate:currentSampleRate});
  gain=ctx.createGain(); gain.gain.value=parseFloat(volIn.value); gain.connect(ctx.destination);
  proc=ctx.createScriptProcessor(4096,0,1); proc.connect(gain);
  const queue=[];
  proc.onaudioprocess=(e)=>{
    const out=e.outputBuffer.getChannelData(0);
    let pos=0;
    while(pos<out.length&&queue.length){
      const c=queue[0],take=Math.min(c.length-c._pos,out.length-pos);
      out.set(c.subarray(c._pos,c._pos+take),pos);
      c._pos+=take; pos+=take; if(c._pos>=c.length)queue.shift();
    }
    for(;pos<out.length;pos++)out[pos]=0;
    let rms=0; for(let i=0;i<out.length;i++)rms+=out[i]*out[i];
    rms=Math.sqrt(rms/out.length);
    status.textContent=rms<gateVal?'Silence...':'';
    const pct=Math.min(rms*300,100);
    vuBar.style.width=pct+'%';
    const waveColor=selectedMic==='analog'?'#1D9E75':'#378ADD';
    vuBar.style.background=pct>80?'#f44336':pct>50?'#ff9800':waveColor;
    drawWave(out,waveColor);
  };
  try{
    const response=await fetch('/audio');
    if(!response.ok)throw new Error('Stream refuse');
    reader=response.body.getReader();
    listenBtn.disabled=false; listenBtn.textContent='Arreter'; listenBtn.className='listen-btn stop';
    const lbl=selectedMic==='analog'?'Analogique 8kHz':'INMP441 24bit 16kHz';
    setConn('connected','En ecoute — '+lbl);
    while(running){
      const{done,value}=await reader.read(); if(done)break;
      const raw=new Float32Array(value.length/2);
      let dcSum=0;
      for(let i=0;i<raw.length;i++){
        let s=(value[i*2+1]<<8)|value[i*2];
        if(s>32767)s-=65536;
        raw[i]=s/32768.0; dcSum+=raw[i];
      }
      const dc=dcSum/raw.length;
      const samples=new Float32Array(raw.length);
      for(let i=0;i<raw.length;i++)samples[i]=raw[i]-dc;
      samples._pos=0; if(queue.length<8)queue.push(samples);
    }
  }catch(e){if(running){setConn('error','Stream interrompu');status.textContent=e.message;}}
  stop();
}

function stop(){
  running=false; listenBtn.disabled=false; listenBtn.textContent='Ecouter'; listenBtn.className='listen-btn';
  document.getElementById('btn-analog').classList.remove('disabled-btn');
  document.getElementById('btn-digital').classList.remove('disabled-btn');
  setConn('','Non connecte'); vuBar.style.width='0%';
  try{if(reader)reader.cancel();}catch(_){}
  try{if(proc)proc.disconnect();}catch(_){}
  try{if(gain)gain.disconnect();}catch(_){}
  try{if(ctx)ctx.close();}catch(_){}
  cCtx.clearRect(0,0,canvas.width,canvas.height); status.textContent='';
}

function drawWave(s,color){
  const w=canvas.width,h=canvas.height;
  cCtx.clearRect(0,0,w,h);
  cCtx.strokeStyle='#333'; cCtx.lineWidth=1;
  cCtx.beginPath(); cCtx.moveTo(0,h/2); cCtx.lineTo(w,h/2); cCtx.stroke();
  cCtx.strokeStyle=color||'#1db954'; cCtx.lineWidth=1.5; cCtx.beginPath();
  const step=Math.ceil(s.length/w);
  for(let x=0;x<w;x++){const v=s[Math.min(x*step,s.length-1)],y=(1-v)/2*h;x===0?cCtx.moveTo(x,y):cCtx.lineTo(x,y);}
  cCtx.stroke();
}
</script>
</body>
</html>
)rawliteral";

// ── Structures et queue ────────────────────────────────────
struct AudioChunk {
  uint8_t data[CHUNK_SAMPLES * 2];
  size_t len;
};
static QueueHandle_t audioQueue = nullptr;

// ── Tâche audio sur core 0 ─────────────────────────────────
void audioTask(void* pvParam) {
  int32_t* i2sBuf = (int32_t*)malloc(CHUNK_SAMPLES * sizeof(int32_t));
  AudioChunk* chunk = (AudioChunk*)malloc(sizeof(AudioChunk));
  if (!i2sBuf || !chunk) {
    vTaskDelete(nullptr);
    return;
  }

  const int analogPeriod = 1000000 / ANALOG_RATE;

  while (true) {
    if (!streaming) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    chunk->len = 0;

    if (activeMic == MIC_ANALOG) {
      for (int i = 0; i < CHUNK_SAMPLES; i++) {
        int64_t t0 = esp_timer_get_time();
        uint32_t acc = 0;
        for (int j = 0; j < ANALOG_OVERSAMPLE; j++) acc += analogRead(MIC_ANALOG_PIN);
        uint16_t raw = (uint16_t)(acc / ANALOG_OVERSAMPLE);
        int16_t s16 = (int16_t)((int32_t)raw - 2048) * 16;
        chunk->data[chunk->len++] = (uint8_t)(s16 & 0xFF);
        chunk->data[chunk->len++] = (uint8_t)((s16 >> 8) & 0xFF);
        while ((esp_timer_get_time() - t0) < analogPeriod)
          ;
      }
    } else {
      size_t bytesRead = 0;
      i2s_read(I2S_NUM_0, i2sBuf, CHUNK_SAMPLES * sizeof(int32_t), &bytesRead, portMAX_DELAY);
      int samplesRead = bytesRead / sizeof(int32_t);
      for (int i = 0; i < samplesRead; i++) {
        int16_t s16 = (int16_t)(i2sBuf[i] >> 14);
        chunk->data[chunk->len++] = (uint8_t)(s16 & 0xFF);
        chunk->data[chunk->len++] = (uint8_t)((s16 >> 8) & 0xFF);
      }
    }
    xQueueSend(audioQueue, chunk, portMAX_DELAY);
  }
  free(i2sBuf);
  free(chunk);
}

void wifiWatchdog(void* pvParam) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      int t = 0;
      while (WiFi.status() != WL_CONNECTED && t < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        t++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] Reconnecte : ");
        Serial.println(WiFi.localIP());
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // délai plus long pour laisser le boot se stabiliser

  // ADC analogique
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(MIC_ANALOG_PIN, INPUT);
  Serial.println("[OK] ADC analogique — GPIO34");

  // I2S INMP441 — init après le boot complet
  delay(500);
  i2s_driver_install(I2S_NUM_0, &i2s_rx_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &i2s_rx_pins);
  Serial.println("[OK] I2S INMP441 — SCK:13 WS:33 SD:35");

  // WiFi
  Serial.printf("\nConnexion a %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print('.');
    tries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    delay(5000);
    ESP.restart();
  }

  Serial.println("\n[OK] WiFi connecte");
  Serial.print("[>>] http://");
  Serial.println(WiFi.localIP());

  audioQueue = xQueueCreate(4, sizeof(AudioChunk));
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(wifiWatchdog, "wifidog", 2048, nullptr, 1, nullptr, 0);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"status\":\"ready\",\"mic\":\"";
    json += (activeMic == MIC_ANALOG) ? "analog" : "digital";
    json += "\",\"heap\":";
    json += ESP.getFreeHeap();
    json += ",\"rssi\":";
    json += WiFi.RSSI();
    json += "}";
    req->send(200, "application/json", json);
  });

  server.on("/select", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("mic")) {
      String mic = req->getParam("mic")->value();
      if (mic == "analog") {
        activeMic = MIC_ANALOG;
        Serial.println("[>>] Micro analogique actif");
      } else if (mic == "digital") {
        activeMic = MIC_DIGITAL;
        Serial.println("[>>] Micro numerique INMP441 actif");
      }
    }
    req->send(200, "text/plain", "ok");
  });

  server.on("/audio", HTTP_GET, [](AsyncWebServerRequest* req) {
    streaming = true;
    AsyncWebServerResponse* resp = req->beginChunkedResponse(
      "application/octet-stream",
      [](uint8_t* buf, size_t maxLen, size_t) -> size_t {
        if (!streaming) return 0;
        AudioChunk chunk;
        if (xQueueReceive(audioQueue, &chunk, pdMS_TO_TICKS(150)) == pdTRUE) {
          size_t n = min((size_t)chunk.len, maxLen);
          memcpy(buf, chunk.data, n);
          return n;
        }
        size_t silenceBytes = min((size_t)(CHUNK_SAMPLES * 2), maxLen);
        memset(buf, 0, silenceBytes);
        return silenceBytes;
      });
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest* req) {
    streaming = false;
    req->send(200, "text/plain", "stopped");
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "{\"rssi\":";
    json += WiFi.RSSI();
    json += ",\"heap\":";
    json += ESP.getFreeHeap();
    json += ",\"mic\":\"";
    json += (activeMic == MIC_ANALOG) ? "analog" : "digital";
    json += "\",\"streaming\":";
    json += streaming ? "true" : "false";
    json += "}";
    req->send(200, "application/json", json);
  });

  server.begin();
  Serial.println("[OK] Serveur demarre — dual micro pret");
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 15000) {
    Serial.printf("[OK] IP:%s RSSI:%ddBm Heap:%u Mic:%s Stream:%s\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(),
                  (activeMic == MIC_ANALOG) ? "analog" : "digital",
                  streaming ? "actif" : "inactif");
    last = millis();
  }
  delay(100);
}
