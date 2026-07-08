# 🎄 Wichtel Max

Ein kleiner **Wichtel‑Bote** für Max: ein ESP32‑S3 mit **E‑Paper‑Display** zeigt
Nachrichten an, die der Wichtel schickt. Papa tippt die Nachrichten in eine kleine
**Web‑App**, das Gerät holt sie per WLAN über **MQTT** ab und zeigt sie auf dem
Display. E‑Paper hält das Bild **stromlos** → mit Deep‑Sleep‑Poll hält der Akku
**Wochen** statt Stunden.

## Komponenten

```
   ┌────────────────────┐        MQTT         ┌──────────────────────────┐
   │ Gerät (ESP32-S3    │  <──────────────>  │  Backend (Node.js)        │
   │        E-Paper)     │                    │  - MQTT-Broker (aedes)    │
   │  - WLAN + Deep-Sleep│      HTTP/REST      │  - REST-API (Express)     │
   │  - LiPo-Akku        │  <──────────────>  │  - speichert letzte Msg   │
   └────────────────────┘                    └────────────┬─────────────┘
                                                          │ HTTP
                                                ┌─────────┴──────────┐
                                                │  Papa-Web-App      │
                                                │  (Browser)         │
                                                └────────────────────┘
```

| Ordner      | Inhalt                                                          |
|-------------|----------------------------------------------------------------|
| `firmware/` | ESP32‑S3 Code (PlatformIO/Arduino, GxEPD2): WLAN, MQTT, Anzeige |
| `backend/`  | Node.js: MQTT‑Broker + REST‑API, merkt sich die letzte Nachricht |
| `webapp/`   | Papa‑Web‑App zum Nachrichten‑Tippen (+ Schnell‑Sprüche)          |
| `docs/`     | MQTT‑Topics, Datenmodell, Hardware‑Notizen                       |

## Hardware

Die Firmware unterstützt **zwei Boards** (Umschaltung über `board.h` / die
gewählte PlatformIO‑Umgebung):

| Board (PlatformIO‑Env) | Display | Treiber |
|---|---|---|
| **Waveshare ESP32‑S3‑ePaper‑1.54** (`waveshare-esp32s3-154`, *Standard*) | 1,54" **200×200** S/W | SSD1681 |
| Elecrow CrowPanel ESP32‑S3 4.2" (`crowpanel-esp32s3-42`) | 4,2" **400×300** S/W | SSD1683 |

- **LiPo‑Akku** mit MX1.25‑Stecker (Waveshare‑Board hat Akku‑Header + Laderegler);
  E‑Paper hält das Bild stromlos, daher lange Laufzeit.

### Verifizierte Pins Waveshare 1,54" (aus dem offiziellen Waveshare‑Demo)
`SCK=12  MOSI=13  CS=11  DC=10  RST=9  BUSY=8` · E‑Paper‑Power `EPD_PWR=6`
(**LOW = an**) · **VBAT_PWR=17** (Power‑Latch, **HIGH = an**) · BOOT‑Taster
`GPIO0` = **Weiter ▶**, PWR‑Taster `GPIO18` = **Zurück ◀** (lang gedrückt = Aus).

## Was das Gerät kann

- **Große feste Schrift** für Leseanfänger; lange Geschichten werden auf **mehrere
  Seiten** verteilt, durch die Max mit den zwei Tasten blättert. Zu lange Wörter
  werden mit Bindestrich getrennt (nichts wird abgeschnitten).
- **Gespeicherter Verlauf:** Beim Aufwachen holt das Gerät die letzten Nachrichten
  vom Backend und speichert sie lokal (NVS). Max kann so auch **ältere Nachrichten
  wieder aufrufen** (weiter/zurück bis in den Verlauf) – auch offline. Papa sieht
  denselben Verlauf in der Web‑App und kann jede Nachricht „nochmal" senden.
- **Aufgaben:** Papa legt in der Web‑App **eine oder mehrere** Aufgaben an (z. B. „Zähne
  putzen"). Der Wichtel zeigt beim Aufwachen das **„Neue Aufgabe"-Gesicht** 😮 (bei mehreren
  „3 neue Aufgaben") + den Aufgabentext mit **Zähler „1 von 3"**; Max **drückt die Taste =
  fertig**, dann lacht der Wichtel (**„Erledigt"** 😄). Sind **alle** geschafft, gibt's das
  **„Super!"-Gesicht** 🤩 + Sterne.
  - **Zeitraum** je Aufgabe: *einmalig*, *jeden Tag*, *jede Woche* oder *jeden Monat* –
    wiederkehrende Aufgaben setzen sich am nächsten Tag/Woche/Monat automatisch zurück.
  - **Sterne-Belohnung:** jede erledigte Aufgabe gibt einen ⭐ (Zähler in der Web‑App).
  - Papa sieht in der Web‑App, was offen/erledigt ist.
- **Wichtelgesichter** je nach Situation: „wartet" (Ruhe), „neue Aufgabe", „erledigt"
  und abends/nachts das **schlafende** Gesicht 😴. Es sind **Ulis eigene Zeichnungen**,
  als 1‑Bit‑Bitmaps aufs E‑Paper gebracht (`firmware/src/wichtel_faces.h`).
- **Neue Nachricht = Briefumschlag:** Kommt Post, zeigt das Fenster einen großen
  ✉️ **Briefumschlag** („Neue Nachricht") mit **Knopf-Hinweis [▶]**, welchen Knopf Max drücken soll.
- **Antwort-Knopf (Zwei-Wege):** Max kann Papa eine kleine Rückmeldung schicken
  („Hab dich lieb!", „Fertig!", „Hunger" …) – **BOOT lang halten** öffnet die Auswahl,
  mit BOOT durchblättern, mit PWR senden. Papa sieht die Antwort in der Web-App.
- **Akkuanzeige:** kleines Batteriesymbol oben rechts am Gerät + Prozent in der Web-App.
- **Fernupdate (OTA):** Neue Firmware lässt sich **über WLAN** aufspielen, ohne das
  Gerät anzustecken (siehe unten).

### Bedienung am Gerät (Waveshare, zwei Tasten)
| Taste | kurz | lang |
|---|---|---|
| **BOOT** (GPIO0) | Weiter ▶ (nächste Seite / ältere Nachricht) · bei einer Aufgabe: **fertig ✓** | **Antwort an Papa** |
| **PWR** (GPIO18) | Zurück ◀ (vorige Seite / neuere Nachricht) · bei einer Aufgabe: **später** | **Ausschalten** |

> **Terminal-Modus:** PWR beim Aufwachen ~2&nbsp;s halten → Konsole (siehe unten).

Nach ~25&nbsp;s ohne Tastendruck schläft das Gerät wieder (das E‑Paper‑Bild bleibt
stehen). Ein Tastendruck weckt es sofort; dort, wo Max war, liest er weiter.

> ⚠️ Waveshare‑Board: **VBAT_PWR (GPIO17) muss HIGH gehalten werden** (auch im
> Tiefschlaf), sonst schaltet sich das Gerät im Akkubetrieb ab. Der EPD‑Power‑Pin
> ist **invertiert** (LOW = an). Beides ist in `firmware/src/main.cpp` bereits
> korrekt behandelt.

## Fahrplan

1. **Display + „Warte auf Nachricht"-Bildschirm** ← *Start*
2. **WLAN + MQTT** verbinden, Nachricht empfangen & anzeigen
3. **Backend** (MQTT‑Broker + REST), letzte Nachricht retained
4. **Papa‑Web‑App** (Tippen + Schnell‑Sprüche, „gesendet"-Bestätigung)
5. Feinschliff: Wichtel‑Optik (Farben, Icon), neue‑Nachricht‑Animation,
   Akku‑Anzeige, mehrere Nachrichten/Verlauf

## Schnellstart

### Backend + Web-App (auf dem PC / einem kleinen Server)
```bash
cd backend
npm install
npm start          # MQTT-Broker :1883, Web-App + API :8080
```
Web‑App im Browser: <http://localhost:8080>

### Von überall erreichbar (Cloud-Backend)
Damit Papa auch **unterwegs** Nachrichten schicken kann (nicht nur im Heim‑WLAN),
läuft das Backend in der Cloud. Das Gerät spricht es dann per **HTTPS** an – MQTT
wird dafür **nicht** gebraucht (der Online‑Status läuft über einen HTTP‑Heartbeat).

**Render (kostenlos):**
1. Projekt zu GitHub pushen, auf [render.com](https://render.com) „**New +** → **Blueprint**"
   und das Repo wählen – `render.yaml` legt den Web‑Service samt Einstellungen an.
2. Nach dem Deploy die URL notieren (z. B. `https://wichtel-max.onrender.com`).
3. In der **App/Web‑App** diese URL als Backend‑Adresse eintragen.
4. In der **Firmware** `config.h`: `BACKEND_HOST` = die Domain (ohne `https://`),
   `BACKEND_TLS 1`, `BACKEND_PORT 443` → neu flashen (oder per OTA verteilen).

**Railway / Fly.io / eigener Server:** genauso – es liegt ein `Dockerfile` +
`Procfile` bei. Wichtig sind nur zwei Umgebungsvariablen:
`ENABLE_MQTT=0` (kein offener TCP‑Port 1883 nötig) und `STATE_DIR=/data`
(auf ein **persistentes Volume**, sonst gehen Nachrichten bei jedem Neustart verloren).
`PORT` setzt der Hoster selbst. Alle Variablen sind in `backend/.env.example` erklärt.

> Free‑Instanzen schlafen bei Inaktivität ein und brauchen beim ersten Aufruf ein
> paar Sekunden zum Aufwachen – für den Wichtel unkritisch (er pollt eh nur alle 30 min).

### Firmware (VS Code + PlatformIO)
1. `firmware/` in VS Code öffnen (Erweiterung **PlatformIO IDE**).
2. `firmware/src/config.h.example` → **`config.h`** kopieren und die Backend‑Adresse eintragen:
   - **Lokal:** `BACKEND_HOST` = IP des Backend‑Rechners, `BACKEND_TLS 0`, `BACKEND_PORT 8080`.
   - **Cloud:** `BACKEND_HOST` = deine Domain (z. B. `wichtel-max.onrender.com`),
     `BACKEND_TLS 1`, `BACKEND_PORT 443`.

   (WLAN kann leer bleiben und später am Gerät eingerichtet werden – siehe unten.)
3. Board per USB‑C anstecken → **Upload** (baut standardmäßig das Waveshare‑1.54‑Board).
   Für das CrowPanel in der PlatformIO‑Leiste die Env `crowpanel-esp32s3-42` wählen.

> Hinweis: Beim Aufwecken über den **BOOT‑Taster (GPIO0)** den Knopf nur kurz
> antippen – hält man ihn beim Start gedrückt, geht der ESP32‑S3 in den
> Download‑Modus statt die Firmware zu starten.

### Lesbarkeit vorab prüfen (ohne Hardware)
`preview/display-preview-154.html` im Browser öffnen → zeigt den Wichtel-Text in
**echter Größe** (1:1, mm-genau via Bankkarten-Kalibrierung) so, wie er auf dem
1,54"-Display erscheint, inkl. Auto-Schriftgröße, Lesbarkeits-Ampel und Warnung,
wenn ein Text zu lang wird. (`display-preview.html` ist das Pendant fürs CrowPanel 4,2".)

### WLAN einrichten (ohne PC – Einrichtungs-Modus)
Das WLAN muss **nicht** fest einkompiliert werden. Findet der Wichtel beim Einschalten
**kein bekanntes WLAN** (und jemand ist per Tastendruck da), öffnet er automatisch ein
**eigenes WLAN „Wichtel-Setup"**:

1. Am Handy/Laptop mit dem WLAN **„Wichtel-Setup"** verbinden.
2. Es öffnet sich (oder unter **192.168.4.1**) eine kleine Seite.
3. Dort **dein Heim-WLAN wählen + Passwort** eintragen → *Speichern*.
4. Der Wichtel startet neu und verbindet sich. Fertig – kein Kabel, kein Neu-Flashen.

Die so gespeicherten Daten (NVS) haben **Vorrang** vor der `config.h`. Zum Zurücksetzen:
im Terminal `wifi clear` (dann kommt beim nächsten Start wieder das Portal) oder `wifi`
startet das Portal direkt. Ideal, wenn das Gerät **verschenkt** wird und der neue Haushalt
sein eigenes WLAN einträgt.

### Firmware aus der Ferne aktualisieren (OTA)
Das Gerät prüft bei jedem Aufwachen am Backend, ob eine **neuere** Firmware bereitliegt,
und flasht sich dann selbst (danach Neustart). So änderst du den Code, ohne per USB
anzustecken – es genügt, dass das Gerät dein Backend erreicht.

1. In `firmware/src/main.cpp` `#define FW_VERSION` **hochzählen** und bauen
   (`pio run -e waveshare-esp32s3-154`).
2. Neue Firmware veröffentlichen – im Ordner `backend/`:
   ```bash
   node publish-firmware.mjs        # nimmt die zuletzt gebaute .bin, erhöht die Version
   ```
   (Alternativ `firmware.bin` von Hand nach `backend/firmware/` kopieren und die
   `version` in `backend/firmware/manifest.json` erhöhen.)
3. Beim nächsten Aufwachen (oder sofort per Tastendruck) holt das Gerät das Update.
   Die Web‑App zeigt oben die laufende **Firmware‑Version** und ob ein **Update bereit** ist.

> Das allererste Mal muss die Firmware **einmal per USB** aufs Gerät (danach geht OTA).
> Der Download (~800&nbsp;KB) dauert ein paar Sekunden – solange bleibt das Gerät wach.

### Fernsteuerung (den Wichtel aus der Ferne anpassen)
Weil das Gerät hinter dem Heim-Router sitzt und die meiste Zeit schläft, ist kein
direkter Live-Zugriff („SSH") von außen möglich. Stattdessen **fragt das Gerät das
Backend ab** und wendet Änderungen beim nächsten Aufwachen an (oder sofort, wenn Max
einen Knopf drückt). In der Web-App gibt es dafür die Karte **„🛰️ Fernsteuerung":**

- **Einstellungen** (werden aufs Gerät übernommen und dort dauerhaft gespeichert):
  Poll-Intervall, Nachtruhe-Zeiten, Lautstärke.
- **Befehl an das Gerät:** dieselben Konsolen-Befehle wie im Terminal (`status`,
  `beep`, `mood`, `i2c …`, `ota`, `reboot` …). Die Antwort erscheint im **Geräte-Log**.
- Zusammen mit **OTA** (Firmware-Update aus der Ferne) lässt sich so praktisch alles
  aus der Ferne anpassen – ganz ohne das Gerät anzufassen.

Technisch: `POST /api/cmd-queue` / `POST /api/config` (Papa) → das Gerät holt beim
Aufwachen `GET /api/pull` und meldet die Ausgabe per `POST /api/log` zurück.

### Terminal-Zugang (Konsole über WLAN/USB)
Das Gerät hat eine kleine **Konsole** zum Debuggen und Steuern – erreichbar per
**USB-Seriell** und über **Telnet** im WLAN.

- **Aktivieren:** beim Aufwachen die **PWR-Taste ~2&nbsp;Sekunden gedrückt halten**
  (CrowPanel: die MENU-Taste). Das Display zeigt „Terminal" + die IP-Adresse.
- **Verbinden:** `telnet <IP>` (Port 23) oder der serielle Monitor (115200 Baud).
- **Befehle:** `help`, `status`, `ip`, `fetch`, `ota`, `mood <0-5>`, `msg`,
  `beep [n]`, `sleep`, `reboot` und **`i2c scan | i2c r <addr> <reg> | i2c w <addr> <reg> <val>`**.
- Nach 5&nbsp;Minuten ohne Eingabe (oder `sleep`/`exit`) geht das Gerät wieder schlafen.

Der `i2c`-Befehl ist u.a. praktisch, um den **ES8311-Audiocodec** (Adresse `0x18`)
am Gerät in Betrieb zu nehmen/zu testen.

### Ton bei neuer Nachricht/Aufgabe
Bei neuer Post/Aufgabe kann das Gerät **piepen** (2× für Nachricht, 3× für Aufgabe).
Der Code ist verdrahtet, standardmäßig aber **aus**:

- **Einfach (empfohlen):** einen **passiven Piezo‑Buzzer** an einen freien GPIO + GND
  hängen und in `config.h` `#define BUZZER_PIN 21` (o. ä.) setzen – fertig.
- Das Board hat zwar einen **ES8311‑Audio‑Codec (I2S)** mit Lautsprecher‑Header (MX1.25),
  aber der braucht einen angeschlossenen Lautsprecher **und** die aufwändigere I2S/ES8311‑
  Anbindung; der Piezo‑Weg ist für ein simples Signal deutlich einfacher.

Ohne Buzzer (`BUZZER_PIN -1`) bleiben die **visuellen** Signale (✉️ Briefumschlag, Gesichter).

### Stromsparen (in `firmware/src/main.cpp` einstellbar)
- **`POLL_MINUTES` = 30** – Intervall, in dem nach neuer Post gesehen wird.
  Höher = längere Akkulaufzeit (der BOOT‑Taster holt jederzeit sofort neue Post).
- **Nachtruhe `NIGHT_START_HOUR`=22 / `NIGHT_END_HOUR`=6** – in diesem Fenster
  wird per Timer **nicht** gepollt (kein WLAN), das Gerät schläft bis zum Morgen
  durch. Ein Tastendruck weckt trotzdem. Die Uhrzeit kommt per **NTP** bei jeder
  WLAN‑Verbindung und übersteht den Tiefschlaf (kein extra RTC nötig; falls das
  Heim‑WLAN kein Internet hat, entfällt die Nachtruhe einfach).
