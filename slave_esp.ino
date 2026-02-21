/*
 * ============================================================
 *  SLAVE ESP32 — Drone Node  (INBOX FIX)
 *
 *  HOW MESSAGING WORKS NOW:
 *  ─────────────────────────────────────────────────────────
 *  Needer opens chat with Volunteer → sends msg
 *    → stored on Needer's drone
 *    → sent to Master via ESP-NOW
 *    → Master relays to Volunteer's drone
 *    → Volunteer's drone stores it
 *    → Volunteer's phone polls /inbox every 2s
 *    → Inbox shows "New message from XYZ" badge
 *    → Volunteer taps → opens chat → sees message → replies
 *  ─────────────────────────────────────────────────────────
 *  SET BEFORE FLASHING:
 *    DRONE_NUMBER  →  1 or 2
 *    MASTER_MAC    →  Master ESP MAC
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <ArduinoJson.h>

// ── CONFIGURE BEFORE FLASHING ────────────────────────────────
#define DRONE_NUMBER   2
#define MASTER_MAC     {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}  // ← replace

#define TOSTRING2(x) #x
#define TOSTRING(x)  TOSTRING2(x)
#define DRONE_ID     "D" TOSTRING(DRONE_NUMBER)
#define AP_SSID      "ReliefNet-Drone-" TOSTRING(DRONE_NUMBER)
#define AP_CHANNEL   6
#define HEARTBEAT_MS   2500
#define MASTER_TIMEOUT 9000

// ── Structures (must match master exactly) ───────────────────
struct MeshMessage {
  char     type[16];
  char     from[16];
  char     to[16];
  char     userId[24];
  char     toUserId[24];
  char     payload[180];
  uint32_t ts;
};

struct ClientUser {
  char     uid[24];
  bool     active;
  uint32_t connTime;
  char     offering[8];
  char     requesting[8];
};

// A single chat message stored on this drone
struct ChatMsg {
  char     fromUid[24];
  char     toUid[24];
  char     text[180];
  uint32_t ts;
};

struct VolEntry {
  char uid[24];
  char drone[8];
  char service[8];
};

// ── Globals ──────────────────────────────────────────────────
uint8_t  masterMac[6]   = MASTER_MAC;
bool     masterOnline   = false;
uint32_t lastMasterSeen = 0;
uint32_t lastHeartbeat  = 0;

WebServer server(80);
DNSServer dnsServer;

#define MAX_CLIENTS  20
#define MAX_CHATS    200
#define MAX_VOLS     40

ClientUser clients[MAX_CLIENTS];   int clientCount = 0;
ChatMsg    chatLog[MAX_CHATS];     int chatCount   = 0;
VolEntry   volCache[MAX_VOLS];     int volCount    = 0;

// ── Helpers ──────────────────────────────────────────────────
int findClient(const char* uid){
  for(int i=0;i<clientCount;i++)
    if(strncmp(clients[i].uid,uid,23)==0) return i;
  return -1;
}
int getOrCreate(const char* uid){
  int idx=findClient(uid);
  if(idx!=-1) return idx;
  if(clientCount>=MAX_CLIENTS) return -1;
  idx=clientCount++;
  strncpy(clients[idx].uid,uid,23);
  clients[idx].active=true;
  clients[idx].connTime=millis();
  clients[idx].offering[0]='\0';
  clients[idx].requesting[0]='\0';
  return idx;
}

// Store a chat msg, skip exact duplicates
void storeChatLocal(const char* fromUid,const char* toUid,const char* text,uint32_t ts){
  // Check last 10 for duplicate
  int start=chatCount>10?chatCount-10:0;
  for(int i=start;i<chatCount;i++)
    if(strncmp(chatLog[i].fromUid,fromUid,23)==0 &&
       strncmp(chatLog[i].toUid,toUid,23)==0 &&
       strncmp(chatLog[i].text,text,179)==0) return;
  if(chatCount>=MAX_CHATS){
    memmove(&chatLog[0],&chatLog[1],sizeof(ChatMsg)*(MAX_CHATS-1));
    chatCount=MAX_CHATS-1;
  }
  ChatMsg& cm=chatLog[chatCount++];
  strncpy(cm.fromUid,fromUid,23);
  strncpy(cm.toUid,toUid,23);
  strncpy(cm.text,text,179);
  cm.ts=ts?ts:millis();
}

// Get unique list of people who sent messages TO a given uid
// Returns JSON array of {uid, lastMsg, ts, unread}
String getInboxFor(const char* myUid){
  // collect unique senders
  struct InboxItem { char uid[24]; char lastMsg[60]; uint32_t ts; int unread; };
  InboxItem items[20]; int itemCount=0;

  for(int i=0;i<chatCount;i++){
    // messages where I am the recipient
    if(strncmp(chatLog[i].toUid,myUid,23)!=0) continue;
    const char* sender=chatLog[i].fromUid;
    // find or create inbox item
    int found=-1;
    for(int j=0;j<itemCount;j++)
      if(strncmp(items[j].uid,sender,23)==0){found=j;break;}
    if(found==-1){
      if(itemCount>=20) continue;
      found=itemCount++;
      strncpy(items[found].uid,sender,23);
      items[found].lastMsg[0]='\0';
      items[found].ts=0;
      items[found].unread=0;
    }
    items[found].unread++;
    if(chatLog[i].ts>=items[found].ts){
      items[found].ts=chatLog[i].ts;
      strncpy(items[found].lastMsg,chatLog[i].text,59);
    }
  }

  StaticJsonDocument<1024> arr;
  JsonArray a=arr.to<JsonArray>();
  for(int i=0;i<itemCount;i++){
    JsonObject o=a.createNestedObject();
    o["uid"]=items[i].uid;
    o["lastMsg"]=items[i].lastMsg;
    o["ts"]=items[i].ts;
    o["unread"]=items[i].unread;
  }
  String out; serializeJson(arr,out);
  return out;
}

// ── ESP-NOW: send to master ───────────────────────────────────
void sendToMaster(const char* type,const char* userId,
                  const char* toUserId,const char* payload){
  MeshMessage msg={};
  strncpy(msg.type,    type,     15);
  strncpy(msg.from,    DRONE_ID, 15);
  strncpy(msg.to,      "MASTER", 15);
  strncpy(msg.userId,  userId,   23);
  strncpy(msg.toUserId,toUserId, 23);
  strncpy(msg.payload, payload, 179);
  msg.ts=millis();
  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,masterMac,6);
  peer.channel=0;peer.encrypt=false;
  if(!esp_now_is_peer_exist(masterMac)) esp_now_add_peer(&peer);
  esp_now_send(masterMac,(uint8_t*)&msg,sizeof(msg));
}

// ── ESP-NOW receive ───────────────────────────────────────────
void onDataRecv(const esp_now_recv_info_t* info,const uint8_t* data,int len){
  if(len<(int)sizeof(MeshMessage)) return;
  MeshMessage msg; memcpy(&msg,data,sizeof(MeshMessage));

  if(strcmp(msg.type,"HEARTBEAT")==0 && strcmp(msg.from,"MASTER")==0){
    masterOnline=true; lastMasterSeen=millis(); return;
  }

  bool forMe=(strcmp(msg.to,DRONE_ID)==0||strcmp(msg.to,"BROADCAST")==0);
  if(!forMe) return;

  // ── Incoming CHAT relayed from master ─────────────────────
  // msg.userId = sender, msg.toUserId = recipient on THIS drone
  if(strcmp(msg.type,"CHAT")==0){
    storeChatLocal(msg.userId, msg.toUserId, msg.payload, msg.ts);
    Serial.printf("[INBOX] Msg from %s to %s: %s\n",msg.userId,msg.toUserId,msg.payload);
    return;
  }

  // ── Volunteer list from master ────────────────────────────
  if(strcmp(msg.type,"VOL_LIST")==0){
    StaticJsonDocument<1024> doc;
    if(!deserializeJson(doc,msg.payload)){
      JsonArray arr=doc.as<JsonArray>();
      volCount=0;
      for(JsonObject o:arr){
        if(volCount>=MAX_VOLS) break;
        strncpy(volCache[volCount].uid,    o["uid"]    |"",23);
        strncpy(volCache[volCount].drone,  o["drone"]  |"",7);
        strncpy(volCache[volCount].service,o["service"]|"",7);
        volCount++;
      }
    }
    return;
  }

  if(strcmp(msg.type,"CMD")==0){
    Serial.printf("[CMD] %s\n",msg.payload); return;
  }
}

// ─────────────────────────────────────────────────────────────
//  HTML
// ─────────────────────────────────────────────────────────────
const char CLIENT_HTML[] PROGMEM = R"HTMLEOF(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ReliefNet</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#07090f;--card:#0f1623;--card2:#161e2e;
  --accent:#00d4ff;--a2:#7c3aed;--green:#10b981;
  --red:#ef4444;--yellow:#f59e0b;--orange:#f97316;
  --text:#e2e8f0;--muted:#64748b;--border:#1a2540
}
body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh}
/* Header */
.hdr{padding:13px 16px;background:linear-gradient(135deg,#0d1117,#1a1040);
     border-bottom:1px solid var(--border);display:flex;align-items:center;gap:10px;
     position:sticky;top:0;z-index:20}
.logo{width:36px;height:36px;border-radius:11px;flex-shrink:0;
      background:linear-gradient(135deg,var(--accent),var(--a2));
      display:flex;align-items:center;justify-content:center;font-size:19px}
.hdr-t{font-size:16px;font-weight:800}
.hdr-s{font-size:11px;color:var(--muted)}
.dpill{margin-left:auto;background:#7c3aed22;color:#a78bfa;border:1px solid #7c3aed44;
       padding:4px 11px;border-radius:20px;font-size:11px;font-weight:700}
/* Info bar */
.ibar{background:var(--card);border-bottom:1px solid var(--border);
      padding:7px 16px;display:flex;align-items:center;gap:7px;flex-wrap:wrap}
.uid-l{font-size:11px;color:var(--muted)}
.uid-v{font-size:11px;color:var(--accent);font-family:monospace;font-weight:600}
.cdot{width:7px;height:7px;border-radius:50%;margin-left:auto;flex-shrink:0}
.don{background:var(--green);box-shadow:0 0 5px var(--green)}
.doff{background:var(--red)}
.clbl{font-size:11px;color:var(--muted)}
/* Tags */
.tbar{padding:7px 16px;background:var(--bg);border-bottom:1px solid var(--border);
      min-height:34px;display:flex;align-items:center;gap:6px;flex-wrap:wrap}
.tag{display:inline-flex;align-items:center;gap:4px;padding:3px 10px;
     border-radius:20px;font-size:11px;font-weight:600}
.to{background:#10b98115;color:var(--green);border:1px solid #10b98130}
.tn{background:#f59e0b15;color:var(--yellow);border:1px solid #f59e0b30}
/* Nav tabs */
.tabs{display:flex;background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:62px;z-index:15}
.tab{flex:1;padding:11px 6px;text-align:center;font-size:12px;font-weight:600;
     color:var(--muted);cursor:pointer;border-bottom:2px solid transparent;
     transition:.15s;position:relative;-webkit-tap-highlight-color:transparent}
.tab.active{color:var(--accent);border-bottom-color:var(--accent)}
.badge{position:absolute;top:6px;right:calc(50% - 20px);background:var(--red);
       color:#fff;border-radius:20px;font-size:10px;font-weight:700;
       padding:1px 6px;min-width:16px;text-align:center}
/* Pages */
.page{display:none;padding:18px 16px;max-width:460px;margin:0 auto}
.page.active{display:block}
.pg-t{text-align:center;font-size:20px;font-weight:800;margin:20px 0 6px}
.pg-s{text-align:center;color:var(--muted);font-size:13px;margin-bottom:26px}
/* Role cards */
.rg{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.rc{background:var(--card);border:2px solid var(--border);border-radius:16px;
    padding:26px 14px;text-align:center;cursor:pointer;transition:.2s;
    -webkit-tap-highlight-color:transparent}
.rc:active{transform:scale(.96)}.rc:hover{border-color:var(--accent);background:var(--card2)}
.rc .ico{font-size:42px;margin-bottom:10px;display:block}
.rc h3{font-size:14px;font-weight:700;margin-bottom:5px}
.rc p{font-size:12px;color:var(--muted);line-height:1.5}
/* Service grid */
.sg{display:grid;grid-template-columns:1fr 1fr;gap:11px;margin-bottom:18px}
.sc{background:var(--card);border:2px solid var(--border);border-radius:14px;
    padding:20px 12px;text-align:center;cursor:pointer;transition:.2s;
    -webkit-tap-highlight-color:transparent}
.sc:active{transform:scale(.96)}.sc.sel{border-color:var(--accent);background:var(--card2)}
.sc .si{font-size:34px;margin-bottom:7px;display:block}
.sc span{font-size:13px;font-weight:600}
/* Buttons */
.btn{display:block;width:100%;padding:13px;border:none;border-radius:13px;
     font-size:14px;font-weight:700;cursor:pointer;transition:.15s;margin-bottom:8px;
     -webkit-tap-highlight-color:transparent}
.btn:active{transform:scale(.97)}
.bpri{background:linear-gradient(135deg,var(--accent),var(--a2));color:#fff}
.bsec{background:var(--card);border:1px solid var(--border);color:var(--muted)}
/* Vol list */
.st{font-size:15px;font-weight:700;margin-bottom:3px}
.ss{font-size:12px;color:var(--muted);margin-bottom:16px}
.vc{background:var(--card);border:1px solid var(--border);border-radius:13px;
    padding:13px 14px;margin-bottom:9px;display:flex;align-items:center;gap:11px}
.va{width:40px;height:40px;border-radius:11px;flex-shrink:0;
    background:linear-gradient(135deg,var(--a2),var(--accent));
    display:flex;align-items:center;justify-content:center;font-size:17px}
.vn{font-size:14px;font-weight:700}
.vid{font-size:11px;color:var(--muted);font-family:monospace;margin-top:1px}
.vdr{font-size:11px;color:var(--accent);margin-top:1px}
.bc{margin-left:auto;padding:7px 14px;background:var(--a2);color:#fff;
    border:none;border-radius:9px;font-size:12px;font-weight:700;cursor:pointer;
    flex-shrink:0;-webkit-tap-highlight-color:transparent}
.empty{text-align:center;padding:44px 20px;color:var(--muted)}
.ei{font-size:48px;margin-bottom:12px}.empty p{font-size:13px;line-height:1.6}
.spin{text-align:center;padding:36px;font-size:26px;
      animation:sp 1s linear infinite;display:block}
@keyframes sp{to{transform:rotate(360deg)}}
/* INBOX */
.inbox-item{background:var(--card);border:1px solid var(--border);border-radius:13px;
            padding:13px 14px;margin-bottom:9px;display:flex;align-items:center;gap:11px;
            cursor:pointer;transition:.15s;-webkit-tap-highlight-color:transparent}
.inbox-item:active{transform:scale(.98)}
.inbox-item.unread{border-color:#00d4ff44;background:var(--card2)}
.ia{width:42px;height:42px;border-radius:11px;flex-shrink:0;
    background:linear-gradient(135deg,#1a2236,#2d1b69);
    display:flex;align-items:center;justify-content:center;font-size:18px;position:relative}
.unread-dot{position:absolute;top:2px;right:2px;width:10px;height:10px;
            background:var(--red);border-radius:50%;border:2px solid var(--card)}
.iname{font-size:13px;font-weight:700}
.ipreview{font-size:12px;color:var(--muted);margin-top:2px;
          white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:180px}
.its{font-size:11px;color:var(--muted);margin-left:auto;flex-shrink:0;align-self:flex-start}
.inbox-empty{text-align:center;padding:52px 20px}
.inbox-empty .ie{font-size:52px;margin-bottom:14px}
.inbox-empty h3{font-size:16px;font-weight:700;margin-bottom:8px}
.inbox-empty p{font-size:13px;color:var(--muted);line-height:1.6}
/* Chat */
.ctop{display:flex;align-items:center;gap:10px;padding:4px 0 14px}
.bk{width:35px;height:35px;background:var(--card);border:1px solid var(--border);
    border-radius:10px;display:flex;align-items:center;justify-content:center;
    cursor:pointer;font-size:17px;flex-shrink:0;-webkit-tap-highlight-color:transparent}
.cn{font-size:15px;font-weight:700}.cu{font-size:11px;color:var(--muted);font-family:monospace}
.mw{height:55vh;overflow-y:auto;display:flex;flex-direction:column;gap:7px;padding:4px 0 6px}
.bb{max-width:80%;padding:10px 13px;border-radius:15px;font-size:14px;line-height:1.5;word-break:break-word}
.out{background:linear-gradient(135deg,var(--a2),#4338ca);align-self:flex-end;border-bottom-right-radius:3px}
.in{background:var(--card2);align-self:flex-start;border:1px solid var(--border);border-bottom-left-radius:3px}
.bts{font-size:10px;color:rgba(255,255,255,.3);margin-top:3px;text-align:right}
.cir{display:flex;gap:8px;padding-top:8px}
.ci{flex:1;background:var(--card);border:1px solid var(--border);border-radius:13px;
    padding:11px 13px;color:var(--text);font-size:14px;outline:none}
.ci:focus{border-color:var(--accent)}
.sb{width:46px;height:46px;flex-shrink:0;
    background:linear-gradient(135deg,var(--accent),var(--a2));
    border:none;border-radius:13px;font-size:20px;cursor:pointer;
    display:flex;align-items:center;justify-content:center}
</style>
</head>
<body>

<div class="hdr">
  <div class="logo">🛸</div>
  <div><div class="hdr-t">ReliefNet</div><div class="hdr-s">Offline Aid Network</div></div>
  <div class="dpill">DRONE %DRONEID%</div>
</div>

<div class="ibar">
  <span class="uid-l">Your ID:</span>
  <span class="uid-v" id="uidDisplay">-</span>
  <span class="cdot doff" id="cDot"></span>
  <span class="clbl" id="cLbl">Connecting...</span>
</div>
<div class="tbar" id="tBar"><span style="font-size:12px;color:var(--muted)">No services yet</span></div>

<!-- TABS — only shown after at least one service registered -->
<div class="tabs" id="tabBar" style="display:none">
  <div class="tab active" id="tab-services" onclick="switchTab('services')">Services</div>
  <div class="tab" id="tab-inbox" onclick="switchTab('inbox')">
    Inbox<span class="badge" id="inboxBadge" style="display:none">0</span>
  </div>
</div>

<!-- ── PAGE: Role ── -->
<div class="page active" id="pgRole">
  <div class="pg-t">How can we help?</div>
  <div class="pg-s">Choose your role — you can do both</div>
  <div class="rg">
    <div class="rc" onclick="goService('offer')">
      <span class="ico">🤝</span><h3>Ready to Help</h3><p>Volunteer your resources</p>
    </div>
    <div class="rc" onclick="goService('need')">
      <span class="ico">🆘</span><h3>Need Help</h3><p>Find volunteers near you</p>
    </div>
  </div>
</div>

<!-- ── PAGE: Service Select ── -->
<div class="page" id="pgSrv">
  <div class="pg-t" id="srvT">Choose Service</div>
  <div class="pg-s" id="srvS">Select the type</div>
  <div class="sg">
    <div class="sc" id="sc-FOOD"  onclick="selSrv('FOOD',this)"><span class="si">🍱</span><span>Food</span></div>
    <div class="sc" id="sc-WATER" onclick="selSrv('WATER',this)"><span class="si">💧</span><span>Water</span></div>
    <div class="sc" id="sc-ACCOM" onclick="selSrv('ACCOM',this)"><span class="si">🏕️</span><span>Shelter</span></div>
    <div class="sc" id="sc-MED"   onclick="selSrv('MED',this)"><span class="si">💊</span><span>Medicine</span></div>
  </div>
  <button class="btn bpri" onclick="submitSrv()">Confirm</button>
  <button class="btn bsec" onclick="goPage('pgRole')">Back</button>
</div>

<!-- ── PAGE: Volunteer List ── -->
<div class="page" id="pgVols">
  <div class="st" id="vlT">Volunteers</div>
  <div class="ss" id="vlS">Tap Chat to connect</div>
  <div id="vlList"></div>
  <button class="btn bsec" style="margin-top:14px" onclick="goPage('pgRole')">+ Add Another Service</button>
</div>

<!-- ── PAGE: INBOX (volunteer sees incoming messages here) ── -->
<div class="page" id="pgInbox">
  <div class="st" style="margin-bottom:4px">Inbox</div>
  <div class="ss">People who messaged you</div>
  <div id="inboxList">
    <div class="inbox-empty">
      <div class="ie">📭</div>
      <h3>No messages yet</h3>
      <p>When someone requests help from you,<br>their message will appear here.</p>
    </div>
  </div>
</div>

<!-- ── PAGE: Chat ── -->
<div class="page" id="pgChat">
  <div class="ctop">
    <div class="bk" id="chatBack" onclick="chatGoBack()">←</div>
    <div><div class="cn" id="chatN">Chat</div><div class="cu" id="chatU"></div></div>
  </div>
  <div class="mw" id="mWrap"></div>
  <div class="cir">
    <input class="ci" id="mInp" placeholder="Type a message..." onkeydown="if(event.key==='Enter')sendMsg()">
    <button class="sb" onclick="sendMsg()">&#10148;</button>
  </div>
</div>

<script>
const DRONE='%DRONEID%';
const UID_KEY='rn_uid_'+DRONE;
let uid=localStorage.getItem(UID_KEY);
if(!uid){uid=DRONE+'-'+Math.random().toString(36).slice(2,8).toUpperCase();localStorage.setItem(UID_KEY,uid);}
document.getElementById('uidDisplay').textContent=uid;

let curRole='', curSrv='', chatPartner='', chatFromPage='pgVols';
let pollTimer=null, inboxTimer=null;
let hasService=false;

// ── Navigation ────────────────────────────────────────────────
function goPage(id){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  stopPoll();
  if(id==='pgChat') startPoll();
  if(id==='pgInbox') renderInbox();
}

function switchTab(tab){
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.getElementById('tab-'+tab).classList.add('active');
  if(tab==='services') goPage('pgRole');
  else if(tab==='inbox'){ goPage('pgInbox'); }
}

function showTabs(){
  if(!hasService){
    hasService=true;
    document.getElementById('tabBar').style.display='flex';
  }
}

function chatGoBack(){
  goPage(chatFromPage);
  if(chatFromPage==='pgInbox') renderInbox();
}

// ── Service flow ──────────────────────────────────────────────
function goService(role){
  curRole=role; curSrv='';
  document.querySelectorAll('.sc').forEach(c=>c.classList.remove('sel'));
  document.getElementById('srvT').textContent=role==='offer'?'What can you offer?':'What do you need?';
  document.getElementById('srvS').textContent=role==='offer'?'Select your volunteering category':'Select the help you need';
  goPage('pgSrv');
}
function selSrv(srv,el){
  document.querySelectorAll('.sc').forEach(c=>c.classList.remove('sel'));
  el.classList.add('sel'); curSrv=srv;
}
function submitSrv(){
  if(!curSrv){alert('Please select a service');return;}
  fetch('/register',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({uid,role:curRole,service:curSrv})})
  .then(r=>r.json()).then(()=>{
    showTabs(); refreshTags();
    if(curRole==='need') loadVols(curSrv);
    else goPage('pgRole');
  });
}

function refreshTags(){
  fetch('/myservices?uid='+encodeURIComponent(uid)).then(r=>r.json()).then(d=>{
    const b=document.getElementById('tBar');
    let h='';
    if(d.offering)   h+=`<span class="tag to">✋ ${srvLabel(d.offering)}</span>`;
    if(d.requesting) h+=`<span class="tag tn">🆘 ${srvLabel(d.requesting)}</span>`;
    b.innerHTML=h||'<span style="font-size:12px;color:var(--muted)">No services yet</span>';
  });
}

// ── Volunteer list ────────────────────────────────────────────
function loadVols(service){
  const list=document.getElementById('vlList');
  list.innerHTML='<span class="spin">&#8635;</span>';
  document.getElementById('vlT').textContent='Volunteers for '+srvLabel(service);
  document.getElementById('vlS').textContent='Fetching from network...';
  goPage('pgVols');
  fetch('/reqvols?service='+service).then(()=>{
    setTimeout(()=>{
      fetch('/volunteers?service='+encodeURIComponent(service)+'&uid='+encodeURIComponent(uid))
      .then(r=>r.json()).then(vols=>{
        document.getElementById('vlS').textContent=vols.length?'Tap Chat to connect':'No volunteers yet';
        if(!vols.length){
          list.innerHTML=`<div class="empty"><div class="ei">🔍</div>
            <p>No volunteers for ${srvLabel(service)} yet.<br>Check back soon.</p></div>
            <button class="btn bsec" style="margin-top:8px" onclick="loadVols('${service}')">↻ Refresh</button>`;
        } else {
          list.innerHTML=vols.map(v=>`
            <div class="vc">
              <div class="va">${srvIcon(service)}</div>
              <div style="flex:1;min-width:0">
                <div class="vn">ID: ${v.uid.slice(-8)}</div>
                <div class="vid">${v.uid}</div>
                <div class="vdr">via ${v.drone||'?'}</div>
              </div>
              <button class="bc" onclick="openChat('${v.uid}','pgVols')">Chat ›</button>
            </div>`).join('');
        }
      });
    },1600);
  });
}

// ── INBOX ─────────────────────────────────────────────────────
let lastInboxCount=0;

function renderInbox(){
  fetch('/inbox?uid='+encodeURIComponent(uid)).then(r=>r.json()).then(items=>{
    const el=document.getElementById('inboxList');
    if(!items.length){
      el.innerHTML=`<div class="inbox-empty">
        <div class="ie">📭</div><h3>No messages yet</h3>
        <p>When someone requests help from you,<br>their message will appear here.</p>
      </div>`;
      return;
    }
    // Sort newest first
    items.sort((a,b)=>b.ts-a.ts);
    el.innerHTML=items.map(item=>{
      const t=item.ts?new Date(item.ts).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'}):'';
      const hasUnread=item.unread>0;
      return `<div class="inbox-item ${hasUnread?'unread':''}" onclick="openChat('${item.uid}','pgInbox')">
        <div class="ia">💬${hasUnread?'<div class="unread-dot"></div>':''}</div>
        <div style="flex:1;min-width:0">
          <div class="iname">ID: ${item.uid.slice(-8)}</div>
          <div class="ipreview">${esc(item.lastMsg)}</div>
        </div>
        <div class="its">${t}</div>
      </div>`;
    }).join('');
  });
}

function pollInbox(){
  fetch('/inbox?uid='+encodeURIComponent(uid)).then(r=>r.json()).then(items=>{
    const total=items.reduce((s,i)=>s+i.unread,0);
    const badge=document.getElementById('inboxBadge');
    if(total>0){badge.style.display='inline';badge.textContent=total>99?'99+':total;}
    else badge.style.display='none';
    // Refresh inbox page if open
    const inboxPage=document.getElementById('pgInbox');
    if(inboxPage.classList.contains('active')) renderInbox();
  }).catch(()=>{});
}

// ── Chat ──────────────────────────────────────────────────────
function openChat(partnerUid, fromPage){
  chatPartner=partnerUid;
  chatFromPage=fromPage||'pgVols';
  document.getElementById('chatN').textContent='ID: '+partnerUid.slice(-8);
  document.getElementById('chatU').textContent=partnerUid;
  document.getElementById('mWrap').innerHTML='';
  goPage('pgChat');
}
function startPoll(){stopPoll();fetchMsgs();pollTimer=setInterval(fetchMsgs,1500);}
function stopPoll(){if(pollTimer){clearInterval(pollTimer);pollTimer=null;}}
function fetchMsgs(){
  if(!chatPartner) return;
  fetch('/messages?uid='+encodeURIComponent(uid)+'&with='+encodeURIComponent(chatPartner))
  .then(r=>r.json()).then(msgs=>{
    const w=document.getElementById('mWrap');
    const atBot=w.scrollTop+w.clientHeight>=w.scrollHeight-20;
    w.innerHTML=msgs.map(m=>{
      const out=m.from===uid;
      const t=new Date(m.ts).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
      return`<div class="bb ${out?'out':'in'}">${esc(m.text)}<div class="bts">${t}</div></div>`;
    }).join('')||'<div style="text-align:center;padding:36px;color:var(--muted);font-size:13px">No messages yet — say hi! 👋</div>';
    if(atBot) w.scrollTop=w.scrollHeight;
  });
}
function sendMsg(){
  const inp=document.getElementById('mInp');
  const text=inp.value.trim(); if(!text) return;
  inp.value='';
  fetch('/send',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({from:uid,to:chatPartner,text})}).then(()=>fetchMsgs());
}

// ── Connection check ──────────────────────────────────────────
function chkConn(){
  fetch('/ping',{cache:'no-store'}).then(()=>{
    document.getElementById('cDot').className='cdot don';
    document.getElementById('cLbl').textContent='Connected';
  }).catch(()=>{
    document.getElementById('cDot').className='cdot doff';
    document.getElementById('cLbl').textContent='Offline';
  });
}

// ── Utils ─────────────────────────────────────────────────────
const srvLabel=s=>({FOOD:'Food',WATER:'Water',ACCOM:'Shelter',MED:'Medicine'}[s]||s);
const srvIcon=s=>({FOOD:'🍱',WATER:'💧',ACCOM:'🏕️',MED:'💊'}[s]||'?');
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}

// ── Init ──────────────────────────────────────────────────────
chkConn(); setInterval(chkConn,4000);
refreshTags();
// Check if user already has services registered, show tabs
fetch('/myservices?uid='+encodeURIComponent(uid)).then(r=>r.json()).then(d=>{
  if(d.offering||d.requesting) showTabs();
});
// Poll inbox every 3 seconds for new message badge
inboxTimer=setInterval(pollInbox,3000);
</script>
</body>
</html>
)HTMLEOF";

// ─────────────────────────────────────────────────────────────
//  HTTP ROUTES
// ─────────────────────────────────────────────────────────────
void setupRoutes(){
  auto servePortal=[](){
    String html=String(CLIENT_HTML);
    html.replace("%DRONEID%",DRONE_ID);
    server.send(200,"text/html; charset=utf-8",html);
  };
  server.on("/",HTTP_GET,servePortal);
  server.on("/generate_204",HTTP_GET,servePortal);
  server.on("/gen_204",HTTP_GET,servePortal);
  server.on("/hotspot-detect.html",HTTP_GET,servePortal);
  server.on("/library/test/success.html",HTTP_GET,servePortal);
  server.on("/ncsi.txt",HTTP_GET,servePortal);
  server.on("/connecttest.txt",HTTP_GET,servePortal);
  server.on("/redirect",HTTP_GET,servePortal);
  server.on("/canonical.html",HTTP_GET,servePortal);
  server.on("/ping",HTTP_GET,[](){server.send(200,"text/plain","ok");});

  // Register service
  server.on("/register",HTTP_POST,[](){
    StaticJsonDocument<256> doc;
    deserializeJson(doc,server.arg("plain"));
    const char* ruid=doc["uid"]    |"";
    const char* role=doc["role"]   |"";
    const char* srv =doc["service"]|"";
    int idx=getOrCreate(ruid);
    if(idx!=-1){
      if(strcmp(role,"offer")==0) strncpy(clients[idx].offering,srv,7);
      else strncpy(clients[idx].requesting,srv,7);
    }
    StaticJsonDocument<256> p;
    p["uid"]=ruid;p["role"]=role;p["srv"]=srv;p["drone"]=DRONE_ID;
    String ps;serializeJson(p,ps);
    sendToMaster("SERVICE_REG",ruid,"",ps.c_str());
    server.send(200,"application/json","{\"ok\":true}");
  });

  // My services
  server.on("/myservices",HTTP_GET,[](){
    String ruid=server.arg("uid");
    int idx=findClient(ruid.c_str());
    StaticJsonDocument<128> doc;
    if(idx!=-1){doc["offering"]=clients[idx].offering;doc["requesting"]=clients[idx].requesting;}
    String out;serializeJson(doc,out);
    server.send(200,"application/json",out);
  });

  // Request vol list from master
  server.on("/reqvols",HTTP_GET,[](){
    String srv=server.arg("service");
    sendToMaster("VOL_REQ","","",srv.c_str());
    server.send(200,"application/json","{\"ok\":true}");
  });

  // Get volunteers
  server.on("/volunteers",HTTP_GET,[](){
    String srv=server.arg("service");
    String myuid=server.arg("uid");
    StaticJsonDocument<2048> arr;
    JsonArray a=arr.to<JsonArray>();
    for(int i=0;i<volCount;i++){
      if(strcmp(volCache[i].service,srv.c_str())==0&&strcmp(volCache[i].uid,myuid.c_str())!=0){
        JsonObject o=a.createNestedObject();o["uid"]=volCache[i].uid;o["drone"]=volCache[i].drone;
      }
    }
    for(int i=0;i<clientCount;i++){
      if(strcmp(clients[i].offering,srv.c_str())==0&&strcmp(clients[i].uid,myuid.c_str())!=0){
        bool found=false;
        for(JsonObject o2:a) if(strcmp(o2["uid"]|"",clients[i].uid)==0){found=true;break;}
        if(!found){JsonObject o=a.createNestedObject();o["uid"]=clients[i].uid;o["drone"]=DRONE_ID;}
      }
    }
    String out;serializeJson(arr,out);
    server.send(200,"application/json",out);
  });

  // ── INBOX — returns list of people who messaged THIS user ──
  server.on("/inbox",HTTP_GET,[](){
    String myuid=server.arg("uid");
    String result=getInboxFor(myuid.c_str());
    server.send(200,"application/json",result);
  });

  // Get messages between two users
  server.on("/messages",HTTP_GET,[](){
    String ruid=server.arg("uid");
    String with=server.arg("with");
    StaticJsonDocument<3072> arr;
    JsonArray a=arr.to<JsonArray>();
    for(int i=0;i<chatCount;i++){
      bool match=
        (strncmp(chatLog[i].fromUid,ruid.c_str(),23)==0&&strncmp(chatLog[i].toUid,with.c_str(),23)==0)||
        (strncmp(chatLog[i].fromUid,with.c_str(),23)==0&&strncmp(chatLog[i].toUid,ruid.c_str(),23)==0);
      if(match){
        JsonObject o=a.createNestedObject();
        o["from"]=chatLog[i].fromUid;o["to"]=chatLog[i].toUid;
        o["text"]=chatLog[i].text;o["ts"]=chatLog[i].ts;
      }
    }
    String out;serializeJson(arr,out);
    server.send(200,"application/json",out);
  });

  // Send message
  server.on("/send",HTTP_POST,[](){
    StaticJsonDocument<512> doc;
    deserializeJson(doc,server.arg("plain"));
    const char* from=doc["from"]|"";
    const char* to  =doc["to"]  |"";
    const char* text=doc["text"]|"";
    // Store locally (sender copy)
    storeChatLocal(from,to,text,millis());
    // Forward to master → master relays to recipient drone
    StaticJsonDocument<256> p;
    p["from"]=from;p["to"]=to;p["text"]=text;p["drone"]=DRONE_ID;
    String ps;serializeJson(p,ps);
    sendToMaster("CHAT",from,to,ps.c_str());
    server.send(200,"application/json","{\"ok\":true}");
  });

  server.onNotFound([](){
    server.sendHeader("Location","http://192.168.4.1/");
    server.send(302,"text/plain","");
  });
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200); delay(500);
  Serial.printf("\n[SLAVE] Drone %s booting\n",DRONE_ID);
  Serial.printf("[MAC] %s\n",WiFi.macAddress().c_str());

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);  // open, no password
  delay(200);
  IPAddress apIP(192,168,4,1);
  WiFi.softAPConfig(apIP,apIP,IPAddress(255,255,255,0));
  Serial.printf("[AP] %s  IP: %s  (open)\n",AP_SSID,WiFi.softAPIP().toString().c_str());

  dnsServer.start(53,"*",apIP);
  Serial.println("[DNS] Captive portal active");

  if(esp_now_init()!=ESP_OK){Serial.println("[ERROR] ESP-NOW failed");return;}
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,masterMac,6);
  peer.channel=0;peer.encrypt=false;
  esp_now_add_peer(&peer);

  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Ready");
  sendToMaster("DRONE_BOOT","","",DRONE_ID);
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop(){
  dnsServer.processNextRequest();
  server.handleClient();
  uint32_t now=millis();
  if(now-lastHeartbeat>HEARTBEAT_MS){
    lastHeartbeat=now;
    sendToMaster("HEARTBEAT","","",DRONE_ID);
    if(masterOnline&&(now-lastMasterSeen>MASTER_TIMEOUT)){
      masterOnline=false;
      Serial.println("[WARN] Master timeout");
    }
  }
}
