/*
 * ============================================================
 *  PIANO WEB + LECTEUR MP3 — ESP32 T-Call
 *  Serveur Web avec Piano virtuel & Upload MP3
 *  Sortie audio sur MAX98357A via I2S
 * ============================================================
 *
 *  CÂBLAGE AMPLI AUDIO (MAX98357A) :
 *    VIN  →  5V ou 3.3V
 *    GND  →  GND
 *    LRC  →  GPIO33  (WS)
 *    BCLK →  GPIO32  (SCK)
 *    DIN  →  GPIO25  (Data)
 *
 *  BIBLIOTHÈQUES REQUISES :
 *    - ESPAsyncWebServer (+ AsyncTCP)
 *    - ESP8266Audio
 *    - SPIFFS (intégré)
 * ============================================================
 */

#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <math.h>

// --- ESP8266Audio libraries (gère I2S en interne) ---
#include "AudioFileSourceSPIFFS.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// ============== CONFIGURATION ==============
const char* ssid     = "arg";
const char* password = "a1b2c3d4e5f6g7h8i9j0";

#define I2S_SCK_PIN   32
#define I2S_WS_PIN    33
#define I2S_DOUT_PIN  25
#define I2S_RATE      22050

// ============== OBJETS GLOBAUX ==============
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Audio output (unique, partagé)
AudioOutputI2S     *audioOut = nullptr;

// MP3 player objects
AudioGeneratorMP3  *mp3      = nullptr;
AudioFileSourceSPIFFS *mp3File = nullptr;

// Piano state
volatile float noteVolume = 0.5;
bool playingMP3  = false;
bool playingNote = false;

// ============================================================
//  PAGE HTML EMBARQUÉE — Piano + Upload MP3
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>🎹 Piano ESP32</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700&display=swap');

  * { margin:0; padding:0; box-sizing:border-box; }

  body {
    font-family: 'Inter', sans-serif;
    background: linear-gradient(135deg, #0f0c29, #302b63, #24243e);
    min-height: 100vh;
    color: #e0e0e0;
    overflow-x: hidden;
  }

  .container {
    max-width: 900px;
    margin: 0 auto;
    padding: 20px 15px;
  }

  header {
    text-align: center;
    padding: 25px 0 15px;
  }

  header h1 {
    font-size: 2.2em;
    font-weight: 700;
    background: linear-gradient(90deg, #ff6fd8, #3813c2);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    text-shadow: none;
    margin-bottom: 5px;
  }

  header p {
    font-size: 0.9em;
    color: #888;
    font-weight: 300;
  }

  .status {
    text-align: center;
    margin: 10px 0;
    font-size: 0.85em;
  }
  .status span {
    display: inline-block;
    padding: 4px 14px;
    border-radius: 20px;
    background: rgba(56, 19, 194, 0.3);
    border: 1px solid rgba(255,111,216,0.3);
  }
  .status.connected span { background: rgba(0,200,83,0.2); border-color: rgba(0,200,83,0.5); color:#4caf50; }
  .status.error span { background: rgba(244,67,54,0.2); border-color: rgba(244,67,54,0.5); color:#f44336; }

  .piano-wrapper {
    background: rgba(255,255,255,0.04);
    border-radius: 16px;
    padding: 20px 10px 25px;
    margin: 15px 0;
    border: 1px solid rgba(255,255,255,0.08);
    backdrop-filter: blur(10px);
    overflow-x: auto;
  }

  .piano-label {
    font-size: 0.8em;
    color: #888;
    text-transform: uppercase;
    letter-spacing: 2px;
    margin-bottom: 12px;
    padding-left: 5px;
  }

  .piano {
    display: flex;
    position: relative;
    height: 200px;
    user-select: none;
    -webkit-user-select: none;
    touch-action: none;
    margin: 0 auto;
    width: fit-content;
  }

  .key {
    position: relative;
    cursor: pointer;
    border-radius: 0 0 8px 8px;
    transition: all 0.08s ease;
  }

  .key.white {
    width: 48px;
    height: 200px;
    background: linear-gradient(180deg, #f8f8f8, #e8e8e8);
    border: 1px solid #ccc;
    margin-right: -1px;
    z-index: 1;
    box-shadow: 0 4px 6px rgba(0,0,0,0.15);
  }

  .key.white:active, .key.white.active {
    background: linear-gradient(180deg, #ddd, #ccc);
    box-shadow: 0 2px 3px rgba(0,0,0,0.1);
    transform: translateY(2px);
  }

  .key.black {
    width: 30px;
    height: 130px;
    background: linear-gradient(180deg, #333, #111);
    border: 1px solid #000;
    margin-left: -15px;
    margin-right: -15px;
    z-index: 2;
    box-shadow: 0 4px 8px rgba(0,0,0,0.4);
  }

  .key.black:active, .key.black.active {
    background: linear-gradient(180deg, #555, #333);
    box-shadow: 0 2px 4px rgba(0,0,0,0.3);
    transform: translateY(2px);
  }

  .key .note-label {
    position: absolute;
    bottom: 8px;
    left: 50%;
    transform: translateX(-50%);
    font-size: 0.6em;
    color: #999;
    pointer-events: none;
  }

  .key.black .note-label {
    color: #666;
    bottom: 6px;
    font-size: 0.55em;
  }

  .controls {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 15px;
    margin: 15px 0;
  }

  .card {
    background: rgba(255,255,255,0.04);
    border-radius: 16px;
    padding: 20px;
    border: 1px solid rgba(255,255,255,0.08);
    backdrop-filter: blur(10px);
  }

  .card h3 {
    font-size: 0.8em;
    color: #888;
    text-transform: uppercase;
    letter-spacing: 2px;
    margin-bottom: 15px;
  }

  .volume-control {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .volume-control .icon { font-size: 1.4em; }

  input[type="range"] {
    -webkit-appearance: none;
    flex: 1;
    height: 6px;
    border-radius: 3px;
    background: linear-gradient(90deg, #ff6fd8, #3813c2);
    outline: none;
  }

  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: #fff;
    cursor: pointer;
    box-shadow: 0 2px 6px rgba(0,0,0,0.3);
  }

  .vol-value {
    font-size: 1em;
    font-weight: 600;
    min-width: 40px;
    text-align: right;
    color: #ff6fd8;
  }

  .upload-zone {
    border: 2px dashed rgba(255,111,216,0.3);
    border-radius: 12px;
    padding: 25px;
    text-align: center;
    cursor: pointer;
    transition: all 0.3s ease;
    position: relative;
  }

  .upload-zone:hover {
    border-color: #ff6fd8;
    background: rgba(255,111,216,0.05);
  }

  .upload-zone .icon { font-size: 2em; margin-bottom: 8px; }
  .upload-zone p { font-size: 0.85em; color: #888; }
  .upload-zone input[type="file"] {
    position: absolute;
    inset: 0;
    opacity: 0;
    cursor: pointer;
  }

  .btn {
    display: inline-block;
    padding: 10px 24px;
    border: none;
    border-radius: 8px;
    font-family: inherit;
    font-size: 0.9em;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s ease;
    margin-top: 10px;
  }

  .btn-play {
    background: linear-gradient(135deg, #ff6fd8, #3813c2);
    color: #fff;
  }
  .btn-play:hover { transform: translateY(-2px); box-shadow: 0 4px 15px rgba(255,111,216,0.4); }
  .btn-play:disabled { opacity: 0.5; cursor: not-allowed; transform: none; }

  .btn-stop {
    background: rgba(244,67,54,0.2);
    color: #f44336;
    border: 1px solid rgba(244,67,54,0.4);
    margin-left: 8px;
  }
  .btn-stop:hover { background: rgba(244,67,54,0.3); }

  .file-info {
    margin-top: 10px;
    font-size: 0.8em;
    color: #aaa;
  }

  .progress-bar {
    width: 100%;
    height: 4px;
    background: rgba(255,255,255,0.1);
    border-radius: 2px;
    margin-top: 10px;
    overflow: hidden;
    display: none;
  }
  .progress-bar .fill {
    height: 100%;
    background: linear-gradient(90deg, #ff6fd8, #3813c2);
    border-radius: 2px;
    width: 0%;
    transition: width 0.3s;
  }

  @media (max-width: 600px) {
    .controls { grid-template-columns: 1fr; }
    .piano { height: 160px; }
    .key.white { width: 36px; height: 160px; }
    .key.black { width: 24px; height: 100px; margin-left: -12px; margin-right: -12px; }
    header h1 { font-size: 1.6em; }
  }
</style>
</head>
<body>

<div class="container">
  <header>
    <h1>🎹 Piano ESP32</h1>
    <p>MAX98357A • Sortie Haut-Parleur</p>
  </header>

  <div class="status" id="statusBar">
    <span>⏳ Connexion...</span>
  </div>

  <div class="piano-wrapper">
    <div class="piano-label">Clavier — 2 Octaves</div>
    <div class="piano" id="piano"></div>
  </div>

  <div class="controls">
    <div class="card">
      <h3>🔊 Volume</h3>
      <div class="volume-control">
        <span class="icon">🔈</span>
        <input type="range" id="volume" min="0" max="100" value="50">
        <span class="vol-value" id="volValue">50%</span>
      </div>
    </div>

    <div class="card">
      <h3>🎵 Lecteur MP3</h3>
      <div class="upload-zone" id="uploadZone">
        <div class="icon">📁</div>
        <p>Glisser un MP3 ici ou cliquer</p>
        <input type="file" id="fileInput" accept=".mp3,audio/mpeg">
      </div>
      <div class="file-info" id="fileInfo"></div>
      <div class="progress-bar" id="progressBar"><div class="fill" id="progressFill"></div></div>
      <div style="margin-top: 10px;">
        <button class="btn btn-play" id="btnPlay" disabled onclick="playMP3()">▶ Jouer</button>
        <button class="btn btn-stop" id="btnStop" onclick="stopMP3()">⏹ Stop</button>
      </div>
    </div>
  </div>
</div>

<script>
  let ws;
  let wsConnected = false;

  function connectWS() {
    ws = new WebSocket('ws://' + location.host + '/ws');
    ws.onopen = () => {
      wsConnected = true;
      setStatus('connected', '🟢 Connecté');
    };
    ws.onclose = () => {
      wsConnected = false;
      setStatus('error', '🔴 Déconnecté');
      setTimeout(connectWS, 2000);
    };
    ws.onerror = () => {
      setStatus('error', '🔴 Erreur');
    };
    ws.onmessage = (e) => {
      if (e.data === 'MP3_DONE') {
        document.getElementById('btnPlay').disabled = false;
        document.getElementById('btnPlay').textContent = '▶ Jouer';
      }
    };
  }
  connectWS();

  function setStatus(cls, text) {
    const s = document.getElementById('statusBar');
    s.className = 'status ' + cls;
    s.innerHTML = '<span>' + text + '</span>';
  }

  const notes = [
    {name:'C4',  freq:261.63, type:'white'},
    {name:'C#4', freq:277.18, type:'black'},
    {name:'D4',  freq:293.66, type:'white'},
    {name:'D#4', freq:311.13, type:'black'},
    {name:'E4',  freq:329.63, type:'white'},
    {name:'F4',  freq:349.23, type:'white'},
    {name:'F#4', freq:369.99, type:'black'},
    {name:'G4',  freq:392.00, type:'white'},
    {name:'G#4', freq:415.30, type:'black'},
    {name:'A4',  freq:440.00, type:'white'},
    {name:'A#4', freq:466.16, type:'black'},
    {name:'B4',  freq:493.88, type:'white'},
    {name:'C5',  freq:523.25, type:'white'},
    {name:'C#5', freq:554.37, type:'black'},
    {name:'D5',  freq:587.33, type:'white'},
    {name:'D#5', freq:622.25, type:'black'},
    {name:'E5',  freq:659.26, type:'white'},
    {name:'F5',  freq:698.46, type:'white'},
    {name:'F#5', freq:739.99, type:'black'},
    {name:'G5',  freq:783.99, type:'white'},
    {name:'G#5', freq:830.61, type:'black'},
    {name:'A5',  freq:880.00, type:'white'},
    {name:'A#5', freq:932.33, type:'black'},
    {name:'B5',  freq:987.77, type:'white'},
  ];

  // Mapping clavier PC (AZERTY) -> index dans notes[]
  const keyMap = {
    'q':0, 'z':2, 'e':4, 'r':5, 't':7, 'y':9, 'u':11,
    'i':12, 'o':14, 'p':16,
    's':1, 'd':3, 'g':6, 'h':8, 'j':10,
    'l':13, 'm':15,
    // QWERTY fallback
    'a':0, 'w':2,
  };

  let activeNotes = new Set();

  const pianoEl = document.getElementById('piano');
  const keyElements = [];
  notes.forEach((n, idx) => {
    const key = document.createElement('div');
    key.className = 'key ' + n.type;
    // Trouver la touche clavier associée
    let kbLabel = '';
    for (let [k, v] of Object.entries(keyMap)) {
      if (v === idx && k.length === 1) { kbLabel = k.toUpperCase(); break; }
    }
    key.innerHTML = '<span class="note-label">' + n.name + (kbLabel ? '<br>'+kbLabel : '') + '</span>';
    key.dataset.freq = n.freq;
    key.dataset.idx = idx;

    // Touch events
    key.addEventListener('touchstart', (e) => {
      e.preventDefault();
      noteOn(idx);
    });
    key.addEventListener('touchend', (e) => {
      e.preventDefault();
      noteOff(idx);
    });
    key.addEventListener('touchcancel', (e) => {
      e.preventDefault();
      noteOff(idx);
    });

    // Mouse events
    key.addEventListener('mousedown', (e) => {
      e.preventDefault();
      noteOn(idx);
    });
    key.addEventListener('mouseup', () => noteOff(idx));
    key.addEventListener('mouseleave', () => {
      if (activeNotes.has(idx)) noteOff(idx);
    });

    pianoEl.appendChild(key);
    keyElements.push(key);
  });

  // Keyboard events
  document.addEventListener('keydown', (e) => {
    if (e.repeat) return;
    const idx = keyMap[e.key.toLowerCase()];
    if (idx !== undefined && !activeNotes.has(idx)) {
      noteOn(idx);
    }
  });
  document.addEventListener('keyup', (e) => {
    const idx = keyMap[e.key.toLowerCase()];
    if (idx !== undefined) {
      noteOff(idx);
    }
  });

  function noteOn(idx) {
    if (activeNotes.has(idx)) return;
    activeNotes.add(idx);
    keyElements[idx].classList.add('active');
    if (wsConnected && ws.readyState === WebSocket.OPEN) {
      ws.send('ON:' + notes[idx].freq.toFixed(2));
    }
  }

  function noteOff(idx) {
    if (!activeNotes.has(idx)) return;
    activeNotes.delete(idx);
    keyElements[idx].classList.remove('active');
    if (wsConnected && ws.readyState === WebSocket.OPEN) {
      ws.send('OFF');
    }
  }

  const volSlider = document.getElementById('volume');
  const volValue = document.getElementById('volValue');
  volSlider.addEventListener('input', () => {
    volValue.textContent = volSlider.value + '%';
    if (wsConnected && ws.readyState === WebSocket.OPEN) {
      ws.send('VOL:' + volSlider.value);
    }
  });

  const fileInput = document.getElementById('fileInput');
  const fileInfo = document.getElementById('fileInfo');

  fileInput.addEventListener('change', (e) => {
    const f = e.target.files[0];
    if (!f) return;
    fileInfo.textContent = '📄 ' + f.name + ' (' + (f.size / 1024).toFixed(1) + ' KB)';
    uploadFile(f);
  });

  function uploadFile(f) {
    const pb = document.getElementById('progressBar');
    const pf = document.getElementById('progressFill');
    const btn = document.getElementById('btnPlay');

    pb.style.display = 'block';
    pf.style.width = '0%';
    btn.disabled = true;

    const maxBytes = 1.3 * 1024 * 1024; // ~1.3 MB max pour SPIFFS
    if (f.size > maxBytes) {
      alert("⚠️ Le fichier MP3 est trop grand ! " + (f.size/1024/1024).toFixed(2) + " MB.\nIl va s'arrêter avant la fin car la mémoire de l'ESP32 (~1.3 MB) sera pleine.");
    }

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload', true);

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        pf.style.width = ((e.loaded / e.total) * 100) + '%';
      }
    };

    xhr.onload = () => {
      if (xhr.status === 200) {
        fileInfo.textContent += ' ✅ Uploadé!';
        btn.disabled = false;
        pf.style.width = '100%';
      } else {
        fileInfo.textContent += ' ❌ Erreur!';
      }
    };

    xhr.onerror = () => { fileInfo.textContent += ' ❌ Erreur réseau!'; };

    const fd = new FormData();
    fd.append('file', f);
    xhr.send(fd);
  }

  function playMP3() {
    if (wsConnected && ws.readyState === WebSocket.OPEN) {
      ws.send('MP3:PLAY');
      document.getElementById('btnPlay').disabled = true;
      document.getElementById('btnPlay').textContent = '⏳ Lecture...';
    }
  }

  function stopMP3() {
    if (wsConnected && ws.readyState === WebSocket.OPEN) {
      ws.send('MP3:STOP');
      document.getElementById('btnPlay').disabled = false;
      document.getElementById('btnPlay').textContent = '▶ Jouer';
    }
  }
</script>
</body>
</html>
)rawliteral";

// ============================================================
//  AUDIO OUTPUT SETUP (ESP8266Audio gère I2S)
// ============================================================
void setupAudioOutput() {
  if (audioOut) return;
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_SCK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
  audioOut->SetRate(I2S_RATE);
  audioOut->SetChannels(1);
  audioOut->SetBuffers(8, 256); // Buffers plus grands pour éviter les craquements (underrun)
  audioOut->SetGain(noteVolume);
  audioOut->begin();
  Serial.println("✅ AudioOutputI2S configuré pour MAX98357A");
}

// ============================================================
//  NON-BLOCKING NOTE GENERATOR (NOTE ON/OFF)
// ============================================================
float    _noteFreq      = 0;
uint32_t _notePos       = 0;
bool     _noteActive    = false;
bool     _noteFadingOut = false;
int      _fadeOutPos    = 0;
int      _fadeOutTotal  = 0;

void noteOn(float freq, float volume) {
  if (audioOut) {
    audioOut->SetGain(volume);
  }
  _noteFreq      = freq;
  _notePos       = 0;
  _noteActive    = true;
  _noteFadingOut = false;
}

void noteOff() {
  if (_noteActive && !_noteFadingOut) {
    _noteFadingOut = true;
    _fadeOutPos    = 0;
    _fadeOutTotal  = (I2S_RATE * 20) / 1000; // 20ms fade out doux
  }
}

// Appelé depuis loop()
void tickNote() {
  if (!audioOut || playingMP3) return;

  int fadeInSamples = (I2S_RATE * 5) / 1000; // 5ms fade in

  // Remplir jusqu'à 64 samples par itération pour garder le buffer I2S plein
  for (int i = 0; i < 64; i++) {
    int16_t sample = 0; // Silence par défaut

    if (_noteActive || _noteFadingOut) {
      float t = (float)_notePos / I2S_RATE;
      float val = sinf(2.0f * M_PI * _noteFreq * t);
      
      float env = 1.0f;
      // Fade in (Attack)
      if (_notePos < (uint32_t)fadeInSamples) {
        env = (float)_notePos / fadeInSamples;
      }

      // Fade out (Release)
      if (_noteFadingOut) {
        float fadeEnv = 1.0f - ((float)_fadeOutPos / _fadeOutTotal);
        if (fadeEnv < 0) fadeEnv = 0;
        env *= fadeEnv;
        _fadeOutPos++;
        if (_fadeOutPos >= _fadeOutTotal) {
          _noteActive = false;
          _noteFadingOut = false;
        }
      }

      // Amplitude restaurée à 30000 (comme à l'origine) pour retrouver le même volume/timbre
      sample = (int16_t)(val * env * 30000.0f);
    }

    int16_t ms[2] = {sample, sample};
    
    // Si ConsumeSample retourne false, le buffer DMA I2S est plein -> on arrête d'écrire pour ce tick
    // On ne doit RIEN incrémenter si le sample est refusé, sinon on "saute" dans le temps et ça fausse la note !
    if (!audioOut->ConsumeSample(ms)) {
      break; 
    }

    // Le sample a été accepté par le buffer I2S, on peut avancer dans le temps
    if (_noteActive || _noteFadingOut) {
      _notePos++;
    }
  }
}

// ============================================================
//  WEBSOCKET HANDLER
// ============================================================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Client WS connecté: #%u\n", client->id());
  }
  else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("Client WS déconnecté: #%u\n", client->id());
    noteOff(); // Arrêter si le client se déconnecte
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      String msg = (char*)data;

      if (msg.startsWith("ON:")) {
        float freq = msg.substring(3).toFloat();
        if (freq > 0 && !playingMP3) {
          Serial.printf("🎵 Note ON: %.1f Hz\n", freq);
          noteOn(freq, noteVolume);
        }
      }
      else if (msg == "OFF") {
        Serial.println("🔇 Note OFF");
        noteOff();
      }
      else if (msg.startsWith("VOL:")) {
        int vol = msg.substring(4).toInt();
        noteVolume = vol / 100.0f;
        if (audioOut) audioOut->SetGain(noteVolume);
        Serial.printf("🔊 Volume: %d%%\n", vol);
      }
      else if (msg == "MP3:PLAY") {
        Serial.println("▶ Lecture MP3 demandée");
        startMP3Playback();
      }
      else if (msg == "MP3:STOP") {
        Serial.println("⏹ Stop MP3");
        stopMP3Playback();
        client->text("MP3_DONE");
      }
    }
  }
}

// ============================================================
//  MP3 PLAYBACK
// ============================================================
void startMP3Playback() {
  stopMP3Playback();

  if (!SPIFFS.exists("/uploaded.mp3")) {
    Serial.println("❌ Pas de fichier MP3 uploadé!");
    return;
  }

  // Stop the current audioOut used for piano, MP3 will create its own
  if (audioOut) {
    audioOut->stop();
    delete audioOut;
    audioOut = nullptr;
  }

  mp3File  = new AudioFileSourceSPIFFS("/uploaded.mp3");
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_SCK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
  
  // Buffers GÉANTS pour le MP3 (16x1024 au lieu des tout petits de base). 
  // Les parties fortes (beaucoup de décibels) demandent plus de calculs au MP3, ce qui vide le buffer et fait grésiller.
  audioOut->SetBuffers(16, 1024);
  
  // Gain limité à 85% du slider pour bloquer toute saturation (clipping digital) sur les gros pics de son
  audioOut->SetGain(noteVolume * 0.85);

  mp3      = new AudioGeneratorMP3();

  if (mp3->begin(mp3File, audioOut)) {
    playingMP3 = true;
    Serial.println("✅ Lecture MP3 démarrée!");
  } else {
    Serial.println("❌ Erreur démarrage MP3");
    stopMP3Playback();
  }
}

void stopMP3Playback() {
  if (mp3) {
    if (mp3->isRunning()) mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (mp3File) {
    delete mp3File;
    mp3File = nullptr;
  }

  bool wasPlaying = playingMP3;
  playingMP3 = false;

  // Recréer l'audioOut pour le piano
  if (wasPlaying || !audioOut) {
    if (audioOut) {
      audioOut->stop();
      delete audioOut;
      audioOut = nullptr;
    }
    setupAudioOutput();
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n============================================");
  Serial.println("  🎹 PIANO WEB + MP3 — ESP32 T-Call");
  Serial.println("============================================");

  // --- SPIFFS ---
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS mount failed!");
  } else {
    Serial.printf("📁 SPIFFS: %u KB libre / %u KB total\n",
      (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024,
      SPIFFS.totalBytes() / 1024);
  }

  // --- WiFi STA (connexion au réseau existant) ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("📡 Connexion au réseau '%s'", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Connecté!");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());

  // --- Audio Output (via ESP8266Audio) ---
  setupAudioOutput();

  // --- WebSocket ---
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // --- Routes HTTP ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  // Upload MP3
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest *request, const String& filename,
       size_t index, uint8_t *data, size_t len, bool final) {

      static File uploadFile;

      if (index == 0) {
        Serial.printf("📤 Upload début: %s\n", filename.c_str());
        stopMP3Playback();
        uploadFile = SPIFFS.open("/uploaded.mp3", FILE_WRITE);
        if (!uploadFile) {
          Serial.println("❌ Impossible d'ouvrir le fichier!");
          return;
        }
      }

      if (uploadFile && len) {
        uploadFile.write(data, len);
      }

      if (final) {
        if (uploadFile) {
          uploadFile.close();
          Serial.printf("✅ Upload terminé: %u octets\n", index + len);
        }
      }
    }
  );

  server.begin();
  Serial.println("🚀 Serveur web démarré sur le port 80");
  Serial.println("Ouvrez http://192.168.4.1 dans votre navigateur!");
  Serial.println("============================================\n");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  // Générer les notes piano (non-bloquant)
  tickNote();

  // Handle MP3 playback
  if (playingMP3 && mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      Serial.println("🎵 MP3 terminé");
      stopMP3Playback();
      ws.textAll("MP3_DONE");
    }
  }

  ws.cleanupClients();
}
