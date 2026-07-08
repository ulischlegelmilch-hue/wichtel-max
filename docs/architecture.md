# Architektur & Schnittstellen – Wichtel Max

## MQTT-Topics
Basis-Präfix pro Gerät: `wichtel/<deviceId>/…` (Default `deviceId = max`).

| Topic                       | Richtung        | Inhalt (JSON)                                  |
|-----------------------------|-----------------|------------------------------------------------|
| `wichtel/<id>/message`      | Backend → Gerät | `{ text, from?, ts }` Nachricht aufs Display   |
| `wichtel/<id>/cmd`          | Backend → Gerät | `{ action: "blink" \| "snow" \| "clear" }`     |
| `wichtel/<id>/status`       | Gerät → Backend | `{ online, rssi, battery?, ts }` Lebenszeichen |

- `message` wird **retained** veröffentlicht → nach Reboot kennt das Gerät die
  letzte Nachricht sofort wieder.
- QoS 1 für `message` (keine verlorene Wichtel-Post).
- `status` als Last-Will: Broker meldet `{ online:false }`, wenn das Gerät weg ist.

## REST-API (Backend ↔ Web-App)
```
GET  /api/state              -> { last: { text, from, ts } | null, deviceOnline }
POST /api/message  { text, from? }   -> speichert + MQTT message (retained)
POST /api/cmd      { action }        -> MQTT cmd
```
(Optional später: WebSocket /ws für Live-„Gerät online"-Anzeige.)

## Datenmodell (Backend, einfach in JSON-Datei)
```
state.json:
{
  "last":   { "text": "...", "from": "Wichtel", "ts": 0 },
  "history":[ { "text": "...", "from": "...", "ts": 0 }, ... ]   // optional, Verlauf
}
```
Kein DB nötig für den Start – eine kleine JSON-Datei reicht. Später bei Bedarf SQLite.

## Anzeige-Logik (Gerät) – E-Paper, batteriesparend
- Gerät **schläft tief** (E-Paper hält das Bild stromlos). Es wacht **alle
  `POLL_MINUTES` (Default 15)** ODER per **MENU-Taster** auf.
- Bei jedem Aufwachen: WLAN + MQTT verbinden, **retained `message`** holen,
  Hash mit der zuletzt gezeigten vergleichen (in `RTC_DATA_ATTR` über Schlaf gemerkt).
  **Nur bei NEUER Nachricht** wird das Display neu geschrieben (spart Refresh + Akku).
- Anzeige: Text **wortweise umgebrochen**, mittig, **Auto-Schriftgröße** (größte
  passende von FreeSansBold 24/18/12pt), echte Umlaute (ä ö ü); Absender klein darunter.
- Leere `message` (= „clear") → Idle-Bildschirm „Wichtel Max / warte auf Post …".
- Kompromiss: Nachricht erscheint erst beim nächsten Aufwachen (max. ~15 min, oder
  sofort per Taster). Kein Hintergrundlicht → im Dunkeln nicht lesbar.

## Hardware-Pins (CrowPanel ESP32-S3 4.2" E-Paper, SSD1683, 400×300)
In `firmware/src/main.cpp` als `#define`. Verifiziert:
- **EPD_PWR = GPIO7** → HIGH versorgt das Panel, LOW = stromlos (Bild bleibt!).
- SPI: **SCK 12, MOSI 11, CS 45, DC 46, RST 47, BUSY 48**.
- **MENU-Taster = GPIO2** (RTC-fähig, weckt aus Tiefschlaf via ext0).
- GxEPD2-Klasse `GxEPD2_420_GYE042A87`, `framework arduino`, lib `zinggjm/GxEPD2`.

## Verworfen: T-Display-S3 (Farb-TFT)
Ursprünglich geplant (ST7789 170×320, TFT_eSPI). Wegen Akkulaufzeit (~8–10 h
Dauerbetrieb vs. Wochen beim E-Paper) und kleiner Schrift auf E-Paper umgestellt.
Die TFT-Firmware liegt in der Git-History; Backend/MQTT/Web-App blieben unverändert.
