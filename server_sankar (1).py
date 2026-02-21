#!/usr/bin/env python3
"""
ReliefNet Master Server
-----------------------
Bridges the Master ESP (via USB Serial) to a WebSocket-based
control dashboard running in your browser on the laptop.

Requirements:
  pip install flask flask-socketio pyserial

Run:
  python server.py --port COM3   (Windows)
  python server.py --port /dev/ttyUSB0   (Linux/Mac)

New in this version:
  - GPS coordinates from mobile users stored and shown on pannable Leaflet map
  - Marquee news ticker broadcast from control centre to all connected browser clients
  - Immediate disconnect detection: users marked "Left" instantly on WebSocket drop
"""

import sys, json, time, threading, argparse, os, logging
from datetime import datetime
from collections import defaultdict

import serial
from flask import Flask, render_template_string, request, jsonify
from flask_socketio import SocketIO, emit

# ── Config ────────────────────────────────────────────────────
SERIAL_BAUD  = 115200
WEB_PORT     = 5000
LOG_FILE     = "reliefnet_log.jsonl"

# ── App setup ─────────────────────────────────────────────────
app     = Flask(__name__)
app.config['SECRET_KEY'] = 'reliefnet2024'
sio     = SocketIO(app, cors_allowed_origins="*", async_mode="threading")
logging.basicConfig(level=logging.WARNING)

# ── State ─────────────────────────────────────────────────────
state = {
    "drones":     {},   # droneId → {active, lastSeen, mac}
    "users":      {},   # uid → {uid, drone, offering, requesting, connTime, status, lat, lng}
    "chats":      [],   # [{from, to, text, drone, ts}]
    "events":     [],   # system events
    "serial_ok":  False,
    "news_ticker": "",  # current marquee message
}

ser: serial.Serial = None
serial_lock = threading.Lock()

# ── Logging ───────────────────────────────────────────────────
def log_event(obj):
    obj["_ts"] = datetime.now().isoformat()
    state["events"].insert(0, obj)
    if len(state["events"]) > 500:
        state["events"] = state["events"][:500]
    with open(LOG_FILE, "a") as f:
        f.write(json.dumps(obj) + "\n")

# ── Serial read thread ────────────────────────────────────────
def serial_reader(port):
    global ser
    while True:
        try:
            ser = serial.Serial(port, SERIAL_BAUD, timeout=1)
            state["serial_ok"] = True
            sio.emit("serial_status", {"ok": True})
            print(f"[Serial] Connected on {port}")
            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                if line:
                    handle_serial_line(line)
        except serial.SerialException as e:
            state["serial_ok"] = False
            sio.emit("serial_status", {"ok": False, "err": str(e)})
            print(f"[Serial] Error: {e} — retrying in 3s")
            time.sleep(3)
        except Exception as e:
            print(f"[Serial] Unexpected: {e}")
            time.sleep(3)

def handle_serial_line(line):
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        return

    t    = msg.get("type", "")
    frm  = msg.get("from", "")
    uid  = msg.get("userId", "")
    pay  = msg.get("payload", "")
    ts   = datetime.now().isoformat()

    # ── Drone online ──────────────────────────────────────────
    if t in ("DRONE_JOIN", "DRONE_BOOT", "HEARTBEAT"):
        drone_id = frm if frm else pay
        if drone_id and drone_id != "MASTER":
            d = state["drones"].setdefault(drone_id, {"active": False, "mac": "", "lastSeen": ""})
            d["active"]   = True
            d["lastSeen"] = ts
            if t == "DRONE_JOIN":
                try:
                    p = json.loads(pay)
                    d["mac"] = p.get("mac","")
                except: pass
            sio.emit("drone_update", {"drones": state["drones"]})

    # ── Drone offline ─────────────────────────────────────────
    elif t == "DRONE_LOST":
        if frm in state["drones"]:
            state["drones"][frm]["active"] = False
            sio.emit("drone_update", {"drones": state["drones"]})
        log_event({"type": "DRONE_LOST", "drone": frm})

    # ── Service registration ──────────────────────────────────
    elif t == "SERVICE_REG":
        try:
            p = json.loads(pay)
            u = state["users"].setdefault(uid, {
                "uid": uid, "drone": p.get("drone",""), "connTime": ts,
                "offering": "", "requesting": "", "status": "active",
                "lat": None, "lng": None
            })
            if p.get("role") == "offer":
                u["offering"] = p.get("srv","")
            else:
                u["requesting"] = p.get("srv","")
            u["drone"] = p.get("drone", u.get("drone",""))
            u["status"] = "active"
            sio.emit("users_update", {"users": list(state["users"].values())})
            log_event({"type":"SERVICE_REG","uid":uid,"role":p.get("role"),"srv":p.get("srv"),"drone":frm})
        except: pass

    # ── GPS update from node ──────────────────────────────────
    elif t == "GPS_UPDATE":
        try:
            p = json.loads(pay)
            node_uid = p.get("uid", uid)
            lat = p.get("lat")
            lng = p.get("lng")
            if node_uid and lat is not None and lng is not None:
                u = state["users"].setdefault(node_uid, {
                    "uid": node_uid, "drone": frm, "connTime": ts,
                    "offering": "", "requesting": "", "status": "active",
                    "lat": None, "lng": None
                })
                u["lat"] = lat
                u["lng"] = lng
                u["drone"] = p.get("drone", frm)
                u["status"] = "active"
                sio.emit("gps_update", {"uid": node_uid, "lat": lat, "lng": lng, "drone": u["drone"]})
                sio.emit("users_update", {"users": list(state["users"].values())})
        except: pass

    # ── Chat ──────────────────────────────────────────────────
    elif t == "CHAT":
        try:
            p = json.loads(pay)
            entry = {
                "from":  p.get("from", uid),
                "to":    p.get("to",""),
                "text":  p.get("text",""),
                "drone": frm,
                "ts":    ts,
            }
            state["chats"].insert(0, entry)
            if len(state["chats"]) > 1000:
                state["chats"] = state["chats"][:1000]
            sio.emit("new_chat", entry)
            log_event({"type":"CHAT", **entry})
        except: pass

    # ── Master boot ───────────────────────────────────────────
    elif t == "MASTER_BOOT":
        sio.emit("master_boot", {"mac": pay or msg.get("mac","")})
        log_event({"type":"MASTER_BOOT"})

    # ── User disconnect from drone ────────────────────────────
    elif t == "USER_LEFT":
        if uid and uid in state["users"]:
            state["users"][uid]["status"] = "left"
            state["users"][uid]["leftTime"] = ts
            sio.emit("users_update", {"users": list(state["users"].values())})
            log_event({"type":"USER_LEFT","uid":uid,"drone":frm})

    # Forward raw to dashboard
    sio.emit("raw_msg", msg)

# ── Send command to ESP via serial ───────────────────────────
def serial_send(obj):
    if ser and ser.is_open:
        with serial_lock:
            ser.write((json.dumps(obj) + "\n").encode())

# ── REST API ──────────────────────────────────────────────────
@app.route("/api/state")
def api_state():
    return jsonify({
        "drones":   state["drones"],
        "users":    list(state["users"].values()),
        "chatCount":len(state["chats"]),
        "serial":   state["serial_ok"],
        "ticker":   state["news_ticker"],
    })

@app.route("/api/chats")
def api_chats():
    uid = request.args.get("uid","")
    if uid:
        filtered = [c for c in state["chats"]
                    if c.get("from")==uid or c.get("to")==uid]
        return jsonify(filtered[:200])
    return jsonify(state["chats"][:200])

@app.route("/api/send", methods=["POST"])
def api_send():
    d = request.json or {}
    serial_send({"type":"CMD","to":d.get("to","BROADCAST"),
                 "userId":d.get("userId",""), "payload":d.get("text","")})
    return jsonify({"ok":True})

@app.route("/api/events")
def api_events():
    return jsonify(state["events"][:100])

@app.route("/api/ticker", methods=["POST"])
def api_ticker():
    """Set the news ticker message and broadcast to all dashboard clients."""
    d = request.json or {}
    msg = d.get("message","").strip()
    state["news_ticker"] = msg
    sio.emit("ticker_update", {"message": msg})
    log_event({"type":"TICKER","text":msg})
    return jsonify({"ok":True})

@app.route("/api/gps", methods=["POST"])
def api_gps():
    """Receive GPS coordinates posted directly from a mobile browser (drone captive portal relay)."""
    d = request.json or {}
    uid  = d.get("uid","")
    lat  = d.get("lat")
    lng  = d.get("lng")
    drone = d.get("drone","?")
    ts   = datetime.now().isoformat()
    if uid and lat is not None and lng is not None:
        u = state["users"].setdefault(uid, {
            "uid": uid, "drone": drone, "connTime": ts,
            "offering": "", "requesting": "", "status": "active",
            "lat": None, "lng": None
        })
        u["lat"] = lat
        u["lng"] = lng
        u["drone"] = drone
        u["status"] = "active"
        sio.emit("gps_update", {"uid": uid, "lat": lat, "lng": lng, "drone": drone})
        sio.emit("users_update", {"users": list(state["users"].values())})
    return jsonify({"ok":True})

# ── SocketIO events ───────────────────────────────────────────
@sio.on("send_cmd")
def on_cmd(data):
    serial_send({"type":"CMD","to":data.get("to","BROADCAST"),
                 "userId":data.get("userId",""),"payload":data.get("text","")})

@sio.on("set_ticker")
def on_set_ticker(data):
    msg = data.get("message","").strip()
    state["news_ticker"] = msg
    sio.emit("ticker_update", {"message": msg})
    log_event({"type":"TICKER","text":msg})

@sio.on("connect")
def on_connect():
    emit("init", {
        "drones": state["drones"],
        "users":  list(state["users"].values()),
        "chats":  state["chats"][:100],
        "serial": state["serial_ok"],
        "ticker": state["news_ticker"],
    })

# ── Dashboard HTML ────────────────────────────────────────────
DASHBOARD_HTML = r"""
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ReliefNet · Master Control</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/socket.io/4.6.1/socket.io.min.js"></script>
<!-- Leaflet for map -->
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#06090f;--sidebar:#0d1117;--card:#111827;--card2:#1a2236;
  --accent:#00d4ff;--accent2:#7c3aed;--green:#10b981;--yellow:#f59e0b;
  --red:#ef4444;--text:#e2e8f0;--muted:#64748b;--border:#1e293b;
}
body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;
     display:flex;flex-direction:column;height:100vh;overflow:hidden}

/* ── NEWS TICKER ── */
.ticker-bar{
  background:linear-gradient(90deg,#0d1117,#111827,#0d1117);
  border-bottom:1px solid var(--border);
  display:flex;align-items:center;gap:0;flex-shrink:0;height:36px;overflow:hidden;
}
.ticker-label{
  background:linear-gradient(135deg,var(--accent),var(--accent2));
  color:#fff;font-size:11px;font-weight:800;letter-spacing:1.5px;
  padding:0 14px;height:100%;display:flex;align-items:center;white-space:nowrap;
  flex-shrink:0;text-transform:uppercase;
}
.ticker-track{flex:1;overflow:hidden;height:100%;position:relative;}
.ticker-inner{
  display:inline-block;white-space:nowrap;font-size:12px;font-weight:500;
  color:var(--text);padding-left:100%;animation:ticker 30s linear infinite;
  line-height:36px;letter-spacing:.3px;
}
.ticker-inner.paused{animation-play-state:paused}
.ticker-inner.no-msg{color:var(--muted);font-style:italic}
@keyframes ticker{from{transform:translateX(0)}to{transform:translateX(-100%)}}

/* ── LAYOUT ── */
.body-row{display:flex;flex:1;overflow:hidden}

/* Sidebar */
.sidebar{width:220px;background:var(--sidebar);border-right:1px solid var(--border);
         display:flex;flex-direction:column;flex-shrink:0}
.logo{padding:20px 16px;border-bottom:1px solid var(--border)}
.logo h1{font-size:20px;font-weight:800;background:linear-gradient(135deg,var(--accent),var(--accent2));
         -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.logo p{font-size:11px;color:var(--muted);margin-top:2px}
.nav{padding:12px 8px;flex:1}
.nav-item{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:10px;
          cursor:pointer;font-size:13px;font-weight:500;color:var(--muted);margin-bottom:4px;transition:.15s}
.nav-item:hover{background:var(--card);color:var(--text)}
.nav-item.active{background:linear-gradient(135deg,#00d4ff1a,#7c3aed1a);
                 border:1px solid #00d4ff33;color:var(--accent)}
.nav-item .nico{font-size:16px}
.serial-badge{margin:12px;padding:10px 12px;border-radius:10px;font-size:12px;
              display:flex;align-items:center;gap:8px;border:1px solid var(--border)}
.serial-badge.ok{background:#10b98111;border-color:#10b98133;color:var(--green)}
.serial-badge.err{background:#ef444411;border-color:#ef444433;color:var(--red)}
/* Main */
.main{flex:1;display:flex;flex-direction:column;overflow:hidden}
.topbar{padding:14px 24px;border-bottom:1px solid var(--border);
        display:flex;align-items:center;gap:16px;background:var(--sidebar)}
.topbar h2{font-size:18px;font-weight:700;flex:1}
.stats-row{display:flex;gap:10px}
.stat{background:var(--card);border:1px solid var(--border);border-radius:10px;
      padding:8px 14px;text-align:center}
.stat .sv{font-size:22px;font-weight:800}
.stat .sl{font-size:11px;color:var(--muted)}
.sv.g{color:var(--green)} .sv.b{color:var(--accent)} .sv.y{color:var(--yellow)}
/* Content area */
.content{flex:1;overflow-y:auto;padding:20px 24px}
/* Views */
.view{display:none} .view.active{display:block}
/* Drones grid */
.drone-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:16px;margin-bottom:24px}
.drone-card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:18px;transition:.2s}
.drone-card.online{border-color:#10b98144}
.drone-card.offline{opacity:.6}
.dc-top{display:flex;align-items:center;gap:12px;margin-bottom:14px}
.dc-icon{width:44px;height:44px;border-radius:12px;display:flex;align-items:center;
         justify-content:center;font-size:22px;flex-shrink:0}
.dc-icon.on{background:linear-gradient(135deg,#10b98122,#10b98144)}
.dc-icon.off{background:var(--card2)}
.dc-name{font-size:16px;font-weight:700}
.dc-mac{font-size:11px;color:var(--muted);font-family:monospace}
.dc-status{margin-left:auto;padding:4px 10px;border-radius:20px;font-size:11px;font-weight:600}
.dc-status.on{background:#10b98122;color:var(--green);border:1px solid #10b98144}
.dc-status.off{background:#ef444411;color:var(--red);border:1px solid #ef444433}
.dc-stat{display:flex;justify-content:space-between;font-size:12px;color:var(--muted);
         padding:8px 0;border-top:1px solid var(--border)}
.dc-stat span{color:var(--text);font-weight:600}
/* Users table */
.tbl-wrap{background:var(--card);border:1px solid var(--border);border-radius:16px;overflow:hidden;margin-bottom:20px}
.tbl-hdr{padding:14px 18px;border-bottom:1px solid var(--border);font-size:14px;font-weight:700;
         display:flex;align-items:center;justify-content:space-between}
.search{background:var(--card2);border:1px solid var(--border);border-radius:8px;
        padding:6px 12px;color:var(--text);font-size:13px;outline:none;width:200px}
table{width:100%;border-collapse:collapse}
th{text-align:left;padding:10px 18px;font-size:11px;color:var(--muted);text-transform:uppercase;
   letter-spacing:.8px;border-bottom:1px solid var(--border);background:#0d1117}
td{padding:12px 18px;font-size:13px;border-bottom:1px solid #0d1117}
tr:last-child td{border-bottom:none}
tr:hover td{background:#ffffff05}
.tag{display:inline-block;padding:3px 10px;border-radius:20px;font-size:11px;font-weight:600}
.tag-food{background:#f59e0b22;color:#f59e0b;border:1px solid #f59e0b44}
.tag-water{background:#06b6d422;color:#06b6d4;border:1px solid #06b6d444}
.tag-accom{background:#8b5cf622;color:#a78bfa;border:1px solid #8b5cf644}
.tag-med{background:#10b98122;color:var(--green);border:1px solid #10b98144}
.tag-none{color:var(--muted)}
.status-active{background:#10b98122;color:var(--green);border:1px solid #10b98144;
               display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}
.status-left{background:#ef444411;color:var(--red);border:1px solid #ef444433;
             display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}
/* Chat log */
.chat-log{background:var(--card);border:1px solid var(--border);border-radius:16px;overflow:hidden}
.chat-msg{padding:12px 18px;border-bottom:1px solid #0d1117;display:grid;
          grid-template-columns:auto 1fr auto;gap:12px;align-items:start}
.cm-avatar{width:34px;height:34px;border-radius:10px;
           background:linear-gradient(135deg,var(--accent2),var(--accent));
           display:flex;align-items:center;justify-content:center;font-size:14px;flex-shrink:0}
.cm-text{font-size:13px;line-height:1.5}
.cm-from{font-weight:600;font-size:12px;color:var(--accent);margin-bottom:3px}
.cm-meta{font-size:11px;color:var(--muted);text-align:right;white-space:nowrap}
/* Send panel */
.send-panel{background:var(--card);border:1px solid var(--border);border-radius:16px;
            padding:18px;margin-bottom:20px}
.send-panel h3{font-size:14px;font-weight:700;margin-bottom:14px}
.send-row{display:flex;gap:10px;margin-bottom:10px}
.inp{flex:1;background:var(--card2);border:1px solid var(--border);border-radius:10px;
     padding:10px 14px;color:var(--text);font-size:14px;outline:none}
.inp:focus{border-color:var(--accent)}
.sel{background:var(--card2);border:1px solid var(--border);border-radius:10px;
     padding:10px 14px;color:var(--text);font-size:14px;outline:none;min-width:120px}
.btn{padding:10px 18px;border:none;border-radius:10px;font-size:13px;font-weight:700;cursor:pointer;transition:.15s}
.btn-pri{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff}
.btn-pri:active{transform:scale(.97)}
.btn-news{background:linear-gradient(135deg,#f59e0b,#ef4444);color:#fff}
/* Events */
.evt{padding:10px 18px;border-bottom:1px solid #0d1117;display:flex;align-items:center;gap:12px;font-size:12px}
.evt-dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.evt-time{color:var(--muted);white-space:nowrap;font-family:monospace}
/* Map */
#mapContainer{background:var(--card);border:1px solid var(--border);border-radius:16px;
              overflow:hidden;margin-bottom:20px;height:480px}
#map{width:100%;height:100%}
.map-legend{padding:12px 18px;border-top:1px solid var(--border);
            display:flex;gap:16px;font-size:12px;color:var(--muted);background:var(--card)}
.leg-item{display:flex;align-items:center;gap:6px}
.leg-dot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
/* Ticker edit panel */
.ticker-panel{background:var(--card);border:1px solid var(--border);border-radius:16px;
              padding:18px;margin-bottom:20px}
.ticker-panel h3{font-size:14px;font-weight:700;margin-bottom:14px;
                 background:linear-gradient(135deg,var(--yellow),var(--red));
                 -webkit-background-clip:text;-webkit-text-fill-color:transparent}
/* Scrollbar */
::-webkit-scrollbar{width:5px} ::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:#1e293b;border-radius:4px}
/* Pulse animation */
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.pulsing{animation:pulse 2s infinite}
/* Responsive topbar */
@media(max-width:900px){.sidebar{width:60px}.logo p,.nav-item span,.serial-badge span{display:none}
.logo h1{font-size:14px}.nav-item{justify-content:center}}
</style>
</head>
<body>

<!-- ── NEWS TICKER ── -->
<div class="ticker-bar">
  <div class="ticker-label">📢 ALERT</div>
  <div class="ticker-track">
    <div class="ticker-inner no-msg" id="tickerInner">No announcements — Control Centre can broadcast messages here</div>
  </div>
</div>

<div class="body-row">
<!-- Sidebar -->
<div class="sidebar">
  <div class="logo">
    <h1>ReliefNet</h1>
    <p>Master Control</p>
  </div>
  <div class="nav">
    <div class="nav-item active" onclick="showView('overview')" id="nav-overview">
      <span class="nico">🗺️</span><span>Overview</span>
    </div>
    <div class="nav-item" onclick="showView('map')" id="nav-map">
      <span class="nico">📍</span><span>Live Map</span>
    </div>
    <div class="nav-item" onclick="showView('drones')" id="nav-drones">
      <span class="nico">🛸</span><span>Drones</span>
    </div>
    <div class="nav-item" onclick="showView('users')" id="nav-users">
      <span class="nico">👥</span><span>Users</span>
    </div>
    <div class="nav-item" onclick="showView('chats')" id="nav-chats">
      <span class="nico">💬</span><span>Messages</span>
    </div>
    <div class="nav-item" onclick="showView('send')" id="nav-send">
      <span class="nico">📡</span><span>Broadcast</span>
    </div>
    <div class="nav-item" onclick="showView('news')" id="nav-news">
      <span class="nico">📰</span><span>News Ticker</span>
    </div>
    <div class="nav-item" onclick="showView('events')" id="nav-events">
      <span class="nico">📋</span><span>Event Log</span>
    </div>
  </div>
  <div class="serial-badge err" id="serialBadge">
    <span>⚡</span><span id="serialText">Serial Off</span>
  </div>
</div>

<!-- Main -->
<div class="main">
  <!-- Topbar -->
  <div class="topbar">
    <h2 id="viewTitle">Overview</h2>
    <div class="stats-row">
      <div class="stat"><div class="sv g" id="statDrones">0</div><div class="sl">Drones Online</div></div>
      <div class="stat"><div class="sv b" id="statUsers">0</div><div class="sl">Active Users</div></div>
      <div class="stat"><div class="sv y" id="statMsgs">0</div><div class="sl">Messages</div></div>
    </div>
  </div>

  <div class="content">

    <!-- OVERVIEW -->
    <div class="view active" id="view-overview">
      <div id="ov-drones" class="drone-grid"></div>
      <div class="tbl-wrap">
        <div class="tbl-hdr">Recent Activity</div>
        <div id="ov-events"></div>
      </div>
    </div>

    <!-- MAP VIEW -->
    <div class="view" id="view-map">
      <div id="mapContainer">
        <div id="map"></div>
      </div>
      <div class="map-legend" id="mapLegend">
        <div class="leg-item"><div class="leg-dot" style="background:var(--green)"></div>Active Node</div>
        <div class="leg-item"><div class="leg-dot" style="background:var(--red)"></div>Left / Offline</div>
        <div class="leg-item"><div class="leg-dot" style="background:var(--yellow)"></div>No GPS Fix</div>
        <span style="margin-left:auto;color:var(--muted)" id="mapNodeCount">0 nodes on map</span>
      </div>
    </div>

    <!-- DRONES -->
    <div class="view" id="view-drones">
      <div id="droneGrid" class="drone-grid"></div>
    </div>

    <!-- USERS -->
    <div class="view" id="view-users">
      <div class="tbl-wrap">
        <div class="tbl-hdr">
          <span>Connected Users</span>
          <input class="search" placeholder="Search UID…" oninput="filterUsers(this.value)" id="userSearch">
        </div>
        <table>
          <thead><tr>
            <th>User ID</th><th>Drone</th><th>Offering</th><th>Requesting</th><th>Status</th><th>GPS</th><th>Since</th>
          </tr></thead>
          <tbody id="usersTbody"></tbody>
        </table>
      </div>
    </div>

    <!-- CHATS -->
    <div class="view" id="view-chats">
      <div class="tbl-wrap">
        <div class="tbl-hdr">
          <span>Message Log</span>
          <input class="search" placeholder="Filter by UID…" oninput="filterChats(this.value)" id="chatSearch">
        </div>
        <div id="chatLogDiv"></div>
      </div>
    </div>

    <!-- SEND -->
    <div class="view" id="view-send">
      <div class="send-panel">
        <h3>📡 Send Message / Command</h3>
        <div class="send-row">
          <select class="sel" id="sendTo">
            <option value="BROADCAST">📢 All Drones</option>
          </select>
          <input class="inp" id="sendText" placeholder="Message or command…">
          <button class="btn btn-pri" onclick="sendCmd()">Send</button>
        </div>
        <div style="font-size:12px;color:var(--muted)">
          Messages are forwarded via Master ESP to all connected slave drones.
        </div>
      </div>
      <div class="tbl-wrap" id="sentLog">
        <div class="tbl-hdr">Sent History</div>
        <div id="sentLogDiv"></div>
      </div>
    </div>

    <!-- NEWS TICKER MANAGER -->
    <div class="view" id="view-news">
      <div class="ticker-panel">
        <h3>📰 Broadcast News Ticker</h3>
        <p style="font-size:12px;color:var(--muted);margin-bottom:14px">
          Type an important message below and click Broadcast. It will scroll across the top of this dashboard
          and will also be sent as a CMD broadcast to all drones (visible on the drone's event log).
        </p>
        <div class="send-row">
          <input class="inp" id="newsText" placeholder="Type important announcement here…"
                 onkeydown="if(event.key==='Enter')sendTicker()">
          <button class="btn btn-news" onclick="sendTicker()">📢 Broadcast</button>
        </div>
        <button class="btn" style="background:var(--card2);color:var(--muted);margin-top:6px;width:auto;padding:8px 16px"
                onclick="clearTicker()">✕ Clear Ticker</button>
      </div>
      <div class="tbl-wrap">
        <div class="tbl-hdr">Current Ticker</div>
        <div style="padding:16px 18px;font-size:13px" id="currentTickerDisplay">
          <em style="color:var(--muted)">No message set</em>
        </div>
      </div>
    </div>

    <!-- EVENTS -->
    <div class="view" id="view-events">
      <div class="tbl-wrap">
        <div class="tbl-hdr">System Event Log</div>
        <div id="eventLogDiv"></div>
      </div>
    </div>

  </div>
</div>
</div><!-- end body-row -->

<script>
const socket = io();
let drones={}, users=[], chats=[], events=[], msgCount=0;
let sentHistory=[];
let leafletMap=null, mapMarkers={};
let currentTicker='';

// ── Tag helpers ───────────────────────────────────────────────
const srvTag = s => {
  const m={FOOD:'tag-food 🍱 Food',WATER:'tag-water 💧 Water',
           ACCOM:'tag-accom 🏕️ Accom',MED:'tag-med 💊 Medicine'};
  if(!s) return '<span class="tag tag-none">—</span>';
  const parts=m[s]?.split(' ') || [];
  return `<span class="tag ${parts[0]}">${parts.slice(1).join(' ')}</span>`;
};

// ── View switching ────────────────────────────────────────────
function showView(v){
  document.querySelectorAll('.view').forEach(e=>e.classList.remove('active'));
  document.querySelectorAll('.nav-item').forEach(e=>e.classList.remove('active'));
  document.getElementById('view-'+v).classList.add('active');
  document.getElementById('nav-'+v).classList.add('active');
  const titles={overview:'Overview',map:'Live Node Map',drones:'Drone Fleet',users:'Connected Users',
                chats:'Message Log',send:'Broadcast',news:'News Ticker',events:'Event Log'};
  document.getElementById('viewTitle').textContent=titles[v]||v;
  if(v==='map') initMap();
}

// ── MAP ───────────────────────────────────────────────────────
function initMap(){
  if(leafletMap) { leafletMap.invalidateSize(); updateAllMapMarkers(); return; }
  leafletMap = L.map('map').setView([20, 78], 5);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{
    attribution:'© OpenStreetMap contributors',maxZoom:19
  }).addTo(leafletMap);
  updateAllMapMarkers();
}

function makeIcon(color){
  return L.divIcon({
    className:'',
    html:`<div style="width:14px;height:14px;border-radius:50%;background:${color};
          border:2px solid rgba(255,255,255,.6);box-shadow:0 0 8px ${color}80"></div>`,
    iconSize:[14,14],iconAnchor:[7,7],popupAnchor:[0,-10]
  });
}

function updateAllMapMarkers(){
  if(!leafletMap) return;
  let count=0;
  users.forEach(u=>{
    if(u.lat==null||u.lng==null) return;
    count++;
    const color = u.status==='left' ? '#ef4444' : '#10b981';
    const label = `<b>${u.uid.slice(-8)}</b><br>
      Drone: ${u.drone||'?'}<br>
      Status: <span style="color:${color}">${u.status||'active'}</span><br>
      ${u.offering?'Offering: '+u.offering+'<br>':''}
      ${u.requesting?'Needs: '+u.requesting+'<br>':''}
      GPS: ${u.lat.toFixed(5)}, ${u.lng.toFixed(5)}`;
    if(mapMarkers[u.uid]){
      mapMarkers[u.uid].setLatLng([u.lat,u.lng]);
      mapMarkers[u.uid].setIcon(makeIcon(color));
      mapMarkers[u.uid].setPopupContent(label);
    } else {
      mapMarkers[u.uid] = L.marker([u.lat,u.lng],{icon:makeIcon(color)})
        .bindPopup(label).addTo(leafletMap);
    }
  });
  document.getElementById('mapNodeCount').textContent = count+' nodes on map';
}

function updateMapMarker(uid, lat, lng, drone){
  if(!leafletMap) return;
  const u = users.find(x=>x.uid===uid)||{};
  const color = (u.status==='left') ? '#ef4444' : '#10b981';
  const label = `<b>${uid.slice(-8)}</b><br>Drone: ${drone||'?'}<br>GPS: ${lat.toFixed(5)}, ${lng.toFixed(5)}`;
  if(mapMarkers[uid]){
    mapMarkers[uid].setLatLng([lat,lng]).setPopupContent(label);
  } else {
    mapMarkers[uid] = L.marker([lat,lng],{icon:makeIcon(color)})
      .bindPopup(label).addTo(leafletMap);
  }
  let count=Object.values(mapMarkers).length;
  document.getElementById('mapNodeCount').textContent = count+' nodes on map';
}

// ── TICKER ────────────────────────────────────────────────────
function setTickerDisplay(msg){
  currentTicker=msg;
  const el=document.getElementById('tickerInner');
  const cd=document.getElementById('currentTickerDisplay');
  if(!msg){
    el.className='ticker-inner no-msg';
    el.textContent='No announcements — Control Centre can broadcast messages here';
    cd.innerHTML='<em style="color:var(--muted)">No message set</em>';
  } else {
    el.className='ticker-inner';
    el.textContent='📢  '+msg+'  ·  📢  '+msg+'  ·  📢  '+msg;
    cd.innerHTML=`<span style="color:var(--yellow)">📢</span> <strong>${msg}</strong>`;
  }
  // Restart animation
  el.style.animation='none';
  setTimeout(()=>{ el.style.animation=''; }, 10);
}

function sendTicker(){
  const msg=document.getElementById('newsText').value.trim();
  if(!msg) return;
  socket.emit('set_ticker',{message:msg});
  // Also broadcast as CMD so drones see it
  socket.emit('send_cmd',{to:'BROADCAST',text:'[ALERT] '+msg,userId:'MASTER'});
  document.getElementById('newsText').value='';
}

function clearTicker(){
  socket.emit('set_ticker',{message:''});
}

// ── Renderers ─────────────────────────────────────────────────
function renderDrones(container){
  const el=document.getElementById(container);
  if(!el) return;
  const ids=Object.keys(drones);
  if(!ids.length){el.innerHTML='<div style="color:var(--muted);padding:20px;font-size:13px">No drones registered yet.</div>';return;}
  el.innerHTML=ids.map(id=>{
    const d=drones[id];
    const on=d.active;
    const users_on_drone=users.filter(u=>u.drone===id);
    return `<div class="drone-card ${on?'online':'offline'}">
      <div class="dc-top">
        <div class="dc-icon ${on?'on':'off'}">${on?'🛸':'📡'}</div>
        <div><div class="dc-name">${id}</div><div class="dc-mac">${d.mac||'—'}</div></div>
        <div class="dc-status ${on?'on':'off'}">${on?'ONLINE':'OFFLINE'}</div>
      </div>
      <div class="dc-stat"><div>Users</div><span>${users_on_drone.length}</span></div>
      <div class="dc-stat"><div>Last seen</div><span>${d.lastSeen?new Date(d.lastSeen).toLocaleTimeString():'—'}</span></div>
    </div>`;
  }).join('');
}

function renderUsers(filter=''){
  const f=filter.toLowerCase();
  const tbody=document.getElementById('usersTbody');
  if(!tbody) return;
  const rows=users.filter(u=>!f||u.uid.toLowerCase().includes(f));
  tbody.innerHTML=rows.map(u=>{
    const st=u.status==='left'
      ?'<span class="status-left">Left</span>'
      :'<span class="status-active">Active</span>';
    const gps=(u.lat!=null&&u.lng!=null)
      ?`<span style="font-family:monospace;font-size:11px;color:var(--accent)">${u.lat.toFixed(4)}, ${u.lng.toFixed(4)}</span>`
      :'<span style="color:var(--muted)">—</span>';
    return `<tr>
      <td style="font-family:monospace;font-size:12px">${u.uid}</td>
      <td><span class="tag" style="background:#00d4ff11;color:var(--accent);border:1px solid #00d4ff33">${u.drone||'?'}</span></td>
      <td>${srvTag(u.offering)}</td>
      <td>${srvTag(u.requesting)}</td>
      <td>${st}</td>
      <td>${gps}</td>
      <td style="color:var(--muted)">${u.connTime?new Date(u.connTime).toLocaleTimeString():'—'}</td>
    </tr>`;
  }).join('');
}

function renderChats(filter=''){
  const f=filter.toLowerCase();
  const el=document.getElementById('chatLogDiv');
  if(!el) return;
  const rows=chats.filter(c=>!f||c.from?.toLowerCase().includes(f)||c.to?.toLowerCase().includes(f));
  el.innerHTML=rows.slice(0,100).map(c=>`<div class="chat-msg">
    <div class="cm-avatar">💬</div>
    <div class="cm-text">
      <div class="cm-from">${c.from?.slice(-8)||'?'} → ${c.to?.slice(-8)||'?'} <span style="color:var(--muted);font-weight:400">via ${c.drone||'?'}</span></div>
      ${c.text}
    </div>
    <div class="cm-meta">${c.ts?new Date(c.ts).toLocaleTimeString():'—'}</div>
  </div>`).join('') || '<div style="padding:20px;color:var(--muted);font-size:13px">No messages yet.</div>';
}

function renderEvents(){
  const el=document.getElementById('eventLogDiv');
  if(!el) return;
  const colors={DRONE_LOST:'var(--red)',DRONE_JOIN:'var(--green)',DRONE_BOOT:'var(--green)',
                SERVICE_REG:'var(--accent)',CHAT:'var(--yellow)',MASTER_BOOT:'var(--accent2)',
                USER_LEFT:'var(--red)',GPS_UPDATE:'var(--green)',TICKER:'var(--yellow)'};
  el.innerHTML=events.slice(0,80).map(e=>`<div class="evt">
    <div class="evt-dot" style="background:${colors[e.type]||'var(--muted)'}"></div>
    <div style="flex:1">${e.type} ${e.drone||e.uid||''} ${e.text||''}</div>
    <div class="evt-time">${e._ts?new Date(e._ts).toLocaleTimeString():'—'}</div>
  </div>`).join('') || '<div style="padding:20px;color:var(--muted)">No events yet.</div>';
}

function renderOverviewEvents(){
  const el=document.getElementById('ov-events');
  if(!el) return;
  el.innerHTML=events.slice(0,10).map(e=>`<div class="evt">
    <div class="evt-dot" style="background:#00d4ff88"></div>
    <div style="flex:1;font-size:12px">${e.type} ${e.drone||e.uid||''}</div>
    <div class="evt-time">${e._ts?new Date(e._ts).toLocaleTimeString():'—'}</div>
  </div>`).join('');
}

function renderSendDroneSelect(){
  const sel=document.getElementById('sendTo');
  if(!sel) return;
  const opts=Object.keys(drones).map(id=>`<option value="${id}">🛸 ${id}</option>`).join('');
  sel.innerHTML='<option value="BROADCAST">📢 All Drones</option>'+opts;
}

function renderAll(){
  renderDrones('ov-drones');
  renderDrones('droneGrid');
  renderUsers();
  renderChats();
  renderEvents();
  renderOverviewEvents();
  renderSendDroneSelect();
  const on=Object.values(drones).filter(d=>d.active).length;
  document.getElementById('statDrones').textContent=on;
  const active=users.filter(u=>u.status!=='left').length;
  document.getElementById('statUsers').textContent=active;
  document.getElementById('statMsgs').textContent=msgCount;
  updateAllMapMarkers();
}

function filterUsers(v){renderUsers(v);}
function filterChats(v){renderChats(v);}

function sendCmd(){
  const to=document.getElementById('sendTo').value;
  const text=document.getElementById('sendText').value.trim();
  if(!text) return;
  socket.emit('send_cmd',{to,text,userId:'MASTER'});
  const entry={to,text,ts:new Date().toISOString()};
  sentHistory.unshift(entry);
  const div=document.getElementById('sentLogDiv');
  div.innerHTML=sentHistory.slice(0,20).map(s=>`<div class="evt">
    <div class="evt-dot" style="background:var(--accent)"></div>
    <div style="flex:1">→ ${s.to}: ${s.text}</div>
    <div class="evt-time">${new Date(s.ts).toLocaleTimeString()}</div>
  </div>`).join('');
  document.getElementById('sendText').value='';
}

// ── Socket events ─────────────────────────────────────────────
socket.on('init', d=>{
  drones=d.drones||{}; users=d.users||[]; chats=d.chats||[];
  msgCount=chats.length;
  renderAll();
  setSerial(d.serial);
  if(d.ticker) setTickerDisplay(d.ticker);
});

socket.on('drone_update',d=>{
  drones=d.drones||{};
  renderDrones('ov-drones');renderDrones('droneGrid');renderSendDroneSelect();
  const on=Object.values(drones).filter(d=>d.active).length;
  document.getElementById('statDrones').textContent=on;
});

socket.on('users_update',d=>{
  users=d.users||[];
  renderUsers();
  const active=users.filter(u=>u.status!=='left').length;
  document.getElementById('statUsers').textContent=active;
  updateAllMapMarkers();
});

socket.on('new_chat',m=>{
  chats.unshift(m);msgCount++;
  renderChats();
  document.getElementById('statMsgs').textContent=msgCount;
});

socket.on('raw_msg',m=>{
  events.unshift({...m,_ts:new Date().toISOString()});
  if(events.length>200)events.pop();
  renderEvents();renderOverviewEvents();
});

socket.on('gps_update',d=>{
  // Update user in local list
  const u=users.find(x=>x.uid===d.uid);
  if(u){u.lat=d.lat;u.lng=d.lng;}
  updateMapMarker(d.uid,d.lat,d.lng,d.drone);
});

socket.on('ticker_update',d=>{
  setTickerDisplay(d.message||'');
});

socket.on('serial_status',d=>setSerial(d.ok));

socket.on('master_boot',d=>{
  events.unshift({type:'MASTER_BOOT',_ts:new Date().toISOString()});
  renderEvents();renderOverviewEvents();
});

function setSerial(ok){
  const b=document.getElementById('serialBadge');
  const t=document.getElementById('serialText');
  b.className='serial-badge '+(ok?'ok':'err');
  t.textContent=ok?'Serial OK':'Serial Off';
}

// Poll for updates (backup to websockets)
setInterval(()=>fetch('/api/state').then(r=>r.json()).then(d=>{
  drones=d.drones; users=d.users; msgCount=d.chatCount;
  renderAll();
  if(d.ticker!==undefined && d.ticker!==currentTicker) setTickerDisplay(d.ticker);
}),5000);
</script>
</body>
</html>
"""

@app.route("/")
def dashboard():
    return render_template_string(DASHBOARD_HTML)

# ── Entry point ───────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ReliefNet Master Server")
    parser.add_argument("--port",    default="COM3",    help="Serial port of Master ESP")
    parser.add_argument("--webport", default=5000, type=int, help="Web server port")
    args = parser.parse_args()

    print(f"""
╔══════════════════════════════════════╗
║      ReliefNet Master Server         ║
║  Serial: {args.port:<28}║
║  Web:    http://localhost:{args.webport:<12}║
╚══════════════════════════════════════╝
""")
    # Start serial reader thread
    t = threading.Thread(target=serial_reader, args=(args.port,), daemon=True)
    t.start()

    sio.run(app, host="0.0.0.0", port=args.webport, debug=False, use_reloader=False)
