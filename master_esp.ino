/*
 * ============================================================
 *  MASTER ESP32 — Mesh Hub + Serial Bridge  (FIXED)
 *  Fixes:
 *   1. CHAT messages relayed to recipient drone (cross-drone chat)
 *   2. VOL_REQ handled — sends global volunteer list back to drone
 *   3. MeshMessage struct synced with slave (added toUserId field)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>

#define SERIAL_BAUD      115200
#define MAX_SLAVES       10
#define HEARTBEAT_MS     3000
#define SLAVE_TIMEOUT_MS 9000

// ── Structures (must match slave exactly) ───────────────────
struct SlaveInfo {
  uint8_t  mac[6];
  char     droneId[8];
  bool     active;
  uint32_t lastSeen;
};

struct MeshMessage {
  char     type[16];
  char     from[16];
  char     to[16];
  char     userId[24];      // sender uid
  char     toUserId[24];    // recipient uid (for CHAT)
  char     payload[180];
  uint32_t ts;
};

// ── Volunteer registry (global across all drones) ────────────
struct VolRecord {
  char uid[24];
  char drone[8];
  char service[8];
};

#define MAX_VOLS 60
VolRecord volunteers[MAX_VOLS];
int       volCount = 0;

// ── Slave registry ───────────────────────────────────────────
SlaveInfo slaves[MAX_SLAVES];
int       slaveCount = 0;
uint32_t  lastHeartbeat = 0;

// ── Helpers ──────────────────────────────────────────────────
String macToStr(const uint8_t* mac) {
  char buf[18];
  snprintf(buf,18,"%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}
bool macEqual(const uint8_t* a,const uint8_t* b){return memcmp(a,b,6)==0;}

int findSlaveByMac(const uint8_t* mac){
  for(int i=0;i<slaveCount;i++) if(macEqual(slaves[i].mac,mac)) return i;
  return -1;
}
int findSlaveByDroneId(const char* id){
  for(int i=0;i<slaveCount;i++) if(strcmp(slaves[i].droneId,id)==0) return i;
  return -1;
}

// Find which drone a user belongs to (from volunteer registry)
int findDroneByUid(const char* uid){
  for(int i=0;i<volCount;i++)
    if(strcmp(volunteers[i].uid,uid)==0)
      return findSlaveByDroneId(volunteers[i].drone);
  return -1;
}

void registerVol(const char* uid,const char* drone,const char* service){
  // Update if exists
  for(int i=0;i<volCount;i++){
    if(strcmp(volunteers[i].uid,uid)==0){
      strncpy(volunteers[i].service,service,7);
      strncpy(volunteers[i].drone,drone,7);
      return;
    }
  }
  if(volCount>=MAX_VOLS) return;
  strncpy(volunteers[volCount].uid,uid,23);
  strncpy(volunteers[volCount].drone,drone,7);
  strncpy(volunteers[volCount].service,service,7);
  volCount++;
}

// ── Serial → Laptop ──────────────────────────────────────────
void sendToLaptop(const char* type,const char* from,const char* to,
                  const char* userId,const char* payload){
  StaticJsonDocument<512> doc;
  doc["type"]=type; doc["from"]=from; doc["to"]=to;
  doc["userId"]=userId; doc["payload"]=payload; doc["ts"]=millis();
  String out; serializeJson(doc,out); Serial.println(out);
}

// ── Send MeshMessage to a specific slave ─────────────────────
void sendToSlave(const uint8_t* mac, MeshMessage& msg){
  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,mac,6);
  peer.channel=0; peer.encrypt=false;
  if(!esp_now_is_peer_exist(mac)) esp_now_add_peer(&peer);
  esp_now_send(mac,(uint8_t*)&msg,sizeof(msg));
}

void broadcastToAllSlaves(MeshMessage& msg){
  for(int i=0;i<slaveCount;i++)
    if(slaves[i].active) sendToSlave(slaves[i].mac,msg);
}

// Send message to a drone by its droneId
void sendToDroneById(const char* droneId, MeshMessage& msg){
  int idx=findSlaveByDroneId(droneId);
  if(idx!=-1 && slaves[idx].active) sendToSlave(slaves[idx].mac,msg);
}

// ── ESP-NOW receive ───────────────────────────────────────────
void onDataRecv(const esp_now_recv_info_t* info,const uint8_t* data,int len){
  if(len<(int)sizeof(MeshMessage)) return;
  MeshMessage msg;
  memcpy(&msg,data,sizeof(MeshMessage));

  const uint8_t* mac=info->src_addr;
  int idx=findSlaveByMac(mac);

  // Auto-register new slave
  if(idx==-1 && slaveCount<MAX_SLAVES){
    idx=slaveCount++;
    memcpy(slaves[idx].mac,mac,6);
    snprintf(slaves[idx].droneId,8,"D%d",idx+1);
    slaves[idx].active=true;
    slaves[idx].lastSeen=millis();
    sendToLaptop("DRONE_JOIN",slaves[idx].droneId,"MASTER","",macToStr(mac).c_str());
  }
  if(idx!=-1){ slaves[idx].lastSeen=millis(); slaves[idx].active=true; }

  const char* droneId=(idx!=-1)?slaves[idx].droneId:"UNKNOWN";

  // ── HEARTBEAT ─────────────────────────────────────────────
  if(strcmp(msg.type,"HEARTBEAT")==0){
    StaticJsonDocument<128> doc;
    doc["droneId"]=droneId; doc["mac"]=macToStr(mac);
    String p; serializeJson(doc,p);
    sendToLaptop("HEARTBEAT",droneId,"MASTER","",p.c_str());
    return;
  }

  // ── DRONE_BOOT ────────────────────────────────────────────
  if(strcmp(msg.type,"DRONE_BOOT")==0){
    sendToLaptop("DRONE_BOOT",droneId,"MASTER","",macToStr(mac).c_str());
    return;
  }

  // ── SERVICE_REG — store in global volunteer registry ──────
  if(strcmp(msg.type,"SERVICE_REG")==0){
    StaticJsonDocument<256> doc;
    if(!deserializeJson(doc,msg.payload)){
      const char* uid  = doc["uid"]  |"";
      const char* role = doc["role"] |"";
      const char* srv  = doc["srv"]  |"";
      if(strcmp(role,"offer")==0 && strlen(uid)>0)
        registerVol(uid, droneId, srv);
    }
    // Forward to laptop for logging
    sendToLaptop("SERVICE_REG",droneId,"MASTER",msg.userId,msg.payload);
    return;
  }

  // ── VOL_REQ — drone asks for volunteer list for a service ─
  if(strcmp(msg.type,"VOL_REQ")==0){
    const char* service=msg.payload;
    // Build JSON array of matching volunteers
    StaticJsonDocument<1024> arr;
    JsonArray a=arr.to<JsonArray>();
    for(int i=0;i<volCount;i++){
      if(strcmp(volunteers[i].service,service)==0){
        JsonObject o=a.createNestedObject();
        o["uid"]    =volunteers[i].uid;
        o["drone"]  =volunteers[i].drone;
        o["service"]=volunteers[i].service;
      }
    }
    String payload; serializeJson(arr,payload);

    // Reply to requesting drone with VOL_LIST
    MeshMessage reply={};
    strncpy(reply.type,"VOL_LIST",15);
    strncpy(reply.from,"MASTER",15);
    strncpy(reply.to,droneId,15);
    strncpy(reply.payload,payload.c_str(),179);
    reply.ts=millis();
    if(idx!=-1) sendToSlave(slaves[idx].mac,reply);
    return;
  }

  // ── CHAT — relay to recipient drone ───────────────────────
  if(strcmp(msg.type,"CHAT")==0){
    // msg.userId = sender uid, msg.toUserId = recipient uid
    // msg.payload has JSON with from/to/text/drone

    // Log to laptop
    sendToLaptop("CHAT",droneId,"MASTER",msg.userId,msg.payload);

    // Find which drone the recipient is on
    const char* recipientUid = msg.toUserId;
    if(strlen(recipientUid)>0){
      int recipIdx = findDroneByUid(recipientUid);
      if(recipIdx != -1 && recipIdx != idx){
        // Relay to recipient's drone
        MeshMessage relay={};
        strncpy(relay.type,   "CHAT",      15);
        strncpy(relay.from,   "MASTER",    15);
        strncpy(relay.to,     slaves[recipIdx].droneId, 15);
        strncpy(relay.userId,    msg.userId,   23);
        strncpy(relay.toUserId,  msg.toUserId, 23);
        strncpy(relay.payload,   msg.payload, 179);
        relay.ts=msg.ts;
        sendToSlave(slaves[recipIdx].mac, relay);
      }
      // Also relay back to sender's drone if different
      // (sender already stored it locally, skip)
    }
    return;
  }

  // ── Forward anything else to laptop ───────────────────────
  sendToLaptop(msg.type,droneId,msg.to,msg.userId,msg.payload);
}

// ── Send callback (v2/v3 compatible) ─────────────────────────
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSentV3(const wifi_tx_info_t* info,esp_now_send_status_t s){(void)info;(void)s;}
#else
void onDataSent(const uint8_t* mac,esp_now_send_status_t s){(void)mac;(void)s;}
#endif

// ── Parse command from laptop ─────────────────────────────────
void handleLaptopCommand(String& line){
  StaticJsonDocument<512> doc;
  if(deserializeJson(doc,line)) return;
  const char* type    = doc["type"]    |"CMD";
  const char* to      = doc["to"]      |"BROADCAST";
  const char* userId  = doc["userId"]  |"";
  const char* payload = doc["payload"] |"";

  MeshMessage msg={};
  strncpy(msg.type,  type,   15);
  strncpy(msg.from,  "MASTER",15);
  strncpy(msg.to,    to,     15);
  strncpy(msg.userId,userId, 23);
  strncpy(msg.payload,payload,179);
  msg.ts=millis();

  if(strcmp(to,"BROADCAST")==0) broadcastToAllSlaves(msg);
  else sendToDroneById(to,msg);
}

// ── Timeout check ─────────────────────────────────────────────
void checkTimeouts(){
  uint32_t now=millis();
  for(int i=0;i<slaveCount;i++){
    if(slaves[i].active && (now-slaves[i].lastSeen>SLAVE_TIMEOUT_MS)){
      slaves[i].active=false;
      sendToLaptop("DRONE_LOST",slaves[i].droneId,"MASTER","","timeout");
    }
  }
}

void sendHeartbeat(){
  MeshMessage msg={};
  strncpy(msg.type,"HEARTBEAT",15);
  strncpy(msg.from,"MASTER",15);
  strncpy(msg.to,"BROADCAST",15);
  msg.ts=millis();
  broadcastToAllSlaves(msg);
}

// ── Setup ─────────────────────────────────────────────────────
void setup(){
  Serial.begin(SERIAL_BAUD);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("{\"type\":\"MASTER_BOOT\",\"mac\":\"");
  Serial.print(WiFi.macAddress());
  Serial.println("\"}");

  if(esp_now_init()!=ESP_OK){
    Serial.println("{\"type\":\"ERROR\",\"payload\":\"ESP-NOW init failed\"}");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_send_cb(onDataSentV3);
#else
  esp_now_register_send_cb(onDataSent);
#endif
  Serial.println("{\"type\":\"READY\",\"payload\":\"Master online\"}");
}

// ── Loop ──────────────────────────────────────────────────────
void loop(){
  if(Serial.available()){
    String line=Serial.readStringUntil('\n');
    line.trim();
    if(line.length()>2) handleLaptopCommand(line);
  }
  uint32_t now=millis();
  if(now-lastHeartbeat>HEARTBEAT_MS){
    lastHeartbeat=now;
    sendHeartbeat();
    checkTimeouts();
  }
}
