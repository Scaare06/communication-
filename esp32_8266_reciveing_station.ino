#include <ESP8266WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>

extern "C" {
  #include "user_interface.h"
}

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

uint16_t myNodeId = 32441; 
uint32_t sequence = 0;
float testAngle = 0.0;

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

volatile bool newMessageAvailable = false;
char globalMsgBuffer[64] = {0};
unsigned long messageDisplayTimer = 0;
unsigned long lastBeaconTime = 0;

void sniffer_callback(uint8_t *buf, uint16_t len) {
    // Bulletproof Memory Scan: Ignore bugged SDK lengths and force-scan the first 64 bytes
    for (uint16_t i = 0; i < 64; i++) {
        if (buf[i] == 0xE0) {
            TextCommandPacket incoming;
            memcpy(&incoming, buf + i, sizeof(TextCommandPacket));
            
            if (incoming.securityKey == 0x99FF88BB && incoming.targetNodeId == myNodeId) {
                strncpy(globalMsgBuffer, incoming.messageText, 63);
                globalMsgBuffer[63] = '\0'; 
                newMessageAvailable = true;
                return; 
            }
        }
    }
}

void showDefaultScreen() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr); 
    
    u8g2.drawStr(12, 28, "SATELLITE - ONLINE");
    u8g2.setCursor(22, 45);
    u8g2.print("ID NO: SAT-");
    u8g2.print(myNodeId);
    
    u8g2.sendBuffer();
}

void showMessageScreen(const char* msg) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    
    u8g2.drawStr(0, 12, ">>> INCOMING MSG");
    u8g2.drawLine(0, 16, 128, 16); 
    
    int y = 30;
    char lineBuf[22];
    int i = 0;
    
    while (msg[i] != '\0' && y < 64) {
        int j = 0;
        while (msg[i] != '\0' && j < 20) { 
            lineBuf[j++] = msg[i++];
        }
        lineBuf[j] = '\0';
        u8g2.drawStr(0, y, lineBuf);
        y += 12; 
    }
    
    u8g2.sendBuffer();
}

void setup() {
    Serial.begin(115200);
    u8g2.begin();
    showDefaultScreen();

    WiFi.mode(WIFI_STA);
    wifi_set_channel(1); 
    wifi_set_promiscuous_rx_cb(sniffer_callback);
    wifi_promiscuous_enable(1);
}

void loop() {
    if (millis() - lastBeaconTime > 1000) {
        lastBeaconTime = millis();
        
        sequence++;
        testAngle += 0.1;
        if(testAngle > 6.28) testAngle = 0.0;

        CustomDataPacket beaconPacket;
        beaconPacket.frameHeader = 0xD0;
        beaconPacket.nodeId = myNodeId;
        beaconPacket.packetId = sequence;
        beaconPacket.valA = testAngle;
        beaconPacket.valB = random(25, 45); 
        beaconPacket.checksum = 0xAA55FFAA; 

        uint8_t rawFrame[24 + sizeof(CustomDataPacket)] = {0};
        rawFrame[0] = 0x40; 
        rawFrame[1] = 0x00;
        for(int i = 4; i < 10; i++) rawFrame[i] = 0xFF; 
        for(int i = 10; i < 22; i++) rawFrame[i] = 0x22; 
        
        memcpy(rawFrame + 24, &beaconPacket, sizeof(CustomDataPacket));

        wifi_promiscuous_enable(0);
        wifi_send_pkt_freedom(rawFrame, sizeof(rawFrame), 0);
        wifi_promiscuous_enable(1);
    }

    if (newMessageAvailable) {
        showMessageScreen(globalMsgBuffer);
        messageDisplayTimer = millis(); 
        newMessageAvailable = false;
    }

    if (messageDisplayTimer != 0 && (millis() - messageDisplayTimer > 10000)) {
        showDefaultScreen();
        messageDisplayTimer = 0; 
    }
}