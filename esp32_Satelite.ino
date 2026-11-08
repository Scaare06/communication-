#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <U8g2lib.h>

// Using SH1106 constructor based on the display edge glitch from your photo.
// (If you use a different OLED for the ESP32, change to U8G2_SSD1306_128X64_NONAME_F_HW_I2C)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

#define WIFI_CHANNEL 1

// --- STRUCTS ---
struct __attribute__((packed)) CustomDataPacket {
    uint8_t frameHeader;
    uint16_t nodeId;
    uint32_t packetId;
    float valA; 
    float valB; 
    uint32_t checksum;
};

struct __attribute__((packed)) TextCommandPacket {
    uint8_t frameHeader; 
    uint16_t targetNodeId; 
    char messageText[64];  
    uint32_t securityKey;  
};

struct NodeTracker {
    uint16_t id;
    float radius;
    float angle;
    uint32_t lastSeen;
    bool active;
};

// --- GLOBALS ---
WebServer server(80);
NodeTracker nodes[6];
String lastTransmittedMessage = "AWAITING COMMANDS...";
unsigned long lastDisplayUpdate = 0;

// --- SNIFFER CALLBACK ---
void snifferHandler(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (len < sizeof(CustomDataPacket)) return;
    
    for (int offset = 0; offset <= (len - sizeof(CustomDataPacket)); offset++) {
        CustomDataPacket* incoming = (CustomDataPacket*)(payload + offset);
        if (incoming->frameHeader == 0xD0 && incoming->checksum == 0xAA55FFAA) {
            int slot = -1;
            for (int i = 0; i < 6; i++) {
                if (nodes[i].id == incoming->nodeId && nodes[i].active) { slot = i; break; }
                if (!nodes[i].active && slot == -1) { slot = i; }
            }
            if (slot != -1) {
                nodes[slot].id = incoming->nodeId;
                nodes[slot].radius = incoming->valB; 
                nodes[slot].angle = incoming->valA;  
                nodes[slot].lastSeen = millis();
                nodes[slot].active = true;
            }
            break; 
        }
    }
}

// --- DISPLAY UPDATE LOOP ---
void updatePhysicalDashboard() {
    u8g2.clearBuffer();
    // Tiny 5x7 font to cram as much data on screen as possible
    u8g2.setFont(u8g2_font_5x7_tr); 
    
    // Header
    u8g2.drawStr(0, 7, "[ ORBITAL HQ SERVER ]");
    u8g2.drawStr(0, 16, "IP: 192.168.4.1 (WIFI)");
    u8g2.drawLine(0, 19, 128, 19);

    // Active Nodes
    u8g2.drawStr(0, 28, "ACTIVE SATELLITES:");
    int yPos = 37;
    int activeCount = 0;
    
    for(int i = 0; i < 6; i++) {
        if(nodes[i].active && (millis() - nodes[i].lastSeen < 15000)) {
            char nodeInfo[32];
            sprintf(nodeInfo, "> SAT-%d [R:%02d A:%01.1f]", nodes[i].id, (int)nodes[i].radius, nodes[i].angle);
            u8g2.drawStr(0, yPos, nodeInfo);
            yPos += 9;
            activeCount++;
        }
    }
    
    if (activeCount == 0) {
        u8g2.drawStr(0, 37, "> NO SATELLITES DETECTED");
    }

    // Footer / Last Transmit
    u8g2.drawLine(0, 53, 128, 53);
    u8g2.drawStr(0, 62, "TX: ");
    
    // Truncate message to fit on the tiny screen if too long
    String shortMsg = lastTransmittedMessage;
    if (shortMsg.length() > 22) shortMsg = shortMsg.substring(0, 20) + "..";
    u8g2.setCursor(18, 62);
    u8g2.print(shortMsg);
    
    u8g2.sendBuffer();
}

// --- WEB ENDPOINTS ---
void handleDataFeed() {
    String json = "[";
    for(int i=0; i<6; i++) {
        if(nodes[i].active && (millis() - nodes[i].lastSeen < 15000)) {
            json += "{\"id\":" + String(nodes[i].id) + ",\"r\":" + String(nodes[i].radius) + ",\"a\":" + String(nodes[i].angle) + "},";
        }
    }
    if(json.endsWith(",")) json.remove(json.length()-1);
    json += "]";
    server.send(200, "application/json", json);
}

void handleSendMessage() {
    if (server.hasArg("msg") && server.hasArg("target")) {
        String msgStr = server.arg("msg");
        uint16_t targetId = server.arg("target").toInt();
        
        // Update the physical OLED with the new message
        lastTransmittedMessage = msgStr;
        updatePhysicalDashboard();

        uint8_t rawFrame[24 + sizeof(TextCommandPacket)] = {0};
        rawFrame[0] = 0x40; // Probe Request
        rawFrame[1] = 0x00;
        for(int i=4; i<10; i++) rawFrame[i] = 0xFF; 
        for(int i=10; i<16; i++) rawFrame[i] = 0x22; 
        for(int i=16; i<22; i++) rawFrame[i] = 0x22; 
        
        TextCommandPacket *txPacket = (TextCommandPacket*)(rawFrame + 24);
        txPacket->frameHeader = 0xE0; 
        txPacket->targetNodeId = targetId;
        txPacket->securityKey = 0x99FF88BB; 
        msgStr.toCharArray(txPacket->messageText, 64);

        esp_wifi_set_promiscuous(false);
        for(int i = 0; i < 15; i++) {
            esp_wifi_80211_tx(WIFI_IF_STA, rawFrame, sizeof(rawFrame), false);
            delay(10);
        }
        esp_wifi_set_promiscuous(true);

        server.send(200, "text/plain", "SUCCESS");
    } else {
        server.send(400, "text/plain", "BAD REQUEST");
    }
}

// --- HTML ASSETS ---
const char LOGIN_PORTAL[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'><title>ACCESS GATE</title><style>body{background:#02060d;color:#00ffcc;font-family:monospace;margin:0;display:flex;flex-direction:column;justify-content:center;align-items:center;height:100vh;}input{background:#071424;border:1px solid #00ffcc;padding:12px;color:#00ffcc;text-align:center;font-size:18px;outline:none;border-radius:4px;margin-bottom:15px;}button{background:#00ffcc;border:none;padding:12px 24px;color:#02060d;font-weight:bold;cursor:pointer;border-radius:4px;}</style></head><body><h2>ENTER ACCESS MATRIX CODE</h2><form action="/login" method="POST"><input type="password" name="passkey" placeholder="****" required><br><button type="submit">DECRYPT INTERFACE</button></form></body></html>
)rawliteral";

const char RADAR_DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name='viewport' content='width=device-width, initial-scale=1.0, user-scalable=no'><title>ORBITAL HQ</title><style>body { background:#030914; color:#00ffcc; font-family:monospace; margin:0; padding:10px; text-align:center; }#radar-scope { margin: 10px auto; display: block; border: 1px solid #005544; border-radius:50%; box-shadow: 0 0 20px #00ffcc33; background: radial-gradient(circle, #071724 0%, #030a12 100%); }#message-deck { background: #05111f; border: 1px solid #005544; padding: 15px; border-radius: 8px; max-width: 340px; margin: 15px auto; }input[type="text"], select { background:#071424; border:1px solid #00ffcc; padding:10px; color:#00ffcc; width: 85%; border-radius:4px; font-family:monospace; margin-bottom:10px; font-size:14px; }button { background:#00ffcc; border:none; padding:10px 20px; color:#02060d; font-weight:bold; cursor:pointer; border-radius:4px; width:85%; }.system-logs { font-size:11px; color:#00aa88; height:50px; overflow-y:scroll; text-align:left; padding:10px; border-top:1px solid #003322; }</style></head><body onload="initRadar()"><h1 style="font-size: 16px; margin: 15px 0 15px 0; letter-spacing: 2px;">SATELLITE DEPLOYMENT NETWORK</h1><canvas id="radar-scope" width="300" height="300"></canvas><div id="message-deck"><h3 style="margin-top:0; font-size:12px; color:#ff3366; letter-spacing: 1px;">💬 TRANSMIT ENCRYPTED COMMS</h3><select id="target-node"><option value="">-- SELECT TARGET SATELLITE --</option></select><br><input type="text" id="comms-text" placeholder="Type message details..."><br><button onclick="sendCommsMessage()">BURST TARGET TRANSMIT</button></div><div class="system-logs" id="logbox">> SECURE CORE DASHBOARD LINKED.</div><script>function sendCommsMessage() {const input = document.getElementById('comms-text');const target = document.getElementById('target-node').value;if(!input.value.trim() || !target) { alert("Select a target satellite and enter text."); return; }fetch('/send-msg?msg=' + encodeURIComponent(input.value.trim()) + '&target=' + target).then(res => {if(res.ok) {document.getElementById('logbox').innerHTML += "<br>> BURST SUCCESS TO SAT-" + target + ": " + input.value;input.value = "";}});}function initRadar() {const canvas = document.getElementById('radar-scope'); const ctx = canvas.getContext('2d');let scanAngle = 0; let satelliteNodes = [];setInterval(() => {fetch('/data-feed').then(res => res.json()).then(data => { satelliteNodes = data; const dropdown = document.getElementById('target-node');const selectedValue = dropdown.value;dropdown.innerHTML = '<option value="">-- SELECT TARGET SATELLITE --</option>';data.forEach(node => {dropdown.innerHTML += `<option value="${node.id}">SAT-${node.id}</option>`;});dropdown.value = selectedValue;});}, 1000);function drawScope() {ctx.clearRect(0,0,300,300); const cx = 150, cy = 150; ctx.strokeStyle = '#003322';for(let r=30; r<=120; r+=30) { ctx.beginPath(); ctx.arc(cx,cy,r,0,Math.PI*2); ctx.stroke(); }scanAngle += 0.02; ctx.strokeStyle = 'rgba(0, 255, 204, 0.15)';ctx.beginPath(); ctx.moveTo(cx,cy); ctx.lineTo(cx + Math.cos(scanAngle)*120, cy + Math.sin(scanAngle)*120); ctx.stroke();satelliteNodes.forEach(node => {let rad = Math.min(110, node.r * 3); let x = cx + Math.cos(node.a) * rad; let y = cy + Math.sin(node.a) * rad;ctx.fillStyle = '#ff3366'; ctx.beginPath(); ctx.arc(x, y, 5, 0, Math.PI*2); ctx.fill();ctx.fillStyle = '#00ffcc'; ctx.font = '10px monospace'; ctx.fillText("SAT-" + node.id, x + 8, y - 4);});requestAnimationFrame(drawScope);}drawScope();}</script></body></html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", LOGIN_PORTAL); }
void handleLoginProcessing() {
    if (server.hasArg("passkey") && server.arg("passkey") == "7777") { server.send(200, "text/html", RADAR_DASHBOARD); } 
    else { server.send(401, "text/html", "ACCESS REJECTED."); }
}

// --- SETUP & LOOP ---
void setup() {
    Serial.begin(115200);
    
    // Initialize Screen
    u8g2.begin();
    u8g2.setContrast(255);
    updatePhysicalDashboard();
    
    // Network Setup
    WiFi.mode(WIFI_AP_STA); 
    WiFi.softAP("STEALTH_BASE_STATION", "CORP889900");
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&snifferHandler);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    
    // Web Server Setup
    server.on("/", handleRoot); 
    server.on("/login", HTTP_POST, handleLoginProcessing); 
    server.on("/data-feed", handleDataFeed); 
    server.on("/send-msg", handleSendMessage); 
    server.begin();
}

void loop() { 
    server.handleClient(); 
    
    // Refresh OLED dashboard every 1 second
    if (millis() - lastDisplayUpdate > 1000) {
        lastDisplayUpdate = millis();
        updatePhysicalDashboard();
    }
}