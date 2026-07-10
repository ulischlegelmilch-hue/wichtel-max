// Wichtel Max – Backend
// 1) Web-App + REST-API (express) auf :8080  – damit tippt Papa Nachrichten
//    (Nachrichten, Aufgaben, OTA, Fernsteuerung laufen ALLE über HTTP).
// 2) Online-Status des Geräts per HTTP-Heartbeat (POST /api/heartbeat) –
//    das Gerät meldet sich bei jedem Aufwachen; "online" = kürzlich gesehen.
// 3) Optional: MQTT-Broker (aedes) auf :1883 für die ältere Firmware
//    (ENABLE_MQTT=1). In der Cloud (Render/Railway) ENABLE_MQTT=0 lassen –
//    dort ist TCP 1883 meist dicht; das Gerät braucht MQTT auch nicht mehr.
//
// Cloud-Deploy: PORT wird vom Hoster gesetzt (schon berücksichtigt). Für
// dauerhafte Daten STATE_DIR auf ein gemountetes Volume zeigen lassen.

import { createServer } from "net";
import { createServer as createHttp } from "http";
import { fileURLToPath } from "url";
import { dirname, join } from "path";
import { readFileSync, writeFileSync, existsSync, statSync, mkdirSync } from "fs";
import express from "express";

const __dirname = dirname(fileURLToPath(import.meta.url));

const DEVICE_ID = process.env.DEVICE_ID || "max";
const MQTT_PORT = 1883;
const HTTP_PORT = process.env.PORT || 8080;
// MQTT ist nur noch für die alte Firmware nötig. Standard: an (lokal). In der
// Cloud ENABLE_MQTT=0 setzen (kein offener TCP-Port 1883 nötig).
const ENABLE_MQTT = !["0", "false", "no"].includes(String(process.env.ENABLE_MQTT || "1").toLowerCase());
// Wo state.json liegt (für Cloud: gemountetes Volume, sonst neben server.js).
const STATE_DIR = process.env.STATE_DIR || __dirname;
try { mkdirSync(STATE_DIR, { recursive: true }); } catch { /* egal */ }
const STATE_FILE = join(STATE_DIR, "state.json");

// ---- OTA / Firmware-Fernupdate ------------------------------------------
// Neue Firmware verteilen: firmware.bin nach backend/firmware/ legen und die
// Version in manifest.json hochzählen (oder das Skript publish-firmware.mjs
// nutzen). Das Gerät lädt sie beim nächsten Aufwachen automatisch.
const FW_DIR = join(__dirname, "firmware");
const FW_BIN = join(FW_DIR, "firmware.bin");
const FW_MANIFEST = join(FW_DIR, "manifest.json");
try { mkdirSync(FW_DIR, { recursive: true }); } catch { /* egal */ }

function otaInfo() {
  let version = 0;
  if (existsSync(FW_MANIFEST)) {
    try { version = Number(JSON.parse(readFileSync(FW_MANIFEST, "utf8")).version) || 0; } catch { /* egal */ }
  }
  const hasBin = existsSync(FW_BIN);
  const size = hasBin ? statSync(FW_BIN).size : 0;
  return { version, hasBin, size };
}

const T_MESSAGE = `wichtel/${DEVICE_ID}/message`;
const T_CMD     = `wichtel/${DEVICE_ID}/cmd`;
const T_STATUS  = `wichtel/${DEVICE_ID}/status`;

// ---- Zustand (einfach als JSON-Datei) ------------------------------------
let state = { last: null, history: [], tasks: [], nextTaskId: 1 };
if (existsSync(STATE_FILE)) {
  try { state = JSON.parse(readFileSync(STATE_FILE, "utf8")); } catch { /* ignore */ }
}
// Migration älterer state.json ohne Aufgaben-Felder
if (!Array.isArray(state.tasks)) state.tasks = [];
if (typeof state.nextTaskId !== "number") state.nextTaskId = 1;
// Fernsteuerung: Befehls-Warteschlange + Log + Fern-Konfiguration
if (!state.remote) state.remote = { queue: [], log: [] };
if (!state.config) state.config = { pollMin: 30, nightStart: 22, nightEnd: 6, volume: 80 };
// Sterne-Belohnung
if (typeof state.stars !== "number") state.stars = 0;
// Antworten von Max (Zwei-Wege)
if (!Array.isArray(state.replies)) state.replies = [];
// Aufgaben-Feld für Zeitraum (once/day/week/month) nachrüsten
for (const t of state.tasks) {
  if (t.scope === undefined) t.scope = "once";
  if (t.lastDoneDate === undefined) t.lastDoneDate = null;
}
function saveState() {
  try { writeFileSync(STATE_FILE, JSON.stringify(state, null, 2)); } catch { /* ignore */ }
}
let deviceFw = 0;      // vom Gerät gemeldete Firmware-Version
let deviceBatt = -1;   // vom Gerät gemeldeter Akkustand in % (-1 = unbekannt)
let deviceRssi = 0;    // WLAN-Signalstärke (dBm), zuletzt gemeldet
let lastSeen = 0;      // ms-Zeitstempel, wann sich das Gerät zuletzt gemeldet hat

// Das Gerät schläft die meiste Zeit tief und meldet sich nur beim Aufwachen
// (alle pollMin Minuten) per HTTP-Heartbeat. "online" heißt daher: kürzlich
// gesehen (innerhalb ~2 Poll-Intervalle) – ehrlicher als ein MQTT-Last-Will,
// der bei einem Deep-Sleep-Gerät fast immer "offline" zeigen würde.
function onlineTtlMs() { return (Number(state.config?.pollMin || 30) * 2 + 2) * 60000; }
function isOnline() { return lastSeen > 0 && (Date.now() - lastSeen) < onlineTtlMs(); }
function markDeviceSeen({ fw, batt, rssi } = {}) {
  lastSeen = Date.now();
  if (Number.isFinite(fw))   deviceFw = fw;
  if (Number.isFinite(batt)) deviceBatt = batt;
  if (Number.isFinite(rssi)) deviceRssi = rssi;
}

// ---- Optional: MQTT-Broker + interner Client (nur ENABLE_MQTT) -----------
// Nur noch für die ältere Firmware, die den Online-Status per MQTT meldet und
// Nachrichten "retained" erwartet. In der Cloud aus lassen (ENABLE_MQTT=0).
let client = null;
if (ENABLE_MQTT) {
  const { default: Aedes } = await import("aedes");
  const { default: mqtt } = await import("mqtt");
  const aedes = new Aedes();
  const broker = createServer(aedes.handle);
  broker.listen(MQTT_PORT, () => console.log(`MQTT-Broker läuft auf :${MQTT_PORT}`));
  client = mqtt.connect(`mqtt://127.0.0.1:${MQTT_PORT}`);
  client.on("connect", () => {
    client.subscribe(T_STATUS);
    console.log("Backend-Client mit Broker verbunden");
  });
  client.on("message", (topic, payload) => {
    if (topic === T_STATUS) {
      try {
        const s = JSON.parse(payload.toString());
        if (s.online) markDeviceSeen({ fw: s.fw, batt: s.batt, rssi: s.rssi });
      } catch { /* ignore */ }
    }
  });
} else {
  console.log("MQTT deaktiviert (ENABLE_MQTT=0) – Online-Status per HTTP-Heartbeat");
}

function sendMessage(text, from = "Wichtel") {
  const msg = { text, from, ts: Date.now() };
  state.last = msg;
  state.history.unshift(msg);
  state.history = state.history.slice(0, 50);
  saveState();
  // Das Gerät holt Nachrichten per HTTP (/api/state). Zusätzlich retained via
  // MQTT, falls die ältere MQTT-Firmware läuft (ENABLE_MQTT).
  if (client) client.publish(T_MESSAGE, JSON.stringify(msg), { qos: 1, retain: true });
  return msg;
}

// Nachricht löschen. Identifikator vom Gerät = ts in SEKUNDEN (Firmware speichert
// ts/1000), daher hier auf Sekunden-Ebene vergleichen. Ist es die letzte/retained
// Nachricht, wird auch state.last + das retained MQTT-Topic aktualisiert, damit sie
// nicht wiederkommt.
function deleteMessage(tsSec) {
  const before = state.history.length;
  state.history = state.history.filter((h) => Math.floor(h.ts / 1000) !== tsSec);
  if (state.last && Math.floor(state.last.ts / 1000) === tsSec) {
    state.last = state.history[0] || null;
    if (client) client.publish(T_MESSAGE, state.last ? JSON.stringify(state.last) : "", { qos: 1, retain: true });
  }
  const changed = state.history.length !== before;
  if (changed) saveState();
  return changed;
}
function sendCmd(action) {
  if (client) client.publish(T_CMD, JSON.stringify({ action }));
}

// ---- Aufgaben ------------------------------------------------------------
// Eine Aufgabe ist eine kleine To-do für Max. Das Gerät holt offene Aufgaben
// per HTTP (/api/state), zeigt das "Neue Aufgabe"-Gesicht und meldet per
// POST /api/task/:id/done zurück, wenn Max sie am Gerät erledigt hat.
// ---- Zeitraum (scope): once | day | week | month -----------------------
function ymd(d) { return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0"); }
function ym(d) { return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0"); }
function isoWeek(d) {
  const dt = new Date(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()));
  const dayNum = (dt.getUTCDay() + 6) % 7;
  dt.setUTCDate(dt.getUTCDate() - dayNum + 3);
  const firstThu = new Date(Date.UTC(dt.getUTCFullYear(), 0, 4));
  const week = 1 + Math.round(((dt - firstThu) / 86400000 - 3 + ((firstThu.getUTCDay() + 6) % 7)) / 7);
  return dt.getUTCFullYear() + "-W" + String(week).padStart(2, "0");
}
// Ist die Aufgabe im AKTUELLEN Zeitraum schon erledigt? (bestimmt „offen")
function doneThisPeriod(t) {
  if (t.scope === "once") return !!t.done;
  if (!t.lastDoneDate) return false;
  const d = new Date(t.lastDoneDate + "T12:00:00");
  const now = new Date();
  if (t.scope === "day")   return ymd(d) === ymd(now);
  if (t.scope === "week")  return isoWeek(d) === isoWeek(now);
  if (t.scope === "month") return ym(d) === ym(now);
  return false;
}
// Aufgaben mit berechnetem Status (offen im aktuellen Zeitraum?)
function annotateTasks() {
  return state.tasks.map(t => ({ ...t, done: doneThisPeriod(t), open: !doneThisPeriod(t) }));
}

function addTask(text, from = "Wichtel", scope = "once") {
  if (!["once", "day", "week", "month"].includes(scope)) scope = "once";
  const t = { id: state.nextTaskId++, text, from, ts: Date.now(),
              scope, done: false, doneAt: null, lastDoneDate: null };
  state.tasks.unshift(t);
  state.tasks = state.tasks.slice(0, 50);
  saveState();
  return t;
}
// Erledigt im aktuellen Zeitraum markieren; gibt Sterne + „alle offenen erledigt?" zurück.
function markTaskDone(id) {
  const t = state.tasks.find(x => x.id === id);
  if (!t) return null;
  if (!doneThisPeriod(t)) {
    if (t.scope === "once") { t.done = true; t.doneAt = Date.now(); }
    else t.lastDoneDate = ymd(new Date());
    state.stars += 1;
    saveState();
  }
  const anyOpen = state.tasks.some(x => !doneThisPeriod(x));
  return { task: t, stars: state.stars, allDone: !anyOpen };
}
function removeTask(id) {
  const n = state.tasks.length;
  state.tasks = state.tasks.filter(x => x.id !== id);
  if (state.tasks.length !== n) { saveState(); return true; }
  return false;
}

// ---- Fernsteuerung (asynchron über das Poll-Modell) ---------------------
// Papa legt Befehle/Config ab; das Gerät holt sie beim Aufwachen (GET /api/pull),
// führt die Konsolen-Befehle aus und meldet die Ausgabe zurück (POST /api/log).
function queueCmd(cmd) {
  state.remote.queue.push({ cmd, ts: Date.now() });
  state.remote.queue = state.remote.queue.slice(-30);
  saveState();
}
function pushReply(text) {
  state.replies.unshift({ text, ts: Date.now() });
  state.replies = state.replies.slice(0, 30);
  saveState();
}
function pushLog(text) {
  const ts = new Date().toLocaleString("de-DE");
  for (const line of String(text).split("\n")) {
    if (line.length) state.remote.log.unshift({ line, ts });
  }
  state.remote.log = state.remote.log.slice(0, 80);
  saveState();
}

// ---- 2) Web-App + REST-API ----------------------------------------------
const app = express();
app.use(express.json());
// CORS offen (lokales Dev-Tool): damit der Display-Simulator aus preview/ live
// auf /api/state zugreifen kann, auch wenn er per file:// geöffnet wird.
app.use((req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  res.header("Access-Control-Allow-Headers", "Content-Type");
  if (req.method === "OPTIONS") return res.sendStatus(204);   // Preflight
  next();
});
app.use(express.static(join(__dirname, "..", "webapp")));

app.get("/api/state", (_req, res) => {
  res.json({ last: state.last, history: state.history, tasks: annotateTasks(), stars: state.stars,
             replies: state.replies, deviceOnline: isOnline(),
             deviceLastSeen: lastSeen || null, deviceRssi,
             deviceFw, deviceBatt, ota: otaInfo(),
             config: state.config, remoteLog: state.remote.log, queueLen: state.remote.queue.length,
             deviceId: DEVICE_ID });
});

// ---- Heartbeat: das Gerät meldet sich beim Aufwachen (Online-Status) ------
// Ersetzt den früheren MQTT-Status. Das Gerät ruft das einmal pro Aufwachen.
app.post("/api/heartbeat", (req, res) => {
  const b = req.body || {};
  markDeviceSeen({
    fw:   Number.isFinite(+b.fw)   ? +b.fw   : undefined,
    batt: Number.isFinite(+b.batt) ? +b.batt : undefined,
    rssi: Number.isFinite(+b.rssi) ? +b.rssi : undefined,
  });
  res.json({ ok: true });
});

// ---- Fernsteuerungs-API --------------------------------------------------
// Papa: Befehl in die Warteschlange legen
app.post("/api/cmd-queue", (req, res) => {
  const cmd = (req.body?.cmd || "").toString().trim();
  if (!cmd) return res.status(400).json({ error: "cmd fehlt" });
  queueCmd(cmd);
  res.json({ ok: true, queued: state.remote.queue.length });
});
// Papa: Fern-Konfiguration setzen
app.post("/api/config", (req, res) => {
  const c = req.body || {};
  for (const k of ["pollMin", "nightStart", "nightEnd", "volume"])
    if (typeof c[k] === "number") state.config[k] = c[k];
  saveState();
  res.json({ ok: true, config: state.config });
});
// Gerät: offene Befehle + Config holen (leert die Warteschlange)
app.get("/api/pull", (_req, res) => {
  markDeviceSeen();                          // Poll zählt als Lebenszeichen
  const commands = state.remote.queue.map(q => q.cmd);
  state.remote.queue = [];
  saveState();
  res.json({ commands, config: state.config });
});
// Gerät: Ausgabe/Log zurückmelden
app.post("/api/log", (req, res) => {
  const text = (req.body?.text || "").toString();
  if (text) pushLog(text);
  res.json({ ok: true });
});

// ---- OTA-API -------------------------------------------------------------
// Das Gerät fragt hier die neueste Firmware-Version ab und lädt ggf. die .bin.
app.get("/api/ota", (_req, res) => res.json(otaInfo()));

app.get("/api/firmware.bin", (_req, res) => {
  if (!existsSync(FW_BIN)) return res.status(404).json({ error: "keine Firmware hinterlegt" });
  res.setHeader("Content-Type", "application/octet-stream");
  res.setHeader("Content-Length", statSync(FW_BIN).size);
  res.sendFile(FW_BIN);
});

app.post("/api/message", (req, res) => {
  const text = (req.body?.text || "").toString().trim();
  const from = (req.body?.from || "Wichtel").toString().trim() || "Wichtel";
  if (!text) return res.status(400).json({ error: "text fehlt" });
  res.json({ ok: true, message: sendMessage(text, from) });
});

// Nachricht löschen (vom Gerät oder der Web-App). ts in Sekunden.
app.post("/api/message/delete", (req, res) => {
  const ts = Number(req.body?.ts);
  if (!ts) return res.status(400).json({ error: "ts fehlt" });
  res.json({ ok: deleteMessage(ts) });
});

// Alles aufräumen: Nachrichten, Aufgaben, Antworten und Sterne löschen.
// Gerätestatus/Config/Fern-Log bleiben erhalten. Praktisch zum Zurücksetzen.
app.post("/api/reset", (_req, res) => {
  state.history = [];
  state.last = null;
  state.tasks = [];
  state.stars = 0;
  state.replies = [];
  if (client) client.publish(T_MESSAGE, "", { qos: 1, retain: true });  // retained Nachricht leeren
  saveState();
  res.json({ ok: true });
});

app.post("/api/cmd", (req, res) => {
  const action = (req.body?.action || "").toString();
  if (!action) return res.status(400).json({ error: "action fehlt" });
  sendCmd(action);
  res.json({ ok: true });
});

// ---- Aufgaben-API --------------------------------------------------------
app.post("/api/task", (req, res) => {
  const text = (req.body?.text || "").toString().trim();
  const from = (req.body?.from || "Wichtel").toString().trim() || "Wichtel";
  const scope = (req.body?.scope || "once").toString();
  if (!text) return res.status(400).json({ error: "text fehlt" });
  res.json({ ok: true, task: addTask(text, from, scope) });
});

// Erledigt melden (vom Gerät ODER aus der Web-App) -> Sterne + allDone
app.post("/api/task/:id/done", (req, res) => {
  const r = markTaskDone(Number(req.params.id));
  if (!r) return res.status(404).json({ error: "Aufgabe nicht gefunden" });
  res.json({ ok: true, ...r });
});

app.delete("/api/task/:id", (req, res) => {
  if (!removeTask(Number(req.params.id))) return res.status(404).json({ error: "Aufgabe nicht gefunden" });
  res.json({ ok: true });
});

// Antwort von Max (vom Gerät)
app.post("/api/reply", (req, res) => {
  const text = (req.body?.text || "").toString().trim();
  if (!text) return res.status(400).json({ error: "text fehlt" });
  pushReply(text);
  res.json({ ok: true });
});

createHttp(app).listen(HTTP_PORT, () =>
  console.log(`Web-App + API auf http://localhost:${HTTP_PORT}  (Gerät: ${DEVICE_ID})`)
);
