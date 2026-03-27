/**
 * @file      WebSMS_GPS.ino
 * @desc      ESP32 + A7670G + GPS L76K + Carte SD
 * - Thème Cyberpunk Noir & Vert
 * - Renommer les contacts opérationnel
 * - Mode Éco forcé : +33662727445 (Toutes les 30s)
 */

#define TINY_GSM_MODEM_A7670

#include "utilities.h"
#include <WiFi.h>
#include <WebServer.h>
#include <TinyGsmClient.h>
#include <TinyGPS++.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <vector>

#ifndef SerialAT
#define SerialAT Serial1
#endif
#ifndef SerialGPS
#define SerialGPS Serial2
#endif

#define BOARD_GPS_TX_PIN  21
#define BOARD_GPS_RX_PIN  22

// --- Configuration WiFi ---
const char* ssid     = "arg";
const char* password = "a1b2c3d4e5f6g7h8i9j0";
const char* ap_ssid  = "ESP32_SMS_GPS";
const char* ap_pass  = "123456789";

// Variables globales
bool wifiOk = false; 
unsigned long lastValidFix = 0; 

WebServer server(80);
TinyGsm modem(SerialAT);
TinyGPSPlus gps;

unsigned long lastSmsCheck  = 0;
unsigned long lastEcoSms    = 0; 
bool hadFix                 = false;

bool suiviActif = false;
unsigned long dernierSmsSuivi = 0;

int systemMode = 0; 
// --- Configuration ---
String cfgNumSuiviMode = "33662727445";    // Reçoit les SMS auto Éco/Suivi
String cfgNumAutorisesGps = "";            // Séparés par virgules. Vide = tout le monde

#define SMS_CHECK_INTERVAL  5000
#define ECO_SMS_INTERVAL    30000

// ════════════════════════════════════════════
//  📝 PROTOTYPES (Obligatoires pour le Linker)
// ════════════════════════════════════════════
String formatPhone(String p);
String getHeureHiverLisible();
String getCompactTimestamp();
String getNickname(String phone);
void setNickname(String phone, String nickname);
void lireModeDepuisSD();
void ecrireModeSurSD(int mode);
void lireConfigDepuisSD();
void ecrireConfigSurSD();
void saveToSD(String phone, String message, bool isSent);
String construireMessageGpsDetail();
void scanAndSaveSmsToSD();
void envoyerSMSSuivi(String cibles, String message);

void handleRoot();
void handleNumeroList();
void handleNumeroChat();
void handleRenommer();
void handleApiMessages();
void handleApiSend();
void handleNouveau();
void handleSendDirect();
void handleAjouter();
void handleWebGPS();
void handleSwitchWifi();
void handleSwitchEco();
void handleToggleSuivi();
void handleParametres();
void handleSupprimer();


// ════════════════════════════════════════════
//  🔧 UTILS & TEMPS
// ════════════════════════════════════════════

String formatPhone(String p) { 
    p.replace(" ", ""); 
    p.replace("+", ""); 
    p.trim(); 
    return p; 
}

String getHeureHiverLisible() {
    if (gps.time.isValid() && gps.date.isValid()) {
        int h = gps.time.hour() + 1; // UTC+1
        int d = gps.date.day();
        int m = gps.date.month();
        int y = gps.date.year();
        if (h >= 24) { h -= 24; d++; }
        char buf[25];
        snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d/%04d", h, gps.time.minute(), d, m, y);
        return String(buf);
    }
    return "Fix GPS requis";
}

String getCompactTimestamp() {
    if (gps.time.isValid() && gps.date.isValid()) {
        int h = gps.time.hour() + 1;
        int d = gps.date.day();
        if (h >= 24) { h -= 24; d++; }
        char buf[15];
        snprintf(buf, sizeof(buf), "%02d%02d%02d%02d%02d%02d", 
                 gps.date.year() % 100, gps.date.month(), d, h, gps.time.minute(), gps.time.second());
        return String(buf);
    }
    return "000000000000";
}


// ════════════════════════════════════════════
//  📂 CARNET D'ADRESSES (Fichier SD)
// ════════════════════════════════════════════

String getNickname(String phone) {
    if (!SD.exists("/contacts.txt")) return "";
    File f = SD.open("/contacts.txt", FILE_READ);
    if (!f) return "";
    while (f.available()) {
        String line = f.readStringUntil('\n'); line.trim();
        int sep = line.indexOf('|');
        if (sep != -1 && line.substring(0, sep) == phone) { f.close(); return line.substring(sep + 1); }
    }
    f.close(); return "";
}

void setNickname(String phone, String nickname) {
    std::vector<String> lines;
    if (SD.exists("/contacts.txt")) {
        File f = SD.open("/contacts.txt", FILE_READ);
        while (f.available()) {
            String line = f.readStringUntil('\n'); line.trim();
            if (line.length() == 0) continue;
            int sep = line.indexOf('|');
            if (sep != -1 && line.substring(0, sep) != phone) lines.push_back(line);
        }
        f.close();
    }
    if (nickname.length() > 0) lines.push_back(phone + "|" + nickname);

    SD.remove("/contacts.txt");
    File f = SD.open("/contacts.txt", FILE_WRITE);
    for (auto& l : lines) f.println(l);
    f.close();
}


// ════════════════════════════════════════════
//  💾 GESTION DES MODES
// ════════════════════════════════════════════

void lireModeDepuisSD() {
    if (SD.exists("/mode.txt")) {
        File f = SD.open("/mode.txt", FILE_READ);
        if (f) {
            String content = f.readString();
            content.trim();
            if (content.length() > 0) systemMode = content.toInt();
            f.close();
        }
    }
}

void ecrireModeSurSD(int mode) {
    SD.remove("/mode.txt");
    File f = SD.open("/mode.txt", FILE_WRITE);
    if (f) {
        f.print(mode);
        f.flush();
        f.close();
    }
}


void lireConfigDepuisSD() {
    if (SD.exists("/autorise.txt")) {
        File f = SD.open("/autorise.txt", FILE_READ);
        if (f) {
            String c1 = f.readStringUntil('\n'); c1.trim();
            String c2 = f.readStringUntil('\n'); c2.trim();
            if (c1.length() > 0) cfgNumSuiviMode = c1;
            cfgNumAutorisesGps = c2;
            f.close();
        }
    }
}

void ecrireConfigSurSD() {
    SD.remove("/autorise.txt");
    File f = SD.open("/autorise.txt", FILE_WRITE);
    if (f) {
        f.println(cfgNumSuiviMode);
        f.println(cfgNumAutorisesGps);
        f.flush();
        f.close();
    }
}


// ════════════════════════════════════════════
//  🛰️ SMS & MÉTIQUES GPS
// ════════════════════════════════════════════

void saveToSD(String phone, String message, bool isSent) {
    String cp = formatPhone(phone);
    String file = "/" + cp + ".txt";
    String ts = getCompactTimestamp();
    String dir = isSent ? "OUT" : "IN ";
    
    // Le message doit tenir sur UNE seule ligne dans le fichier txt pour que la lecture
    // (api/messages) ne soit pas cassée par des retours à la ligne de la réponse GPS.
    String logMsg = message;
    logMsg.replace("\r", "");
    logMsg.replace("\n", "<br>");
    
    String line = ts + " [" + dir + "] (\"" + logMsg + "\")\n";

    File f = SD.open(file.c_str(), FILE_APPEND);
    if (f) {
        f.print(line);
        f.flush();
        f.close();
    }
}

String construireMessageGpsDetail() {
    char lat[12] = "N/A", lng[12] = "N/A";
    String altitude = "N/A", vitesse = "N/A", satellites = "0", heureFix = "N/A";

    if (gps.location.isValid()) {
        dtostrf(gps.location.lat(), 1, 6, lat);
        dtostrf(gps.location.lng(), 1, 6, lng);
    }
    if (gps.altitude.isValid()) altitude = String(gps.altitude.meters(), 1) + "m";
    if (gps.speed.isValid()) vitesse = String(gps.speed.kmph(), 1) + "km/h";
    if (gps.satellites.isValid()) satellites = String(gps.satellites.value());
    heureFix = getHeureHiverLisible();

    // Format compact pour ne pas dépasser les 160 caractères (limite stricte SMS)
    String msg = "Lat:" + String(lat) + "  Lng:" + String(lng) + "\n" +
                 "Alt:" + altitude + "  Vit:" + vitesse + "\n" +
                 "Sat:" + satellites + " | " + heureFix;

    if (gps.location.isValid()) {
        msg += "\nMap: https://maps.google.com/?q=" + String(lat) + "," + String(lng);
    }

    return msg;
}
void envoyerSMSSuivi(String cibles, String message) {
    int start = 0;
    while(start < cibles.length()) {
        int end = cibles.indexOf(',', start);
        if (end == -1) end = cibles.length();
        String num = cibles.substring(start, end);
        num.trim();
        if (num.length() >= 4) {
            if (modem.sendSMS(num, message)) saveToSD(num, message, true);
        }
        start = end + 1;
    }
}

void scanAndSaveSmsToSD() {
    modem.sendAT("+CMGF=1");
    modem.waitResponse(1000);
    String response = "";
    modem.sendAT("+CMGL=\"ALL\"");
    if (modem.waitResponse(10000UL, response) != 1 || response.indexOf("+CMGL:") == -1) return;

    int index = 0;
    while ((index = response.indexOf("+CMGL:", index)) != -1) {
        int ci = response.indexOf(",", index);
        int si = response.substring(index + 7, ci).toInt();
        int q1 = response.indexOf("\"", index);
        int q2 = response.indexOf("\"", q1 + 1);
        int q3 = response.indexOf("\"", q2 + 1);
        int q4 = response.indexOf("\"", q3 + 1);
        String sender = response.substring(q3 + 1, q4);
        int endHeader = response.indexOf("\r\n", q4);
        int nextCmgl = response.indexOf("+CMGL:", endHeader);
        String body = (nextCmgl != -1) ? response.substring(endHeader + 2, nextCmgl) : response.substring(endHeader + 2);
        body.replace("\r\n", " "); body.trim();
        if (body.endsWith("OK")) body = body.substring(0, body.length() - 2);
        body.trim();

        if (body.length() > 0 && sender.length() > 3) {
            saveToSD(sender, body, false);

            String bodyLow = body; bodyLow.toLowerCase();
            if (bodyLow.indexOf("gps") != -1) {
                bool authorized = true;
                if (cfgNumAutorisesGps.length() > 0) {
                    String cleanSender = formatPhone(sender);
                    if (cleanSender.length() >= 9) cleanSender = cleanSender.substring(cleanSender.length() - 9);
                    authorized = (cfgNumAutorisesGps.indexOf(cleanSender) != -1);
                }
                
                if (authorized) {
                    String msgGPS = construireMessageGpsDetail();
                    if (modem.sendSMS(sender, msgGPS)) {
                        saveToSD(sender, msgGPS, true);
                    }
                }
            }
            modem.sendAT("+CMGD=" + String(si));
            modem.waitResponse(1000);
        }
        index = endHeader;
    }
}


// ════════════════════════════════════════════
//  🎨 CSS ET THEMES
// ════════════════════════════════════════════
const char STYLE_CSS[] PROGMEM = R"rawliteral(
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',Arial,sans-serif;background:#0d0d0d;color:#e0e0e0;padding:15px;padding-bottom:85px;}
  .container{max-width:500px;margin:0 auto;background:#1a1a1a;border-radius:16px;overflow:hidden;box-shadow:0 10px 30px rgba(0,0,0,0.7);border:1px solid #262626}
  .header{background:#111;color:#00ff88;padding:18px;font-size:1.15rem;font-weight:bold;text-align:center;position:relative;border-bottom:2px solid #00ff88;text-shadow:0 0 10px rgba(0,255,136,0.3)}
  .back-btn{position:absolute;left:15px;top:50%;transform:translateY(-50%);color:#00ff88;text-decoration:none;font-size:1.4rem}
  .header-actions{position:absolute;right:15px;top:50%;transform:translateY(-50%);display:flex;gap:8px}
  .icon-btn{color:#00ff88;text-decoration:none;font-size:1rem;padding:6px 10px;background:#14291f;border-radius:8px;border:1px solid #00ff88;}
  .big-clock{text-align:center;padding:30px 10px;background:#141414;border-bottom:1px solid #262626}
  .big-clock .time{font-size:3.5rem;font-weight:bold;color:#00ff88;text-shadow:0 0 15px rgba(0,255,136,0.4)}
  .big-clock .subtitle{font-size:0.95rem;color:#888;margin-top:5px}
  .list-item-container{display:flex;align-items:center;border-bottom:1px solid #262626;}
  .list-item{flex:1;display:flex;flex-direction:column;padding:16px;color:#fff;text-decoration:none}
  .delete-btn{background:transparent;border:none;color:#ff4d4d;font-size:1.25rem;padding:16px;cursor:pointer;}
  .chat-box{height:420px;overflow-y:auto;padding:15px;background:#111;display:flex;flex-direction:column;gap:10px}
  .msg{max-width:80%;padding:12px 16px;border-radius:14px;word-wrap:break-word;font-size:0.95rem;line-height:1.4;}
  .msg.sent{background:linear-gradient(135deg, #0d261a, #143d29);border:1px solid #00ff88;color:#00ff88;align-self:flex-end;border-bottom-right-radius:2px}
  .msg.received{background:#262626;color:#ffffff;align-self:flex-start;border-bottom-left-radius:2px;border:1px solid #333}
  .input-area{display:flex;padding:15px;background:#141414;border-top:1px solid #262626;gap:10px}
  .input-area input{flex:1;padding:14px;background:#1f1f1f;border-radius:10px;color:#fff;outline:none;border:1px solid #333;}
  .input-area button{background:#00ff88;color:#000;border:none;padding:0 20px;border-radius:10px;font-weight:bold;cursor:pointer;}
  .form-wrap{padding:25px;display:flex;flex-direction:column;gap:15px}
  .form-wrap label{color:#aaa;font-size:0.9rem;margin-bottom:-5px}
  .form-wrap input,.form-wrap textarea{padding:14px;background:#1f1f1f;border:1px solid #333;border-radius:10px;color:#fff;outline:none;}
  .form-wrap button{padding:14px;background:#00ff88;color:#000;border:none;border-radius:10px;font-weight:bold;}
  .gps-row{display:flex;justify-content:space-between;align-items:center;padding:16px;border-bottom:1px solid #262626;font-size:0.95rem;background:#1c1c1c}
  .gps-row span:first-child{color:#999}
  .gps-row span:last-child{color:#fff;font-weight:bold}
  .mode-bar {display: flex; gap: 12px; padding: 15px; background: #111; border-top: 1px solid #262626; position: fixed; bottom: 0; left: 0; right: 0; z-index: 999}
  .mode-btn {flex: 1; padding: 14px; border: none; border-radius: 10px; font-weight: bold; font-size: 0.95rem; cursor: pointer; text-align:center; text-decoration: none;}
  .mode-wifi { background: #00ff88; color: #000; }
  .mode-eco { background: #ff3333; color: #fff; }
  .new-btn{display:block;text-align:center;padding:16px;background:#00ff88;color:#000;text-decoration:none;font-weight:bold;}
  .add-btn{display:block;text-align:center;padding:14px;background:#1f1f1f;color:#00ff88;text-decoration:none;font-weight:bold;border-top:1px solid #262626}
</style>
)rawliteral";


// ════════════════════════════════════════════
//  🌐 PAGES SERVEUR WEB
// ════════════════════════════════════════════

void handleRoot() { server.sendHeader("Location", "/numero"); server.send(303); }

void handleSupprimer() {
    if (server.hasArg("phone")) {
        String phone = formatPhone(server.arg("phone"));
        SD.remove("/" + phone + ".txt");
        setNickname(phone, "");
    }
    server.sendHeader("Location", "/numero"); server.send(303);
}

void handleSwitchWifi() {
    if (systemMode == 1) ecrireModeSurSD(0); else ecrireModeSurSD(1);
    server.send(200, "text/html", "<body style='background:#0d0d0d;color:#00ff88;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Redémarrage en cours... Reconnectez-vous dans 15s.</h2></body>");
    delay(2000);
    ESP.restart();
}

void handleSwitchEco() {
    ecrireModeSurSD(2); 
    server.send(200, "text/html", "<body style='background:#0d0d0d;color:#ff3333;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Activation du mode Éco... Le WiFi est coupé.</h2></body>");
    delay(2000);
    ESP.restart();
}

void handleToggleSuivi() {
    suiviActif = !suiviActif;
    if (suiviActif) { dernierSmsSuivi = millis() - 30000; } // Force le premier SMS immédiat
    server.sendHeader("Location", "/numero");
    server.send(303);
}

void handleParametres() {
    if (server.method() == HTTP_POST) {
        cfgNumSuiviMode = formatPhone(server.arg("numSuivi"));
        String numAut = server.arg("numGps"); numAut.replace(" ", ""); numAut.trim();
        cfgNumAutorisesGps = numAut;
        ecrireConfigSurSD();
        server.sendHeader("Location", "/numero");
        server.send(303);
        return;
    }
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'>";
    html += "<div class='header'><a href='/numero' class='back-btn'>←</a> Paramètres</div>";
    html += "<form action='/parametres' method='POST' class='form-wrap'>";
    html += "<label>Numéro ciblé par le mode Éco & Suivi 30s</label>";
    html += "<p style='color:#777;font-size:0.8rem;margin-bottom:10px;'>Séparez par la virgule pour cibler plusieurs téléphones (ex: 0611,0622).</p>";
    html += "<input type='text' name='numSuivi' value='" + cfgNumSuiviMode + "' required>";
    html += "<label style='margin-top:10px;'>Numéros autorisés à pinger 'gps'</label>";
    html += "<p style='color:#777;font-size:0.8rem;margin-bottom:10px;'>Séparez par la virgule (ex: 0611,0622). Laissez vide pour autoriser le monde entier.</p>";
    html += "<input type='text' name='numGps' value='" + cfgNumAutorisesGps + "'>";
    html += "<button type='submit' style='margin-top:20px;'>💾 Sauvegarder</button>";
    html += "</form></div></body></html>";
    server.send(200, "text/html", html);
}

void handleNumeroList() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'><div class='header'>Boîte de Réception";
    html += "<div class='header-actions'><a href='/gps' class='icon-btn'>🛰️</a> <a href='/parametres' class='icon-btn'>⚙️</a></div></div>";

    String printTime = "00:00";
    String dt = getHeureHiverLisible();
    if (dt != "Fix GPS requis") printTime = dt.substring(0, 5);

    html += "<div class='big-clock'><div class='time'>" + printTime + "</div><div class='subtitle'>WiFi : " + String(systemMode == 0 ? "Point d'Accès" : "Routeur Box") + "</div></div>";

    File root = SD.open("/");
    File file = root.openNextFile();
    bool found = false;

    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            if (name.endsWith(".txt") && name != "contacts.txt" && name != "mode.txt" && name != "autorise.txt" && name != "config.txt" && name != "gps_log.txt") {
                found = true;
                String phone = name.substring(0, name.length() - 4);
                if (phone.startsWith("/")) phone = phone.substring(1);
                String nick = getNickname(phone);
                
                html += "<div class='list-item-container'>";
                html += "<a href='/numero/" + phone + "' class='list-item'><div>" + (nick.length()>0 ? nick : "+"+phone) + "</div></a>";
                html += "<button class='delete-btn' onclick=\"if(confirm('Supprimer cette conversation ?')) window.location.href='/supprimer?phone=" + phone + "';\">🗑️</button>";
                html += "</div>";
            }
        }
        file = root.openNextFile();
    }
    if (!found) html += "<div style='text-align:center; padding: 25px; color:#666;'>Aucun contact trouvé.</div>";
    
    html += "<a href='/ajouter' class='add-btn'>+ Ajouter un contact</a>";
    html += "<a href='/nouveau' class='new-btn'>✏️ Écrire un SMS</a>";
    html += "</div>";

    html += "<div class='mode-bar'>";
    html += "  <a href='/mode_wifi' class='mode-btn mode-wifi' style='font-size:0.8rem;padding:12px 2px;'>🌐 " + String(systemMode == 0 ? "Box Arg" : "WiFi AP") + "</a>";
    if (!suiviActif) {
        html += "  <a href='/toggle_suivi' class='mode-btn' style='background:#ff9900;color:#000;font-size:0.8rem;padding:12px 2px;'>📍 Suivi OFF</a>";
    } else {
        html += "  <a href='/toggle_suivi' class='mode-btn' style='background:#ff0000;color:#fff;font-size:0.8rem;padding:12px 2px;'>🛑 Suivi ON</a>";
    }
    html += "  <a href='/mode_eco' class='mode-btn mode-eco' style='font-size:0.8rem;padding:12px 2px;' onclick=\"return confirm('Attention : le WiFi sera coupé ! On continue ?');\">🔋 ÉCO</a>";
    html += "</div>";

    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleNumeroChat() {
    String path = server.uri();
    String cp = formatPhone(path.substring(8));
    String nick = getNickname(cp);

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'>";
    html += "<div class='header'><a href='/numero' class='back-btn'>←</a> " + (nick.length()>0 ? nick : "+"+cp);
    html += "<div class='header-actions'><a href='/renommer?phone=" + cp + "' class='icon-btn'>✏️</a></div></div>";

    html += "<div class='chat-box' id='chatBox'></div>";
    html += "<div class='input-area'><input type='text' id='msgInput' placeholder='Écrire un message...' onkeydown='if(event.key===\"Enter\") sendMsg()'><button onclick='sendMsg()'>Envoyer</button></div>";
    html += "</div>";

    html += "<script>"
            "var phone='" + cp + "'; var lastLen = 0;"
            "function load() {"
            "  fetch('/api/messages?phone='+phone).then(r=>r.json()).then(d=>{"
            "    if(d.messages.length!==lastLen){"
            "      document.getElementById('chatBox').innerHTML=''; d.messages.forEach(m=>{"
            "        var dv = document.createElement('div'); dv.className='msg '+(m.sent?'sent':'received');"
            "        dv.innerHTML=m.text+'<br><span style=\"font-size:10px;color:#777;display:block;text-align:right\">'+m.time+'</span>';"
            "        document.getElementById('chatBox').appendChild(dv);"
            "      }); lastLen=d.messages.length;"
            "      document.getElementById('chatBox').scrollTop=document.getElementById('chatBox').scrollHeight;"
            "    }"
            "  });"
            "}"
            "function sendMsg(){"
            "  var i=document.getElementById('msgInput'); var m=i.value.trim(); if(!m) return;"
            "  fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "  body:'phone=%2B'+phone+'&message='+encodeURIComponent(m)}).then(()=>{i.value=''; load();});"
            "}"
            "window.onload=load; setInterval(load, 2500);"
            "</script></body></html>";
    server.send(200, "text/html", html);
}

void handleRenommer() {
    if (server.method() == HTTP_POST) {
        String phone = formatPhone(server.arg("phone"));
        String nick = server.arg("nickname"); nick.trim();
        setNickname(phone, nick);
        server.sendHeader("Location", "/numero/" + phone);
        server.send(303);
        return;
    }
    String phone = formatPhone(server.arg("phone"));
    String currentNick = getNickname(phone);

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'>";
    html += "<div class='header'><a href='/numero/" + phone + "' class='back-btn'>←</a> Renommer</div>";
    html += "<form action='/renommer' method='POST' class='form-wrap'>";
    html += "<input type='hidden' name='phone' value='" + phone + "'>";
    html += "<label>Nouveau Surnom</label>";
    html += "<input type='text' name='nickname' value='" + currentNick + "' placeholder='Ex: Maman, Papa...' required>";
    html += "<button type='submit'>💾 Sauvegarder</button>";
    html += "</form></div></body></html>";
    server.send(200, "text/html", html);
}

void handleApiMessages() {
    if (!server.hasArg("phone")) { server.send(400, "application/json", "{}"); return; }
    String cp = formatPhone(server.arg("phone"));
    String fname = "/" + cp + ".txt";

    String json = "{\"messages\":[";
    bool first = true;

    if (SD.exists(fname)) {
        File f = SD.open(fname, FILE_READ);
        while (f.available()) {
            String line = f.readStringUntil('\n');
            if (line.length() <= 14) continue;
            bool sent = line.indexOf("[OUT]") != -1;
            int s = line.indexOf("(\"") + 2; int e = line.lastIndexOf("\")");
            if (s < 2 || e < 0) continue;
            String txt = line.substring(s, e);
            String ts = line.substring(0, 12);
            String fmtTime = ts.substring(6, 8) + ":" + ts.substring(8, 10) + " " + ts.substring(4, 6) + "/" + ts.substring(2, 4);
            txt.replace("\"", "\\\"");

            if (!first) json += ",";
            json += "{\"sent\":" + String(sent ? "true" : "false") + ",\"text\":\"" + txt + "\",\"time\":\"" + fmtTime + "\"}";
            first = false;
        }
        f.close();
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleApiSend() {
    if (server.hasArg("phone") && server.hasArg("message")) {
        String phone = server.arg("phone");
        String message = server.arg("message");
        if (modem.sendSMS(phone, message)) saveToSD(phone, message, true);
        server.send(200, "application/json", "{\"ok\":true}");
    }
}

void handleNouveau() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'><div class='header'><a href='/numero' class='back-btn'>←</a> Nouveau Message</div>"
            "<form action='/send_direct' method='POST' class='form-wrap'>"
            "<label>Numéro ciblé (ex: +33...)</label><input type='text' name='phone' placeholder='+33...' required>"
            "<label>Message</label><textarea name='message' placeholder='Écrire...' required onkeydown='if(event.key===\"Enter\" && !event.shiftKey){event.preventDefault();this.form.submit();}'></textarea>"
            "<button type='submit'>🚀 Envoyer</button></form></div></body></html>";
    server.send(200, "text/html", html);
}

void handleSendDirect() {
    if (server.hasArg("phone") && server.hasArg("message")) {
        String phone = server.arg("phone"); String message = server.arg("message");
        if (modem.sendSMS(phone, message)) saveToSD(phone, message, true);
    }
    server.sendHeader("Location", "/numero/" + formatPhone(server.arg("phone"))); server.send(303);
}

void handleAjouter() {
    if (server.method() == HTTP_POST) {
        String phone = formatPhone(server.arg("phone"));
        String file = "/" + phone + ".txt";
        if (!SD.exists(file)) { File f = SD.open(file.c_str(), FILE_WRITE); if (f) f.close(); }
        server.sendHeader("Location","/numero"); server.send(303); return;
    }
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'><div class='header'><a href='/numero' class='back-btn'>←</a> Créer une Fiche</div>"
            "<form action='/ajouter' method='POST' class='form-wrap'>"
            "<label>Numéro de téléphone</label><input type='text' name='phone' placeholder='+33...' required>"
            "<button type='submit'>💾 Sauvegarder</button></form></div></body></html>";
    server.send(200, "text/html", html);
}

void handleWebGPS() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
    html += STYLE_CSS;
    html += "</head><body><div class='container'><div class='header'><a href='/numero' class='back-btn'>←</a> Informations GPS</div>";

    char lat[12] = "N/A", lng[12] = "N/A";
    String altitude = "N/A", vitesse = "N/A", satellites = "0", heureFix = "N/A";

    if (gps.location.isValid()) {
        dtostrf(gps.location.lat(), 1, 6, lat);
        dtostrf(gps.location.lng(), 1, 6, lng);
    }
    if (gps.altitude.isValid()) altitude = String(gps.altitude.meters(), 1) + " m";
    if (gps.speed.isValid()) vitesse = String(gps.speed.kmph(), 1) + " km/h";
    if (gps.satellites.isValid()) satellites = String(gps.satellites.value());
    heureFix = getHeureHiverLisible();

    html += "<div class='gps-row'><span>📍 Latitude</span><span>" + String(lat) + "</span></div>";
    html += "<div class='gps-row'><span>📍 Longitude</span><span>" + String(lng) + "</span></div>";
    html += "<div class='gps-row'><span>🏔️ Altitude</span><span>" + altitude + "</span></div>";
    html += "<div class='gps-row'><span>🏎️ Vitesse</span><span>" + vitesse + "</span></div>";
    html += "<div class='gps-row'><span>🛰️ Satellites vus</span><span style='color:#00ff88'>" + satellites + "</span></div>";
    html += "<div class='gps-row'><span>🕒 Horodatage Fix</span><span>" + heureFix + "</span></div>";

    html += "<div class='form-wrap'>";
    if (gps.location.isValid()) {
        html += "<a href='http://maps.google.com/?q=" + String(lat) + "," + String(lng) + "' target='_blank' style='display:block;width:100%;text-decoration:none;text-align:center;padding:14px;background:#00ff88;color:#000;border-radius:10px;font-weight:bold'>🌍 Google Maps</a>";
    }
    html += "<a href='/gps' class='add-btn' style='margin-top:10px;text-align:center;border-radius:10px;border:1px solid #262626'>🔄 Rafraîchir les valeurs</a>";
    html += "</div></div></body></html>";

    server.send(200, "text/html", html);
}


// ════════════════════════════════════════════
//  🏁 SETUP ET BOUCLE PRINCIPALE
// ════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== BOOT ESP32 ===");

    SPI.begin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN);
    SD.begin(BOARD_SD_CS_PIN);

    lireModeDepuisSD();
    lireConfigDepuisSD();

    SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);

    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW); delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    while (!modem.testAT()) delay(500);
    modem.sendAT("+CMGF=1"); modem.waitResponse(1000);

    scanAndSaveSmsToSD();

    if (systemMode == 0) {
        WiFi.softAP(ap_ssid, ap_pass);
        wifiOk = true;
    } else if (systemMode == 1) {
        WiFi.begin(ssid, password);
        unsigned long wt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wt < 8000) { delay(500); }
        if (WiFi.status() == WL_CONNECTED) {
            wifiOk = true;
        } else {
            // Sauvetage : Impossible de joindre ARG, on rebascule en Mode 0 (Point d'accès interne)
            systemMode = 0;
            ecrireModeSurSD(0);
            WiFi.disconnect();
            WiFi.softAP(ap_ssid, ap_pass);
            wifiOk = true;
        }
    } else if (systemMode == 2) {
        WiFi.mode(WIFI_OFF);
        wifiOk = false;
    }

    if (wifiOk) {
        server.on("/", handleRoot);
        server.on("/numero", handleNumeroList);
        server.on("/gps", handleWebGPS);
        server.on("/supprimer", handleSupprimer);
        server.on("/nouveau", handleNouveau);
        server.on("/ajouter", handleAjouter);
        server.on("/renommer", handleRenommer);
        server.on("/mode_wifi", handleSwitchWifi);
        server.on("/mode_eco", handleSwitchEco);
        server.on("/toggle_suivi", handleToggleSuivi);
        server.on("/parametres", handleParametres);
        server.on("/api/messages", handleApiMessages);
        server.on("/api/send", handleApiSend);
        server.on("/send_direct", HTTP_POST, handleSendDirect);
        server.onNotFound([]() { if (server.uri().startsWith("/numero/")) handleNumeroChat(); else server.send(404, "text/plain", "Not found"); });
        server.begin();
    }
}

void loop() {
    while (SerialGPS.available()) {
        int c = SerialGPS.read();
        gps.encode(c);
        if (gps.location.isUpdated()) {
            hadFix = true;
            lastValidFix = millis();
        }
    }

    if (wifiOk) server.handleClient();

    if (millis() - lastSmsCheck >= SMS_CHECK_INTERVAL) {
        lastSmsCheck = millis();
        scanAndSaveSmsToSD();
    }

    if (systemMode == 2 && millis() - lastEcoSms >= ECO_SMS_INTERVAL) {
        lastEcoSms = millis();
        String msgEco = construireMessageGpsDetail();
        envoyerSMSSuivi(cfgNumSuiviMode, msgEco);
    }

    if (systemMode != 2 && suiviActif && millis() - dernierSmsSuivi >= 30000) {
        dernierSmsSuivi = millis();
        String msgSuivi = construireMessageGpsDetail();
        envoyerSMSSuivi(cfgNumSuiviMode, msgSuivi);
    }
}