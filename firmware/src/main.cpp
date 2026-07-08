// Wichtel Max – Firmware (interaktiver Reader, board-umschaltbar; siehe board.h)
//   Standard-Board: Waveshare ESP32-S3-ePaper-1.54 (200x200, SSD1681)
//   Alternativ:     Elecrow CrowPanel ESP32-S3 4.2"  (400x300, SSD1683)
//
// Konzept: Das Gerät schläft die meiste Zeit tief (E-Paper hält das Bild ohne
// Strom). Beim Aufwachen holt es die neueste Nachricht + den Verlauf vom Backend
// (HTTP /api/state), speichert ihn lokal (NVS -> auch offline blätterbar) und
// zeigt ihn in GROSSER fester Schrift mit Seiten zum Blättern.
//
// Bedienung (Waveshare, zwei Tasten):
//   BOOT (GPIO0) = Weiter ▶ : nächste Seite / nächste (ältere) Nachricht
//   PWR  (GPIO18)= Zurück ◀ : vorige Seite / neuere Nachricht;  LANG halten = AUS
// Nach kurzer Inaktivität legt sich das Gerät wieder schlafen (Bild bleibt stehen).
//
// Der „Warte auf Post"-Ruhebildschirm zeigt ein kleines Wichtelgesicht.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>  // HTTPS zum Cloud-Backend (TLS)
#include <HTTPClient.h>
#include <HTTPUpdate.h>   // OTA: Firmware per WLAN vom Backend nachladen
#include <WebServer.h>    // WLAN-Einrichtung: eigener Webserver im AP-Modus
#include <DNSServer.h>    // WLAN-Einrichtung: Captive Portal
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <time.h>
#include "board.h"
#include "config.h"
#include "wichtel_faces.h"   // 6 Wichtelgesichter (1-Bit-Bitmaps aus der Vorlage)
#include "audio.h"           // Töne über den Onboard-ES8311-Lautsprecher

// ---- Firmware-Version (für OTA-Fernupdate) -------------------------------
// Bei jeder neuen Firmware, die du übers Backend verteilen willst, HOCHZÄHLEN.
// Das Gerät lädt sich nur eine .bin, deren Version größer als diese ist.
#define FW_VERSION      1

// ---- Verhalten -----------------------------------------------------------
#define POLL_MINUTES    30    // wie oft aufwachen & nach neuer Nachricht sehen
#define WIFI_TIMEOUT    8000  // ms pro WLAN-Versuch warten
#define WIFI_TRIES      2     // Anzahl Verbindungsversuche, bevor aufgegeben wird
#define BOOT_DIAG       0     // Boot-Diagnose (Tasten + I2C/Touch-Scan) ueber Serial; 1=an zum Debuggen
#define HTTP_TIMEOUT    5000  // ms auf die Backend-Antwort warten
#define IDLE_SLEEP_MS   180000 // ms ohne Tastendruck -> automatisch schlafen (3 Minuten)
#define LONGPRESS_MS    1500  // ms PWR halten = weglegen/schlafen (jeder Knopf weckt wieder)
#define ARCHIVE_MAX     15    // so viele Nachrichten am Gerät durchblätterbar
#define STORE_TEXT_MAX  800   // pro Nachricht max. Zeichen im Gerätespeicher
#define TASK_MAX        10    // so viele offene Aufgaben gleichzeitig
#define DONE_SHOW_MS    2500  // wie lange das "Erledigt"-Gesicht stehen bleibt

// ---- Nachtruhe -----------------------------------------------------------
#define NIGHT_START_HOUR 22
#define NIGHT_END_HOUR    6
#define NTP_SYNC_MS    3000
#define TZ_GERMANY "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Schrift -------------------------------------------------------------
// GROSSE feste Leseschrift (für Leseanfänger). Umschaltbar: für noch größere
// Schrift &FreeSansBold24pt7b setzen (dann mehr Seiten pro Geschichte).
#define READER_FONT (&FreeSansBold18pt7b)
#define FOOTER_FONT (&FreeSans9pt7b)
#define FOOTER_H     20    // reservierte Höhe unten (Absender / Seitenzähler)
#define SIDE_MARGIN  26    // seitl. Rand fürs Umbrechen (zentrierter Text bleibt links+rechts von den Knopf-Labels)
#define LABEL_FONT (&FreeSansBold9pt7b)  // Schrift der Knopf-Labels
// Beide Knöpfe sitzen RECHTS in der unteren Displayhälfte, übereinander. Die zwei
// Wörter stehen SENKRECHT am rechten Rand: unteres Wort am unteren Bildschirmrand,
// oberes direkt darüber. Falls oben/unten zum jeweiligen Knopf vertauscht: auf 1.
#define SWAP_BTN_LABELS 0
// Leserichtung der senkrechten Schrift. 3 = von unten nach oben (Köpfe links).
// Falls sie auf dem echten Display kopfüber/falsch steht: auf 1 setzen.
#define VLABEL_ROT 3

// ---- Umlaut-Darstellung --------------------------------------------------
#define UMLAUTS_REAL     1
#define UMLAUT_DOT_RAISE 0.95
// --------------------------------------------------------------------------

#if defined(BOARD_WAVESHARE_154)
  GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>
    display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#elif defined(BOARD_CROWPANEL_42)
  GxEPD2_BW<GxEPD2_420_GYE042A87, GxEPD2_420_GYE042A87::HEIGHT>
    display(GxEPD2_420_GYE042A87(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#endif

WiFiClient net;
PubSubClient mqtt(net);
Preferences prefs;

String topicStatus = String("wichtel/") + DEVICE_ID + "/status";

// ---- Backend-Adresse (lokal HTTP oder Cloud HTTPS) ----------------------
// Ältere config.h kennen nur MQTT_HOST -> als Fallback für den HTTP-Host nutzen.
#ifndef BACKEND_HOST
  #define BACKEND_HOST MQTT_HOST
#endif
#ifndef BACKEND_TLS
  #define BACKEND_TLS 0            // 0 = http (lokal), 1 = https (Cloud)
#endif
// Online-Status läuft jetzt über HTTP-Heartbeat statt MQTT. MQTT nur einschalten,
// wenn ein alter MQTT-Broker bedient werden soll (dann auch Backend ENABLE_MQTT=1).
#ifndef USE_MQTT_STATUS
  #define USE_MQTT_STATUS 0
#endif

#if BACKEND_TLS
  typedef WiFiClientSecure ApiClient;
  // Ohne Zertifikatsprüfung (setInsecure) – für ein Bastelprojekt ausreichend
  // und spart das Pinnen/Aktualisieren von Root-CAs im Flash.
  static inline void apiClientPrep(ApiClient &c) { c.setInsecure(); }
#else
  typedef WiFiClient ApiClient;
  static inline void apiClientPrep(ApiClient &c) { (void)c; }
#endif

// Basis-URL des Backends, z. B. "http://192.168.0.5:8080" oder
// "https://wichtel-max.onrender.com". Standard-Ports (80/443) werden weggelassen.
String apiBase() {
  String s = BACKEND_TLS ? F("https://") : F("http://");
  s += BACKEND_HOST;
  int p = BACKEND_PORT;
  bool stdPort = (BACKEND_TLS && p == 443) || (!BACKEND_TLS && p == 80);
  if (!stdPort) { s += ":"; s += String(p); }
  return s;
}

// ---- Nachrichten-Archiv (neueste zuerst) ---------------------------------
struct Msg { String text; String from; uint32_t ts; };
Msg  archive[ARCHIVE_MAX];
int  archiveN = 0;

// ---- Offene Aufgaben -----------------------------------------------------
// scope: 0=einmalig, 1=heute, 2=diese Woche, 3=diesen Monat
struct Task { int id; String text; String from; int scope; };
Task tasks[TASK_MAX];
int  tasksN = 0;

// Über Tiefschlaf gemerkt
RTC_DATA_ATTR uint32_t lastShownHash = 0;   // Hash der zuletzt gezeigten neuesten Nachricht
RTC_DATA_ATTR bool     timeValid     = false;
RTC_DATA_ATTR int      navMsg        = 0;    // Blätter-Position: Nachricht
RTC_DATA_ATTR int      navPage       = 0;    // Blätter-Position: Seite
RTC_DATA_ATTR uint32_t navSig        = 0;    // Signatur des Archivs (zum Fortsetzen)
RTC_DATA_ATTR uint32_t lastTaskSig   = 0;    // Signatur der offenen Aufgaben (neue erkennen)
RTC_DATA_ATTR uint32_t lastReadTs    = 0;    // ts der zuletzt gelesenen Nachricht (alles <= gilt als gelesen)

// Fern-Konfiguration (per Backend änderbar, in NVS gesichert, über Schlaf gemerkt)
RTC_DATA_ATTR int  cfgPollMin    = POLL_MINUTES;
RTC_DATA_ATTR int  cfgNightStart = NIGHT_START_HOUR;
RTC_DATA_ATTR int  cfgNightEnd   = NIGHT_END_HOUR;
RTC_DATA_ATTR int  cfgVolume     = 80;
RTC_DATA_ATTR bool cfgLoaded     = false;

// Signatur der offenen Aufgaben – ändert sich, sobald eine Aufgabe dazukommt/wegfällt.
uint32_t taskSig() {
  uint32_t s = (uint32_t)tasksN;
  for (int i = 0; i < tasksN; i++) s = s * 131u + (uint32_t)tasks[i].id;
  return s;
}

// aktuelle Blätter-Position
int curMsg = 0, curPage = 0;

// ---- Textaufbereitung (UTF-8 -> ASCII-Basis + Umlaut-Flags) --------------
static String gBase;
static bool   gUm[1024];

void buildBase(const String &t) {
  gBase = ""; memset(gUm, 0, sizeof(gUm));
  int len = t.length();
  for (int i = 0; i < len && gBase.length() < 1000; ) {
    uint8_t c = (uint8_t)t[i];
    if (c < 0x80) { gBase += (char)c; i += 1; }
    else if (c == 0xC3 && i + 1 < len) {
      uint8_t d = (uint8_t)t[i + 1]; i += 2;
      char b = '?'; bool um = false, ss = false;
      switch (d) {
        case 0xA4: b = 'a'; um = true; break;  // ä
        case 0xB6: b = 'o'; um = true; break;  // ö
        case 0xBC: b = 'u'; um = true; break;  // ü
        case 0x84: b = 'A'; um = true; break;  // Ä
        case 0x96: b = 'O'; um = true; break;  // Ö
        case 0x9C: b = 'U'; um = true; break;  // Ü
        case 0x9F: ss = true; break;           // ß
        default:   b = '?'; break;
      }
#if UMLAUTS_REAL
      if (ss) { gBase += 's'; gBase += 's'; }
      else { if (um) gUm[gBase.length()] = true; gBase += b; }
#else
      if (ss) { gBase += "ss"; }
      else if (um) { gBase += b; gBase += (b < 'a' ? 'E' : 'e'); }
      else { gBase += b; }
#endif
    } else {  // sonstiges Mehrbyte (Emoji o.ä.) -> Leerzeichen
      i += 1; while (i < len && ((uint8_t)t[i] & 0xC0) == 0x80) i++;
      gBase += ' ';
    }
  }
}

uint32_t hashMsg(const String &text, const String &from) {
  uint32_t h = 2166136261u;                  // FNV-1a
  String s = text + "\n" + from;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

// ---- Layout-Helfer -------------------------------------------------------
struct Range { int s, e; };
static Range lines[80]; static bool hyph[80]; static int nLines = 0;

int marginPx() { return display.height() >= 300 ? 8 : 6; }

int textW(const String &s) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void fontAscDesc(const GFXfont *f, int &asc, int &desc) {
  display.setFont(f);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds("Mg", 0, 100, &x1, &y1, &w, &h);
  asc = 100 - y1; desc = (y1 + h) - 100;
}

// Bricht gBase mit Font f in Zeilen um (gefüllt bis nLines). Bevorzugt Umbruch an
// Leerzeichen; passt ein einzelnes Wort nicht in die Zeile, wird es hart getrennt
// (hyph[]=true -> beim Zeichnen kommt ein Bindestrich ans Zeilenende).
void wrapWith(const GFXfont *f, int maxW) {
  display.setFont(f);
  nLines = 0;
  int L = gBase.length(), i = 0;
  while (i < L && nLines < 80) {
    while (i < L && gBase[i] == ' ') i++;             // führende Leerzeichen weg
    if (i >= L) break;
    int start = i, j = i, lastSpace = -1;
    while (j < L && textW(gBase.substring(start, j + 1)) <= maxW) {
      if (gBase[j] == ' ') lastSpace = j;
      j++;
    }
    int end; bool hy = false;
    if (j >= L)                 end = L;               // Rest passt komplett
    else if (lastSpace >= start) end = lastSpace;      // an letzter Wortgrenze
    else { end = (j > start) ? j : start + 1; hy = true; } // langes Wort hart trennen
    int e2 = end; while (e2 > start && gBase[e2 - 1] == ' ') e2--;
    lines[nLines] = { start, e2 };
    hyph[nLines] = hy;
    nLines++;
    i = end;
  }
}

void drawUmlautDots(int bx, int ax, int baselineY, int asc) {
  int cw = ax - bx;
  int r  = max(2, asc / 12);
  int dy = baselineY - (int)(asc * UMLAUT_DOT_RAISE);
  display.fillCircle(bx + cw * 0.30, dy, r, GxEPD_BLACK);
  display.fillCircle(bx + cw * 0.70, dy, r, GxEPD_BLACK);
}

// Zeichnet eine Zeile aus gBase zentriert ab Grundlinie baselineY.
void drawBaseLine(const Range &r, const GFXfont *f, int baselineY, int asc, bool hy) {
  display.setFont(f);
  display.setTextColor(GxEPD_BLACK);
  int lw = textW(gBase.substring(r.s, r.e)) + (hy ? textW("-") : 0);
  int x  = (display.width() - lw) / 2;
  display.setCursor(x, baselineY);
  for (int k = r.s; k < r.e; k++) {
    int bx = display.getCursorX();
    display.print(gBase[k]);
    int ax = display.getCursorX();
    if (gUm[k]) drawUmlautDots(bx, ax, baselineY, asc);
  }
  if (hy) display.print('-');           // Trennstrich bei hart getrenntem Wort
}

// Zeilen pro Seite (feste Leseschrift).
int gAsc, gDesc, gLineH, gLinesPerPage;
void computeReaderMetrics() {
  fontAscDesc(READER_FONT, gAsc, gDesc);
  gLineH = READER_FONT->yAdvance;
  int M = marginPx();
  int firstBase = M + gAsc;
  int lastBaseMax = display.height() - FOOTER_H - gDesc;
  gLinesPerPage = (lastBaseMax - firstBase) / gLineH + 1;
  if (gLinesPerPage < 1) gLinesPerPage = 1;
}

// Seitenzahl einer Nachricht (baut/bricht sie dafür um).
int pageCountOf(int idx) {
  if (idx < 0 || idx >= archiveN) return 1;
  buildBase(archive[idx].text);
  wrapWith(READER_FONT, display.width() - 2 * SIDE_MARGIN);
  int pc = (nLines + gLinesPerPage - 1) / gLinesPerPage;
  return pc < 1 ? 1 : pc;
}

// ---- Wichtelgesichter ----------------------------------------------------
// Die sechs Stimmungen stammen 1:1 aus Ulis Vorlage und liegen als 1-Bit-
// Bitmaps in wichtel_faces.h (Feld FACE_BMP[mood], 150×150). Gezeichnet werden
// sie zentriert in showMood(). Die Enum-Reihenfolge MUSS zu FACE_BMP passen.
enum Mood { MOOD_WAIT, MOOD_NEWTASK, MOOD_DONE, MOOD_SUPER, MOOD_SAD, MOOD_SLEEP };

// Zeichnet Text zentriert mit der größten Schrift, die in die Breite passt.
void drawCenteredFit(const String &s, int baselineY, const GFXfont **fonts, int nFonts, int maxW) {
  const GFXfont *use = fonts[nFonts - 1];
  for (int i = 0; i < nFonts; i++) { display.setFont(fonts[i]); if (textW(s) <= maxW) { use = fonts[i]; break; } }
  display.setFont(use); display.setTextColor(GxEPD_BLACK);
  display.setCursor((display.width() - textW(s)) / 2, baselineY);
  display.print(s);
}

const GFXfont *TITLE_FONTS[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };

bool isNightNow();   // (weiter unten definiert – für den Schlaf-Ruhebildschirm)

// ---- Batterie ------------------------------------------------------------
#if HAS_BATTERY
int batteryPercent() {
  analogReadResolution(12);
  long sum = 0; const int N = 8;
  for (int i = 0; i < N; i++) { sum += analogReadMilliVolts(BATT_ADC_PIN); delay(2); }
  float v = (sum / (float)N) * BATT_DIVIDER / 1000.0f;      // Batteriespannung in V
  int p = (int)((v - 3.30f) / (4.20f - 3.30f) * 100.0f);   // grobe LiPo-Kennlinie
  return p < 0 ? 0 : p > 100 ? 100 : p;
}
// kleines Batteriesymbol (Rahmen + Füllung + Pluspol)
void drawBatteryIcon(int x, int y, int pct) {
  const int w = 22, h = 11;
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.fillRect(x + w, y + 3, 2, h - 6, GxEPD_BLACK);
  int fill = (w - 4) * pct / 100;
  if (fill > 0) display.fillRect(x + 2, y + 2, fill, h - 4, GxEPD_BLACK);
}
#else
int batteryPercent() { return -1; }
#endif

// Ein SENKRECHTES Wort am rechten Rand (unteres Ende bei yBottom). Weißer Streifen
// dahinter, damit es über jedem Inhalt lesbar bleibt. Nutzt kurz die Display-Drehung.
void drawVLabel(const char *word, int yBottom) {
  if (!word || !word[0]) return;
  const int W = display.width(), H = display.height();
  display.setFont(LABEL_FONT);
  int len = textW(word);
  int yTop = yBottom - len;
  // weißer Hintergrundstreifen + dünne Trennlinie (in normaler Orientierung)
  display.fillRect(W - 20, yTop - 2, 20, len + 4, GxEPD_WHITE);
  display.drawFastVLine(W - 20, yTop - 2, len + 4, GxEPD_BLACK);
  // gedreht schreiben
  display.setTextColor(GxEPD_BLACK);
  display.setRotation((EPD_ROTATION + VLABEL_ROT) % 4);
#if VLABEL_ROT == 1
  display.setCursor(yTop, 3);                    // von oben nach unten, Köpfe rechts
#else
  display.setCursor((H - 1) - yBottom, W - 4);   // von unten nach oben, Köpfe links (Standard)
#endif
  display.print(word);
  display.setRotation(EPD_ROTATION);             // Drehung zurücksetzen
}

// Zwei senkrechte Knopf-Labels rechts: unteres am unteren Rand, oberes darüber.
// upper = oberer Knopf, lower = unterer Knopf. Immer ZULETZT zeichnen.
void drawButtonLabels(const char *upper, const char *lower) {
  const char *u = upper, *l = lower;
#if SWAP_BTN_LABELS
  u = lower; l = upper;
#endif
  const int H = display.height();
  display.setFont(LABEL_FONT);
  int lowLen = (l && l[0]) ? textW(l) : 0;
  int ybLow = H - 3;                 // unteres Wort ganz unten
  drawVLabel(l, ybLow);
  drawVLabel(u, ybLow - lowLen - 12); // oberes Wort direkt darüber (12 px Abstand)
}

// Vollbild: ein Wichtelgesicht (Bitmap) mittig oben + Titel + kleiner Untertitel.
// Optional bl/br = Knopf-Labels unten; dann kompakteres Layout (Gesicht höher,
// kein Untertitel), damit die Leiste nicht überlappt.
void showMood(Mood mood, const char *title, const char *sub,
              const char *bl = nullptr, const char *br = nullptr) {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  int fx = (W - FACE_W) / 2;
  int fy = (H >= 300) ? 20 : 2;
  int titleY = fy + FACE_H + ((H >= 300) ? 44 : 22);
  int bpct = batteryPercent();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
#if HAS_BATTERY
    if (bpct >= 0) drawBatteryIcon(W - 28, 4, bpct);
#endif
    display.drawBitmap(fx, fy, FACE_BMP[mood], FACE_W, FACE_H, GxEPD_BLACK);
    drawCenteredFit(title, titleY, TITLE_FONTS, 3, maxW);
    if (sub && sub[0]) {
      const GFXfont *s[] = { &FreeSans9pt7b };
      drawCenteredFit(sub, titleY + 20, s, 1, maxW);
    }
    drawButtonLabels(bl, br);    // Knopf-Labels rechts (nur wenn Wörter übergeben)
  } while (display.nextPage());
}

// Ruhebildschirm: tagsüber „wartet", abends/nachts das schlafende Gesicht.
void renderIdle() {
  // Feste UI-Labels bewusst ohne Umlaute (werden roh gedruckt, nicht über
  // die Umlaut-Aufbereitung von buildBase).
  // Knopf-Labels: oberer Knopf = Antwort an Papa, unterer = schlafen legen.
  if (timeValid && isNightNow())
    showMood(MOOD_SLEEP, "Gute Nacht", "schlaf gut, Max", "ANTWORT", "RUHE");
  else
    showMood(MOOD_WAIT, "Wichtel Max", "warte auf Post ...", "ANTWORT", "RUHE");
}

// Briefumschlag-Symbol (Signal für neue Nachricht).
void drawEnvelope(int cx, int cy, int w, int h) {
  int x0 = cx - w / 2, y0 = cy - h / 2;
  display.drawRect(x0, y0, w, h, GxEPD_BLACK);              // Körper (2 px)
  display.drawRect(x0 + 1, y0 + 1, w - 2, h - 2, GxEPD_BLACK);
  display.drawLine(x0, y0, cx, cy, GxEPD_BLACK);            // Klappe (V zur Mitte)
  display.drawLine(x0, y0 + 1, cx, cy + 1, GxEPD_BLACK);
  display.drawLine(x0 + w - 1, y0, cx, cy, GxEPD_BLACK);
  display.drawLine(x0 + w - 1, y0 + 1, cx, cy + 1, GxEPD_BLACK);
}

// Knopf-Anleitung für Max: ein "Knopf" (gerundetes Rechteck) mit ▶-Pfeil +
// Wort, damit klar ist, welchen Knopf er zum Öffnen drücken soll. Der ▶-Pfeil
// steht für den WEITER-Knopf (BOOT), der überall die Vorwärts-/Öffnen-Taste ist.
void drawOpenHint(int cx, int topY, const char *label) {
  int bw = 118, bh = 34, x = cx - bw / 2, y = topY;
  display.drawRoundRect(x, y, bw, bh, 8, GxEPD_BLACK);
  display.drawRoundRect(x + 1, y + 1, bw - 2, bh - 2, 7, GxEPD_BLACK);   // 2 px dick
  int ty = y + bh / 2;
  display.fillTriangle(x + 15, ty - 9, x + 15, ty + 9, x + 31, ty, GxEPD_BLACK);  // ▶
  display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_BLACK);
  int16_t bx, by; uint16_t tw, th; display.getTextBounds(label, 0, 0, &bx, &by, &tw, &th);
  display.setCursor(x + 42, ty + th / 2);
  display.print(label);
}

// Signalisiert eine neue Nachricht: Briefumschlag + Knopf-Anleitung.
void renderMessageAlert() {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  int ew = (H >= 300) ? 170 : 122, eh = (H >= 300) ? 110 : 76;
  int cx = W / 2, cy = (H >= 300) ? 90 : 54;
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawEnvelope(cx, cy, ew, eh);
    int ty = cy + eh / 2 + ((H >= 300) ? 40 : 28);
    drawCenteredFit("Neue Nachricht", ty, TITLE_FONTS, 3, maxW);
    drawOpenHint(cx, ty + ((H >= 300) ? 22 : 12), "lesen");
  } while (display.nextPage());
}

// Signalisiert eine neue Aufgabe: (kleines) Wichtelgesicht + Knopf-Anleitung.
void renderTaskAlert(int count) {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  int fx = (W - FACE_SM_W) / 2, fy = (H >= 300) ? 16 : 2;
  String title = (count > 1) ? (String(count) + " neue Aufgaben") : String("Neue Aufgabe");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(fx, fy, FACE_SM_BMP[MOOD_NEWTASK], FACE_SM_W, FACE_SM_H, GxEPD_BLACK);
    int ty = fy + FACE_SM_H + ((H >= 300) ? 34 : 22);
    drawCenteredFit(title, ty, TITLE_FONTS, 3, maxW);
    drawOpenHint(W / 2, ty + ((H >= 300) ? 22 : 12), "oeffnen");
  } while (display.nextPage());
}

// Zeichnet die aktuelle Seite (curMsg/curPage).
void renderCurrent() {
  const int W = display.width(), H = display.height();
  const int M = marginPx();
  computeReaderMetrics();
  buildBase(archive[curMsg].text);
  wrapWith(READER_FONT, W - 2 * SIDE_MARGIN);
  int pc = (nLines + gLinesPerPage - 1) / gLinesPerPage; if (pc < 1) pc = 1;
  if (curPage >= pc) curPage = pc - 1;
  if (curPage < 0)   curPage = 0;
  int start = curPage * gLinesPerPage;
  int end   = min(nLines, start + gLinesPerPage);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    int y = M + gAsc;
    for (int i = start; i < end; i++) { drawBaseLine(lines[i], READER_FONT, y, gAsc, hyph[i]); y += gLineH; }
    // Absender mittig unten
    display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_BLACK);
    String fr = "von Lumbi";
    display.setCursor((W - textW(fr)) / 2, H - 5);
    display.print(fr);
    // Seitenzähler oben rechts (nur wenn mehrseitig)
    if (pc > 1) {
      String pg = String(curPage + 1) + "/" + String(pc);
      display.setFont(FOOTER_FONT);
      display.setCursor(W - M - textW(pg), M + 12);
      display.print(pg);
    }
    // kleiner Archiv-Hinweis oben links (welche gespeicherte Nachricht)
    if (archiveN > 1) {
      String am = (curMsg == 0) ? String("neu") : String(curMsg + 1) + ".";
      display.setFont(FOOTER_FONT);
      display.setCursor(M, M + 12);
      display.print(am);
    }
    // Knopf-Labels: oberer = weiter blättern, unterer = löschen
    drawButtonLabels("WEITER", "LOESCHEN");
  } while (display.nextPage());
}

// ---- Blättern ------------------------------------------------------------
void navNext() {                       // Weiter ▶
  int pc = pageCountOf(curMsg);
  if (curPage < pc - 1) curPage++;
  else if (curMsg < archiveN - 1) { curMsg++; curPage = 0; }
  // sonst: am Ende angekommen -> bleibt stehen
}
void navPrev() {                       // Zurück ◀
  if (curPage > 0) curPage--;
  else if (curMsg > 0) { curMsg--; curPage = pageCountOf(curMsg) - 1; }
  // sonst: bei der neuesten Nachricht -> bleibt stehen
}
void navNewest() { curMsg = 0; curPage = 0; }

// ---- E-Paper an/aus ------------------------------------------------------
void epdPowerOn()  { pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, EPD_PWR_ON_LEVEL); }
void epdPowerOff() { pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, EPD_PWR_OFF_LEVEL); }
void epdBegin() {
  epdPowerOn();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(EPD_ROTATION);
}
void epdEnd() { display.hibernate(); epdPowerOff(); }

// ---- Archiv speichern/laden (NVS) ---------------------------------------
// Jede Nachricht als eigener NVS-Eintrag ("m0".."mN"), damit keine einzelne
// Zeichenkette das NVS-Limit (~4 KB) überschreitet.
void saveArchiveNVS() {
  prefs.begin("wichtel", false);
  prefs.putInt("n", archiveN);
  for (int i = 0; i < archiveN; i++) {
    JsonDocument o;
    o["t"] = archive[i].text.substring(0, STORE_TEXT_MAX);
    o["f"] = archive[i].from;
    o["s"] = archive[i].ts;
    String s; serializeJson(o, s);
    prefs.putString((String("m") + i).c_str(), s);
  }
  prefs.end();
}

// "Gelesen"-Merker: alles mit ts <= lastReadTs gilt als gelesen.
void loadLastRead() { prefs.begin("wichtel", true);  lastReadTs = prefs.getUInt("lastread", lastReadTs); prefs.end(); }
void saveLastRead() { prefs.begin("wichtel", false); prefs.putUInt("lastread", lastReadTs); prefs.end(); }
int  unreadCount()  { int n = 0; for (int i = 0; i < archiveN; i++) if (archive[i].ts > lastReadTs) n++; return n; }
// Fern-Konfiguration laden/speichern (NVS)
void loadConfigNVS() {
  prefs.begin("wcfg", true);
  cfgPollMin    = prefs.getInt("pollMin", POLL_MINUTES);
  cfgNightStart = prefs.getInt("nStart",  NIGHT_START_HOUR);
  cfgNightEnd   = prefs.getInt("nEnd",    NIGHT_END_HOUR);
  cfgVolume     = prefs.getInt("vol",     80);
  prefs.end();
  cfgLoaded = true;
}
void saveConfigNVS() {
  prefs.begin("wcfg", false);
  prefs.putInt("pollMin", cfgPollMin);
  prefs.putInt("nStart",  cfgNightStart);
  prefs.putInt("nEnd",    cfgNightEnd);
  prefs.putInt("vol",     cfgVolume);
  prefs.end();
}

// ---- WLAN verbinden: gespeicherte Zugangsdaten (NVS) haben Vorrang vor der
//      fest einkompilierten config.h. -------------------------------------
// Sind WLAN-Zugangsdaten gespeichert? (Steuert, ob bei Verbindungsfehler das
// Einrichtungs-Portal starten darf oder nicht.)
bool hasStoredWiFi() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  prefs.end();
  return s.length() > 0;
}

bool connectWiFi() {
  String ssid, pass;
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  const char *useSsid = ssid.length() ? ssid.c_str() : WIFI_SSID;   // sonst config.h-Fallback
  const char *usePass = ssid.length() ? pass.c_str() : WIFI_PASS;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // stabilere/schnellere Verbindung
  // Mehrere Anläufe: ein einzelner flakiger Versuch soll nicht gleich alles kippen.
  for (int attempt = 0; attempt < WIFI_TRIES; attempt++) {
    WiFi.disconnect(true);
    delay(50);
    WiFi.begin(useSsid, usePass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT) delay(200);
    if (WiFi.status() == WL_CONNECTED) return true;
  }
  return false;
}

void loadArchiveNVS() {
  prefs.begin("wichtel", true);
  int n = prefs.getInt("n", 0);
  archiveN = 0;
  for (int i = 0; i < n && i < ARCHIVE_MAX; i++) {
    String s = prefs.getString((String("m") + i).c_str(), "");
    if (!s.length()) break;
    JsonDocument o;
    if (deserializeJson(o, s)) break;
    archive[archiveN].text = String((const char *)(o["t"] | ""));
    archive[archiveN].from = String((const char *)(o["f"] | "Wichtel"));
    archive[archiveN].ts   = (uint32_t)(o["s"] | 0);
    archiveN++;
  }
  prefs.end();
}

// ---- Backend: Verlauf per HTTP holen ------------------------------------
bool httpFetchState() {
  ApiClient httpNet; apiClientPrep(httpNet);   // eigener Client (nicht der von MQTT!)
  HTTPClient http;
  String url = apiBase() + "/api/state";
  if (!http.begin(httpNet, url)) return false;
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray hist = doc["history"].as<JsonArray>();
  int n = 0;
  for (JsonObject m : hist) {
    if (n >= ARCHIVE_MAX) break;
    archive[n].text = String((const char *)(m["text"] | ""));
    archive[n].from = String((const char *)(m["from"] | "Wichtel"));
    archive[n].ts   = (uint32_t)(m["ts"].as<uint64_t>() / 1000ULL); // ms -> s
    n++;
  }
  // Falls (noch) kein Verlauf, aber eine "last"-Nachricht existiert:
  if (n == 0 && doc["last"].is<JsonObject>()) {
    archive[0].text = String((const char *)(doc["last"]["text"] | ""));
    archive[0].from = String((const char *)(doc["last"]["from"] | "Wichtel"));
    archive[0].ts   = (uint32_t)(doc["last"]["ts"].as<uint64_t>() / 1000ULL);
    n = 1;
  }
  archiveN = n;
  if (archiveN > 0) saveArchiveNVS();

  // ---- Offene Aufgaben (nur die im aktuellen Zeitraum offenen) ----
  tasksN = 0;
  JsonArray tk = doc["tasks"].as<JsonArray>();
  for (JsonObject t : tk) {
    if (tasksN >= TASK_MAX) break;
    if (!(t["open"] | false)) continue;            // nur offene zeigen
    tasks[tasksN].id   = (int)(t["id"] | 0);
    tasks[tasksN].text = String((const char *)(t["text"] | ""));
    tasks[tasksN].from = String((const char *)(t["from"] | "Wichtel"));
    const char *sc = t["scope"] | "once";
    tasks[tasksN].scope = !strcmp(sc, "day") ? 1 : !strcmp(sc, "week") ? 2 : !strcmp(sc, "month") ? 3 : 0;
    tasksN++;
  }
  return archiveN > 0;
}

// Meldet dem Backend, dass eine Aufgabe erledigt ist. Braucht WLAN (bei der
// interaktiven Bedienung ist es dafür an, solange offene Aufgaben da sind).
bool httpPostTaskDone(int id, int &stars, bool &allDone) {
  stars = 0; allDone = false;
  if (WiFi.status() != WL_CONNECTED) return false;
  ApiClient httpNet; apiClientPrep(httpNet);
  HTTPClient http;
  String url = apiBase() + "/api/task/" + String(id) + "/done";
  if (!http.begin(httpNet, url)) return false;
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.POST("");
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, http.getString())) {
      stars = (int)(d["stars"] | 0);
      allDone = d["allDone"] | false;
    }
  }
  http.end();
  return code == 200;
}

// Antwort von Max ans Backend senden (verbindet das WLAN bei Bedarf kurz neu).
bool httpPostReply(const char *text) {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;
  ApiClient net; apiClientPrep(net); HTTPClient http;
  String url = apiBase() + "/api/reply";
  if (!http.begin(net, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument o; o["text"] = text; String pl; serializeJson(o, pl);
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.POST(pl);
  http.end();
  return code == 200;
}

// Nachricht am Backend löschen (Identifikator = ts in Sekunden). Backend entfernt
// den passenden Verlauf-Eintrag, damit die Nachricht nicht wiederkommt.
bool httpDeleteMessage(uint32_t ts) {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;
  ApiClient net; apiClientPrep(net); HTTPClient http;
  String url = apiBase() + "/api/message/delete";
  if (!http.begin(net, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument o; o["ts"] = ts; String pl; serializeJson(o, pl);
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.POST(pl);
  http.end();
  return code == 200;
}

// Nachricht aus dem lokalen Archiv entfernen (+ NVS aktualisieren).
void removeLocalMsg(int idx) {
  if (idx < 0 || idx >= archiveN) return;
  for (int i = idx; i < archiveN - 1; i++) archive[i] = archive[i + 1];
  archiveN--;
  saveArchiveNVS();
}

// ---- OTA: Firmware aus der Ferne aktualisieren --------------------------
// Fragt das Backend nach der neuesten Firmware-Version. Ist sie höher als die
// eigene (FW_VERSION), lädt das Gerät die neue .bin und flasht sich selbst
// (danach automatischer Neustart). So lässt sich der Code aus der Ferne ändern,
// ohne das Gerät per USB anzustecken.
void checkOTA() {
  if (WiFi.status() != WL_CONNECTED) return;
  String base = apiBase();

  ApiClient infoNet; apiClientPrep(infoNet);
  HTTPClient http;
  if (!http.begin(infoNet, base + "/api/ota")) return;
  http.setTimeout(HTTP_TIMEOUT);
  if (http.GET() != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  JsonDocument d;
  if (deserializeJson(d, body)) return;
  int ver = d["version"] | 0;
  bool hasBin = d["hasBin"] | false;
  if (ver <= FW_VERSION || !hasBin) return;      // nichts Neueres da

  // Update verfügbar -> Hinweis zeigen und flashen (blockiert ~10-30 s).
  epdBegin(); showMood(MOOD_WAIT, "Update", "bitte warten ..."); epdEnd();

  httpUpdate.rebootOnUpdate(true);               // nach Erfolg automatisch neu starten
  ApiClient upNet; apiClientPrep(upNet);
  t_httpUpdate_return r = httpUpdate.update(upNet, base + "/api/firmware.bin");
  // Erfolg -> Gerät startet neu (kommt hier nicht zurück).
  if (r == HTTP_UPDATE_FAILED) {
    // Fehlgeschlagen -> normal weiterlaufen; nächstes Aufwachen versucht es erneut.
    epdBegin(); showMood(MOOD_WAIT, "Wichtel Max", "warte auf Post ..."); epdEnd();
  }
}

// ---- Heartbeat: Online-Status per HTTP an das Backend melden -------------
// Ersetzt den MQTT-Status. Das Gerät meldet sich bei jedem Aufwachen; das
// Backend zeigt "online", solange der letzte Heartbeat noch frisch ist.
// Funktioniert auch über HTTPS in der Cloud (im Gegensatz zu MQTT).
bool httpHeartbeat(bool online) {
  if (WiFi.status() != WL_CONNECTED) return false;
  ApiClient hbNet; apiClientPrep(hbNet); HTTPClient http;
  if (!http.begin(hbNet, apiBase() + "/api/heartbeat")) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument o;
  o["online"] = online;
  o["fw"]     = FW_VERSION;
  o["batt"]   = batteryPercent();
  o["rssi"]   = WiFi.RSSI();
  String pl; serializeJson(o, pl);
  http.setTimeout(HTTP_TIMEOUT);
  int code = http.POST(pl);
  http.end();
  return code == 200;
}

// ---- MQTT-Status (nur Online-Anzeige der Web-App) -----------------------
bool mqttStatus(bool online) {
  if (!mqtt.connected()) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(256);
    String cid = String("wichtel-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (!mqtt.connect(cid.c_str(),
                      strlen(MQTT_USER) ? MQTT_USER : nullptr,
                      strlen(MQTT_PASS) ? MQTT_PASS : nullptr,
                      topicStatus.c_str(), 1, true, "{\"online\":false}")) return false;
  }
  JsonDocument st; st["online"] = online; st["rssi"] = WiFi.RSSI(); st["fw"] = FW_VERSION;
  st["batt"] = batteryPercent();
  char buf[110]; size_t nb = serializeJson(st, buf);
  return mqtt.publish(topicStatus.c_str(), (const uint8_t *)buf, nb, true);
}

// ---- Zeit / Nachtruhe ----------------------------------------------------
void syncTimeNTP() {
  configTzTime(TZ_GERMANY, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  unsigned long t0 = millis();
  while (millis() - t0 < NTP_SYNC_MS) {
    if (time(nullptr) > 1700000000UL) { timeValid = true; return; }
    delay(50);
  }
}
bool isNightNow() {
  time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
  int h = tm.tm_hour;
  if (cfgNightStart < cfgNightEnd) return (h >= cfgNightStart && h < cfgNightEnd);
  return (h >= cfgNightStart || h < cfgNightEnd);
}
uint32_t secondsUntilMorning() {
  time_t now = time(nullptr); struct tm tm; localtime_r(&now, &tm);
  int nowSec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
  int endSec = cfgNightEnd * 3600;
  int secs = (nowSec < endSec) ? (endSec - nowSec) : (24 * 3600 - nowSec) + endSec;
  return secs < 60 ? 60 : (uint32_t)secs;
}

// ---- Power-Latch (nur Waveshare) -----------------------------------------
#if HAS_POWER_LATCH
void latchPowerOn() {
  rtc_gpio_hold_dis((gpio_num_t)VBAT_PWR);
  rtc_gpio_hold_dis((gpio_num_t)EPD_PWR);
  pinMode(VBAT_PWR, OUTPUT);
  digitalWrite(VBAT_PWR, HIGH);
}
#endif

void goToSleep() {
#if USE_MQTT_STATUS
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) { mqttStatus(false); mqtt.disconnect(); }
#endif
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);

  uint64_t sleepSec = (uint64_t)cfgPollMin * 60ULL;
  if (timeValid && isNightNow()) sleepSec = secondsUntilMorning();
  esp_sleep_enable_timer_wakeup(sleepSec * 1000000ULL);

#if HAS_POWER_LATCH
  digitalWrite(VBAT_PWR, HIGH);
  epdPowerOff();
  rtc_gpio_hold_en((gpio_num_t)VBAT_PWR);
  rtc_gpio_hold_en((gpio_num_t)EPD_PWR);
  #if HAS_BACK_BUTTON
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_WAKE) | (1ULL << BTN_POWER), ESP_EXT1_WAKEUP_ANY_LOW);
  #else
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_WAKE), ESP_EXT1_WAKEUP_ANY_LOW);
  #endif
#else
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_WAKE, 0);
#endif

  Serial.flush();
  esp_deep_sleep_start();
}

// ---- Interaktiver Reader (Tasten) ---------------------------------------
// Wartet, bis beide Tasten losgelassen sind (die Weck-Taste ist beim Start noch
// gedrückt) – verhindert Fehlbedienung direkt nach dem Aufwachen.
void waitButtonsReleased() {
  unsigned long t0 = millis();
  auto pressed = []() {
    bool p = (digitalRead(BTN_WAKE) == LOW);
#if HAS_BACK_BUTTON
    p = p || (digitalRead(BTN_POWER) == LOW);
#endif
    return p;
  };
  while (pressed() && millis() - t0 < 3000) delay(10);
  delay(40); // entprellen
}

// ---- Antwort-Knopf (Max schickt Papa eine kleine Rückmeldung) -----------
#if HAS_BACK_BUTTON
const char *REPLIES[] = { "Hab dich lieb!", "Fertig!", "Danke!", "Gute Nacht", "Ja", "Nein" };
const int REPLY_N = sizeof(REPLIES) / sizeof(REPLIES[0]);

void renderReplyOption(int sel) {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  bool back = (sel >= REPLY_N);              // letzte Option = zurück
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_BLACK);
    { String h = "Antwort an Lumbi:"; display.setCursor((W - textW(h)) / 2, 16); display.print(h); }
    drawCenteredFit(back ? "zurueck" : REPLIES[sel], H / 2 + 6, TITLE_FONTS, 3, maxW);
    if (!back) {
      display.setFont(FOOTER_FONT);
      String c = String(sel + 1) + "/" + String(REPLY_N);
      display.setCursor((W - textW(c)) / 2, H / 2 + 26); display.print(c);
    }
    // oberer Knopf = nächste Antwort; unterer = senden bzw. zurück
    drawButtonLabels("WEITER", back ? "ZURUECK" : "SENDEN");
  } while (display.nextPage());
}

// Oberer Knopf = nächste Antwort (inkl. "zurück" am Ende), unterer = senden.
void runReplyMenu() {
  int sel = 0;
  const int N = REPLY_N + 1;                 // + "zurück"
  renderReplyOption(sel);
  waitButtonsReleased();
  unsigned long lastActive = millis();
  int bootPrev = HIGH, pwrPrev = HIGH;
  for (;;) {
    int b = digitalRead(BTN_WAKE), p = digitalRead(BTN_POWER);
    if (bootPrev == HIGH && b == LOW) { sel = (sel + 1) % N; renderReplyOption(sel); lastActive = millis(); }
    if (pwrPrev == HIGH && p == LOW) {
      if (sel >= REPLY_N) break;             // zurück
      showMood(MOOD_WAIT, "sende ...", "");
      bool ok = httpPostReply(REPLIES[sel]);
      showMood(ok ? MOOD_DONE : MOOD_SAD, ok ? "Gesendet!" : "kein WLAN", "");
      delay(DONE_SHOW_MS);
      break;
    }
    bootPrev = b; pwrPrev = p;
    if (millis() - lastActive > IDLE_SLEEP_MS) break;
    delay(15);
  }
}
#endif // HAS_BACK_BUTTON

// Interaktives Ruhebild: zeigt das Wichtelgesicht und erlaubt (2-Tasten-Gerät)
// per BOOT-Langdruck eine Antwort an Papa. Ohne Nachrichten/Aufgaben.
void runIdleInteractive() {
  epdBegin();
  renderIdle();
  waitButtonsReleased();
  unsigned long lastActive = millis();
#if HAS_BACK_BUTTON
  int bootPrev = HIGH, pwrPrev = HIGH;
  for (;;) {
    int b = digitalRead(BTN_WAKE), p = digitalRead(BTN_POWER);
    // BOOT (links) = Antwort an Papa
    if (bootPrev == HIGH && b == LOW) { runReplyMenu(); renderIdle(); waitButtonsReleased(); lastActive = millis(); }
    // PWR (rechts) = schlafen legen (jeder Knopf weckt wieder)
    if (pwrPrev == HIGH && p == LOW) goToSleep();
    bootPrev = b; pwrPrev = p;
    if (millis() - lastActive > IDLE_SLEEP_MS) break;
    delay(15);
  }
#else
  while (millis() - lastActive < 3000) { if (digitalRead(BTN_WAKE) == LOW) lastActive = millis(); delay(20); }
#endif
  epdEnd();
}

#if HAS_BACK_BUTTON
// Sicherheitsabfrage vor dem Löschen: oben NEIN, unten JA. true = löschen.
bool confirmDelete() {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredFit("Loeschen?", H / 2 + 6, TITLE_FONTS, 3, maxW);
    drawButtonLabels("NEIN", "JA");
  } while (display.nextPage());
  waitButtonsReleased();
  unsigned long t0 = millis();
  int bootPrev = HIGH, pwrPrev = HIGH;
  for (;;) {
    int b = digitalRead(BTN_WAKE), p = digitalRead(BTN_POWER);
    if (bootPrev == HIGH && b == LOW) return false;   // NEIN
    if (pwrPrev == HIGH && p == LOW) return true;     // JA
    bootPrev = b; pwrPrev = p;
    if (millis() - t0 > IDLE_SLEEP_MS) return false;
    delay(15);
  }
}
#endif

// Nachrichten lesen: oberer Knopf = weiter blättern (am Ende zurück ins Menü),
// unterer Knopf = Nachricht löschen (mit Sicherheitsabfrage).
void runReader() {
  epdBegin();                 // Display einmal initialisieren, dann nur neu zeichnen
  renderCurrent();
  if (archive[curMsg].ts > lastReadTs) lastReadTs = archive[curMsg].ts;  // als gelesen markieren

  waitButtonsReleased();
  unsigned long lastActive = millis();
  int bootPrev = HIGH;
#if HAS_BACK_BUTTON
  int pwrPrev = HIGH;
#else
  unsigned long bootDown = 0;
#endif

  for (;;) {
    int b = digitalRead(BTN_WAKE);
#if HAS_BACK_BUTTON
    int p = digitalRead(BTN_POWER);
    if (bootPrev == HIGH && b == LOW) {                 // WEITER (am Ende zurück ins Menü)
      int pm = curMsg, pp = curPage;
      navNext();
      if (curMsg == pm && curPage == pp) break;
      renderCurrent(); lastActive = millis();
    }
    if (pwrPrev == HIGH && p == LOW) {                  // LOESCHEN (mit Abfrage)
      if (confirmDelete()) {
        showMood(MOOD_WAIT, "loesche ...", "");
        httpDeleteMessage(archive[curMsg].ts);
        removeLocalMsg(curMsg);
        if (archiveN == 0) break;                       // nichts mehr -> Menü
        if (curMsg >= archiveN) curMsg = archiveN - 1;
        curPage = 0;
      }
      renderCurrent(); lastActive = millis();
    }
    pwrPrev = p;
#else
    // Einknopf-Bedienung (CrowPanel): kurz = Weiter, lang = neueste
    if (bootPrev == HIGH && b == LOW) bootDown = millis();
    if (bootPrev == LOW && b == HIGH) {
      unsigned long held = millis() - bootDown;
      if (held >= LONGPRESS_MS) navNewest(); else navNext();
      renderCurrent(); lastActive = millis();
      bootDown = 0;
    }
#endif
    bootPrev = b;

    if (millis() - lastActive > IDLE_SLEEP_MS) break;
    delay(15);
  }
  saveLastRead();               // gelesene Nachrichten dauerhaft merken
  epdEnd();
}

// ---- Aufgaben am Gerät ---------------------------------------------------
// Zeigt Aufgabe ti (von n) mit Banner "AUFGABE  x/n", Zeitraum-Label und
// Hinweis "Knopf = fertig".
void renderTask(int ti, int n) {
  const int W = display.width(), H = display.height(), M = marginPx();
  const int bandH = 22;
  int scope = tasks[ti].scope;
  const char *scopeLbl = scope == 1 ? "bis heute" : scope == 2 ? "bis Sonntag" : scope == 3 ? "bis Monatsende" : "";
  int footH = (scope > 0) ? 34 : 20;
  // Lange Aufgaben in kleinerer Schrift, damit alles auf eine Seite passt.
  const GFXfont *tf = (tasks[ti].text.length() > 70) ? &FreeSansBold12pt7b : READER_FONT;
  int tAsc, tDesc; fontAscDesc(tf, tAsc, tDesc);
  int tLineH = tf->yAdvance;
  buildBase(tasks[ti].text);
  wrapWith(tf, W - 2 * SIDE_MARGIN);
  int avail = H - bandH - footH;
  int block = nLines * tLineH;
  int y = bandH + (avail - block) / 2 + tAsc;
  if (y < bandH + tAsc) y = bandH + tAsc;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    // Kopf-Banner + Zähler
    display.fillRect(0, 0, W, bandH, GxEPD_BLACK);
    display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_WHITE);
    { String h = "AUFGABE"; display.setCursor((W - textW(h)) / 2, 15); display.print(h); }
    if (n > 1) { String c = String(ti + 1) + "/" + String(n); display.setCursor(W - M - textW(c), 15); display.print(c); }
    display.setTextColor(GxEPD_BLACK);
    // Aufgabentext (vertikal zentriert)
    int yy = y;
    for (int i = 0; i < nLines; i++) { drawBaseLine(lines[i], tf, yy, tAsc, hyph[i]); yy += tLineH; }
    // Frist unten mittig ("bis wann")
    display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_BLACK);
    if (scope > 0) { display.setCursor((W - textW(scopeLbl)) / 2, H - 22); display.print(scopeLbl); }
    // Knopf-Labels rechts: oberer = nächste Aufgabe, unterer = erledigen
    drawButtonLabels("WEITER", "FERTIG");
  } while (display.nextPage());
}

// Aufgabe erledigt melden + belohnen: alle offenen geschafft -> "Super!"+Sterne,
// sonst "Erledigt".
void completeCurrentTask(int ti) {
  int stars; bool allDone;
  httpPostTaskDone(tasks[ti].id, stars, allDone);
  if (allDone) {
    char sub[20]; snprintf(sub, sizeof(sub), "%d Sterne!", stars);
    showMood(MOOD_SUPER, "Super!", sub);
  } else {
    showMood(MOOD_DONE, "Erledigt", "");
  }
#if HAS_AUDIO
  audioDone();
#endif
  delay(DONE_SHOW_MS);
}

// Zeigt die offenen Aufgaben zum Durchblättern. Oberer Knopf = nächste Aufgabe
// (am Ende zurück ins Menü). Unterer Knopf = aktuelle Aufgabe erledigen.
void runTasks() {
  epdBegin();
  int ti = 0;
  renderTask(ti, tasksN);
  waitButtonsReleased();
  unsigned long lastActive = millis();
  int bootPrev = HIGH;
#if HAS_BACK_BUTTON
  int pwrPrev = HIGH;
#else
  unsigned long bootDown = 0;
#endif

  while (ti < tasksN) {
    int b = digitalRead(BTN_WAKE);
#if HAS_BACK_BUTTON
    int p = digitalRead(BTN_POWER);
    // WEITER = nächste Aufgabe ansehen; nach der letzten zurück ins Menü
    if (bootPrev == HIGH && b == LOW) {
      ti++;
      if (ti >= tasksN) break;
      renderTask(ti, tasksN); lastActive = millis();
    }
    // FERTIG = aktuelle Aufgabe erledigen (aus der Liste nehmen, nächste zeigen)
    if (pwrPrev == HIGH && p == LOW) {
      completeCurrentTask(ti);
      for (int j = ti; j < tasksN - 1; j++) tasks[j] = tasks[j + 1];
      tasksN--;
      if (tasksN == 0) break;
      if (ti >= tasksN) ti = tasksN - 1;
      renderTask(ti, tasksN); lastActive = millis();
    }
    pwrPrev = p;
#else
    // Einknopf (CrowPanel): kurz = erledigt, lang = überspringen
    if (bootPrev == HIGH && b == LOW) bootDown = millis();
    if (bootPrev == LOW && b == HIGH) {
      unsigned long held = millis() - bootDown; bootDown = 0;
      if (held < LONGPRESS_MS) completeCurrentTask(ti);
      ti++; lastActive = millis();
      if (ti < tasksN) renderTask(ti, tasksN);
    }
#endif
    bootPrev = b;
    if (millis() - lastActive > IDLE_SLEEP_MS) break;
    delay(15);
  }
  epdEnd();
}

#if HAS_BACK_BUTTON
// Kurze, sauber zentrierte Meldung (ohne Gesicht) – z.B. "Keine Aufgaben".
void showInfo(const char *msg) {
  const int W = display.width(), H = display.height();
  const int maxW = W - 2 * marginPx();
  epdBegin();
  display.setFullWindow(); display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); drawCenteredFit(msg, H / 2 + 6, TITLE_FONTS, 3, maxW); } while (display.nextPage());
  epdEnd();
}

// ---- Screensaver (Ruhebild) + Hauptmenü ---------------------------------
// Ruhebild: Wichtelgesicht + "Hallo Max!" + Info-Kasten (Anzahl neuer Sachen) +
// Uhrzeit/Akku. Knöpfe: oben MENUE, unten SCHLAF.
void renderScreensaver() {
  const int W = display.width(), H = display.height();
  Mood m = (timeValid && isNightNow()) ? MOOD_SLEEP : MOOD_WAIT;
  int fx = (W - FACE_SM_W) / 2, fy = 6;
  int bpct = batteryPercent();
  char tbuf[6] = "";
  if (timeValid) { struct tm t; if (getLocalTime(&t, 5)) strftime(tbuf, sizeof(tbuf), "%H:%M", &t); }
  int unread = unreadCount();
  String info;
  if (unread > 0) info = String(unread) + " neue Post";
  if (tasksN > 0) { if (info.length()) info += "  "; info += String(tasksN) + " Aufg."; }
  if (info.length() == 0) info = "Nichts Neues";
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(fx, fy, FACE_SM_BMP[m], FACE_SM_W, FACE_SM_H, GxEPD_BLACK);
    const GFXfont *tf[] = { &FreeSansBold12pt7b };
    drawCenteredFit("Hallo Max!", fy + FACE_SM_H + 22, tf, 1, W - 2 * marginPx());
    display.setFont(FOOTER_FONT); display.setTextColor(GxEPD_BLACK);
    int iw = textW(info) + 12, ix = (W - iw) / 2, iy = H - 42;
    display.drawRect(ix, iy, iw, 20, GxEPD_BLACK);
    display.setCursor(ix + 6, iy + 14); display.print(info);
    if (tbuf[0]) { display.setCursor(6, H - 4); display.print(tbuf); }
    if (bpct >= 0) { String pc = String(bpct) + "%"; display.setCursor(W - 26 - textW(pc), H - 4); display.print(pc); }
    drawButtonLabels("MENU", "");
  } while (display.nextPage());
}

// Hauptmenü: Titelbalken + Liste mit Markierung. Knöpfe: oben WEITER, unten OK.
void renderMenu(int sel) {
  const int W = display.width();
  int unread = unreadCount();
  String items[3] = {
    unread > 0 ? ("Nachrichten (" + String(unread) + " neu)") : String("Nachrichten"),
    "Aufgaben (" + String(tasksN) + ")",
    "Antwort"
  };
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, W, 26, GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b); display.setTextColor(GxEPD_WHITE);
    { String t = "Menu";                       // "Menü" mit echten Umlaut-Punkten über dem u
      int tx = (W - textW(t)) / 2;
      display.setCursor(tx, 19); display.print(t);
      int ucx = tx + textW(t) - textW("u") / 2; // Mitte des u (vom rechten Rand her)
      display.fillCircle(ucx - 2, 8, 1, GxEPD_WHITE);
      display.fillCircle(ucx + 2, 8, 1, GxEPD_WHITE);
    }
    display.setFont(&FreeSansBold9pt7b);        // kleinere Listenschrift (damit "(2)" nicht abgeschnitten wird)
    int y0 = 36, rowH = 34;
    for (int i = 0; i < 3; i++) {
      int ry = y0 + i * rowH;
      if (i == sel) { display.fillRect(4, ry, W - 26, 26, GxEPD_BLACK); display.setTextColor(GxEPD_WHITE); }
      else display.setTextColor(GxEPD_BLACK);
      display.setCursor(12, ry + 17); display.print(items[i]);
    }
    drawButtonLabels("WEITER", "OK");
  } while (display.nextPage());
}

// Menü-Schleife: WEITER schiebt die Markierung, OK öffnet. Kehrt bei Timeout
// zurück (danach schläft das Gerät automatisch).
void runMainMenu() {
  const int N = 3;
  int sel = 0;
  epdBegin();
  renderMenu(sel);
  waitButtonsReleased();
  unsigned long lastActive = millis();
  int bootPrev = HIGH, pwrPrev = HIGH;
  for (;;) {
    int b = digitalRead(BTN_WAKE), p = digitalRead(BTN_POWER);
    if (bootPrev == HIGH && b == LOW) { sel = (sel + 1) % N; renderMenu(sel); lastActive = millis(); }
    if (pwrPrev == HIGH && p == LOW) {
      epdEnd();                                       // Unterfunktion initialisiert das Display selbst neu
      if (sel == 0) {
        if (archiveN > 0) { curMsg = 0; curPage = 0; runReader(); }
        else { showInfo("Keine Post"); delay(1200); }
      } else if (sel == 1) {
        if (tasksN > 0) runTasks();
        else { showInfo("Keine Aufgaben"); delay(1200); }
      } else if (sel == 2) {
        epdBegin(); runReplyMenu(); epdEnd();
      }
      epdBegin(); renderMenu(sel);                    // zurück im Menü
      waitButtonsReleased(); lastActive = millis();
    }
    bootPrev = b; pwrPrev = p;
    if (millis() - lastActive > IDLE_SLEEP_MS) { epdEnd(); return; }
    delay(15);
  }
}

// Screensaver-Schleife: oben öffnet das Menü, unten legt schlafen.
void runScreensaver() {
  epdBegin();
  renderScreensaver();
  waitButtonsReleased();
  unsigned long lastActive = millis();
  int bootPrev = HIGH, pwrPrev = HIGH;
  for (;;) {
    int b = digitalRead(BTN_WAKE), p = digitalRead(BTN_POWER);
    // beide Knöpfe öffnen das Menü (im Menü gibt es "Schlafen")
    if (bootPrev == HIGH && b == LOW) { epdEnd(); runMainMenu(); return; }
    if (pwrPrev == HIGH && p == LOW) { epdEnd(); runMainMenu(); return; }
    bootPrev = b; pwrPrev = p;
    if (millis() - lastActive > IDLE_SLEEP_MS) { epdEnd(); return; }
    delay(15);
  }
}
#endif // HAS_BACK_BUTTON

// ---- Signalton (optional, nur wenn BUZZER_PIN gesetzt) -------------------
// Kurze Piep-Folge bei neuer Nachricht/Aufgabe. Braucht einen passiven Piezo an
// BUZZER_PIN + GND. Ohne Buzzer (BUZZER_PIN < 0) passiert nichts.
void notifyBeep(int beeps) {
#if BUZZER_PIN >= 0
  for (int i = 0; i < beeps; i++) {
    tone(BUZZER_PIN, 2200, 120);
    delay(180);
  }
  noTone(BUZZER_PIN);
#else
  (void)beeps;
#endif
}

// ---- WLAN-Einrichtungs-Modus (Captive Portal) ---------------------------
#define WIFI_SETUP_TIMEOUT 300000UL   // 5 min ohne Eingabe -> zurück/schlafen

// E-Paper-Anleitung während der Einrichtung.
void showWifiSetupScreen() {
  const int W = display.width(); const int maxW = W - 2 * marginPx();
  const GFXfont *hd[] = { &FreeSansBold12pt7b, &FreeSansBold9pt7b };
  const GFXfont *sm[] = { &FreeSans9pt7b };
  display.setFullWindow(); display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredFit("WLAN einrichten", 24, hd, 2, maxW);
    drawCenteredFit("1) Handy-WLAN:", 58, sm, 1, maxW);
    drawCenteredFit("Wichtel-Setup", 84, TITLE_FONTS, 3, maxW);
    drawCenteredFit("2) Seite folgt", 120, sm, 1, maxW);
    drawCenteredFit("3) Dein WLAN eintragen", 142, sm, 1, maxW);
    drawCenteredFit("sonst: 192.168.4.1", 170, sm, 1, maxW);
  } while (display.nextPage());
}

// Öffnet ein eigenes WLAN „Wichtel-Setup" + Konfig-Seite, speichert SSID/Passwort
// in NVS und startet neu. Bei Timeout ohne Speichern kehrt es zurück.
void runWifiSetup() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  int nnet = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < nnet && i < 20; i++) {
    String s = WiFi.SSID(i); s.replace("<", ""); s.replace(">", ""); s.replace("\"", "'");
    if (s.length()) opts += "<option>" + s + "</option>";
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("Wichtel-Setup");
  IPAddress apIP = WiFi.softAPIP();
  DNSServer dns; dns.start(53, "*", apIP);
  WebServer web(80);
  bool saved = false;

  web.on("/", [&]() {
    String h = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Wichtel WLAN</title><style>body{font-family:sans-serif;background:#0e1a12;color:#eaf3ec;margin:0;padding:22px}"
      "h2{color:#7ee0a0}label{display:block;margin:12px 0 4px;font-size:14px;color:#9fc4ab}"
      "input,select{width:100%;padding:11px;border-radius:8px;border:1px solid #2c4733;background:#0f1d14;color:#eaf3ec;font-size:16px;box-sizing:border-box}"
      "button{width:100%;margin-top:16px;padding:14px;border:0;border-radius:10px;background:#1f7a3d;color:#fff;font-size:17px;font-weight:700}</style></head>"
      "<body><h2>&#127876; Wichtel WLAN</h2><form action='/save' method='post'>"
      "<label>WLAN auswaehlen</label><select name='ssid'>" + opts + "</select>"
      "<label>oder Name selbst eingeben</label><input name='ssid2' placeholder='(optional)'>"
      "<label>Passwort</label><input name='pass' type='password'>"
      "<button type='submit'>Speichern &amp; Neustart</button></form></body></html>";
    web.send(200, "text/html", h);
  });
  web.on("/save", HTTP_POST, [&]() {
    String ssid = web.arg("ssid2"); ssid.trim();
    if (!ssid.length()) ssid = web.arg("ssid");
    String pass = web.arg("pass");
    if (ssid.length()) {
      prefs.begin("wifi", false);
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      prefs.end();
      saved = true;
      web.send(200, "text/html", "<meta charset=utf-8><body style='font-family:sans-serif;background:#0e1a12;color:#eaf3ec;padding:24px'>"
        "<h2 style='color:#7ee0a0'>Gespeichert!</h2><p>Der Wichtel startet neu und verbindet sich mit <b>" + ssid + "</b>.</p></body>");
    } else {
      web.send(400, "text/html", "<meta charset=utf-8><body>Bitte ein WLAN angeben. <a href='/'>zurueck</a></body>");
    }
  });
  web.onNotFound([&]() { web.sendHeader("Location", String("http://") + apIP.toString()); web.send(302, "text/plain", ""); });
  web.begin();

  epdBegin(); showWifiSetupScreen(); epdEnd();
  Serial.print("WLAN-Setup: mit AP 'Wichtel-Setup' verbinden, dann http://"); Serial.println(apIP);

  unsigned long t0 = millis();
  while (!saved && millis() - t0 < WIFI_SETUP_TIMEOUT) { dns.processNextRequest(); web.handleClient(); delay(2); }

  web.stop(); dns.stop(); WiFi.softAPdisconnect(true);
  if (saved) { delay(1500); ESP.restart(); }
}

// ---- Terminal / Konsole (Telnet über WLAN + USB-Seriell) -----------------
#define CONSOLE_PORT       23
#define CONSOLE_TIMEOUT_MS 300000UL   // 5 min ohne Eingabe -> zurück in den Schlaf

// Sammelt Print-Ausgaben in einem String (für die Fernsteuerung: Befehlsausgabe
// zurück ans Backend).
class StrPrint : public Print {
public:
  String s;
  size_t write(uint8_t c) override { s += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) override { for (size_t i = 0; i < n; i++) s += (char)b[i]; return n; }
};

// Führt einen Konsolenbefehl aus; Ausgabe geht an `out` (Serial, Telnet-Client
// ODER StrPrint bei der Fernsteuerung). Rückgabe true = Konsole beenden.
bool consoleExec(String line, Print &out) {
  line.trim();
  if (!line.length()) return false;
  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? String("") : line.substring(sp + 1);
  rest.trim(); cmd.toLowerCase();

  if (cmd == "help") {
    out.println("Befehle:");
    out.println("  status | ip | fetch | ota");
    out.println("  mood <0-5>   (0=wartet 1=neu 2=erledigt 3=super 4=traurig 5=schlaeft)");
    out.println("  msg          Briefumschlag-Signal zeigen");
    out.println("  beep [n]     Signalton (Buzzer, falls BUZZER_PIN gesetzt)");
#if HAS_AUDIO
    out.println("  audio [test|msg|task|done|tone <f> <ms>|reg]   ES8311-Lautsprecher");
#endif
    out.println("  set poll|nightstart|nightend|vol <wert>");
    out.println("  i2c scan | i2c r <addr> <reg> | i2c w <addr> <reg> <val>   (Hex ok)");
    out.println("  wifi | wifi clear   (WLAN-Einrichtung / Zugangsdaten loeschen)");
    out.println("  sleep | reboot | exit");
  } else if (cmd == "status") {
    out.printf("Firmware v%d\n", FW_VERSION);
    out.print("IP: "); out.println(WiFi.localIP());
    out.printf("WLAN: %s (%d dBm)\n", WiFi.status() == WL_CONNECTED ? "verbunden" : "getrennt", WiFi.RSSI());
    out.printf("Nachrichten: %d, Aufgaben: %d\n", archiveN, tasksN);
    out.printf("Config: poll=%dmin, Nacht %d-%d Uhr, vol=%d\n", cfgPollMin, cfgNightStart, cfgNightEnd, cfgVolume);
    { int bp = batteryPercent(); if (bp >= 0) out.printf("Batterie: %d%%\n", bp); }
#if HAS_AUDIO
    out.println("Onboard-Audio (ES8311) vorhanden: i2c scan zeigt 0x18");
#endif
  } else if (cmd == "ip") {
    out.println(WiFi.localIP());
  } else if (cmd == "fetch") {
    bool ok = httpFetchState();
    out.printf("fetch %s: %d Nachrichten, %d Aufgaben\n", ok ? "ok" : "leer/fehler", archiveN, tasksN);
  } else if (cmd == "ota") {
    out.println("OTA-Check (startet neu, falls neuere Version)...");
    checkOTA();
    out.println("keine neuere Firmware hinterlegt.");
  } else if (cmd == "mood") {
    int n = rest.toInt();
    const char *nm[] = { "wartet", "neue Aufgabe", "erledigt", "super", "traurig", "schlaeft" };
    if (n < 0 || n > 5) { out.println("mood 0..5"); }
    else { epdBegin(); showMood((Mood)n, nm[n], ""); epdEnd(); out.printf("zeige: %s\n", nm[n]); }
  } else if (cmd == "msg") {
    epdBegin(); renderMessageAlert(); epdEnd(); out.println("Briefumschlag gezeigt.");
  } else if (cmd == "beep") {
    int n = rest.length() ? rest.toInt() : 2; if (n < 1) n = 1; if (n > 9) n = 9;
    notifyBeep(n); out.printf("beep x%d (nur hörbar, wenn BUZZER_PIN gesetzt)\n", n);
  } else if (cmd == "audio") {
#if HAS_AUDIO
    // audio [test] | msg | task | done | tone <f> <ms> | reg
    // Direkter ES8311-Test ueber den Onboard-Lautsprecher, iterativ ohne Neuflashen.
    int s2 = rest.indexOf(' ');
    String sub = (s2 < 0) ? rest : rest.substring(0, s2); sub.toLowerCase();
    String ar  = (s2 < 0) ? String("") : rest.substring(s2 + 1); ar.trim();
    if (sub == "" || sub == "test") {
      out.println("Selbsttest: 1 kHz, 2 s, laut...");
      audioSelfTest();
    } else if (sub == "msg")  { out.println("Melodie: neue Nachricht"); audioMessage(); }
    else if (sub == "task")   { out.println("Melodie: neue Aufgabe");   audioTask(); }
    else if (sub == "done")   { out.println("Melodie: erledigt");       audioDone(); }
    else if (sub == "tone") {
      int k = ar.indexOf(' ');
      int f  = (k < 0 ? ar : ar.substring(0, k)).toInt();
      int ms = (k < 0) ? 400 : ar.substring(k + 1).toInt();
      if (f < 50) f = 1000; if (ms < 20) ms = 400;
      out.printf("Ton: %d Hz, %d ms, laut...\n", f, ms);
      audioInit(); audioTone(f, ms, 0.9f); audioOff();
    } else if (sub == "reg") {
      audioInit();
      out.printf("ES8311 @0x%02X  ID: 0xFD=%02X 0xFE=%02X 0xFF=%02X  (erwartet 83 11 ..)\n",
                 ES8311_ADDR, es8311R(0xFD), es8311R(0xFE), es8311R(0xFF));
      out.printf("  0x00=%02X 0x01=%02X 0x12(DACpwr)=%02X 0x32(vol)=%02X 0x31(mute)=%02X\n",
                 es8311R(0x00), es8311R(0x01), es8311R(0x12), es8311R(0x32), es8311R(0x31));
      out.printf("  PWR(GPIO%d)=%d PA(GPIO%d)=%d\n",
                 AUDIO_PWR, digitalRead(AUDIO_PWR), AUDIO_PA, digitalRead(AUDIO_PA));
    } else {
      out.println("audio [test|msg|task|done|tone <f> <ms>|reg]");
    }
#else
    out.println("dieses Board hat kein Onboard-Audio.");
#endif
  } else if (cmd == "i2c") {
#if HAS_I2C
    int s2 = rest.indexOf(' ');
    String sub = (s2 < 0) ? rest : rest.substring(0, s2);
    String args = (s2 < 0) ? String("") : rest.substring(s2 + 1); args.trim();
    long a[3] = {0, 0, 0}; int na = 0;
    while (args.length() && na < 3) {
      int k = args.indexOf(' ');
      String tok = (k < 0) ? args : args.substring(0, k);
      a[na++] = strtol(tok.c_str(), nullptr, 0);
      args = (k < 0) ? String("") : args.substring(k + 1); args.trim();
    }
    if (sub == "scan") {
      int found = 0;
      for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) { out.printf("  gefunden: 0x%02X\n", addr); found++; }
      }
      out.printf("%d Geraet(e) am I2C-Bus.\n", found);
    } else if (sub == "r" && na >= 2) {
      Wire.beginTransmission((uint8_t)a[0]); Wire.write((uint8_t)a[1]);
      if (Wire.endTransmission(false) != 0) { out.println("kein ACK"); return false; }
      Wire.requestFrom((uint8_t)a[0], (uint8_t)1);
      if (Wire.available()) out.printf("0x%02X reg 0x%02X = 0x%02X\n", (int)a[0], (int)a[1], Wire.read());
      else out.println("keine Antwort");
    } else if (sub == "w" && na >= 3) {
      Wire.beginTransmission((uint8_t)a[0]); Wire.write((uint8_t)a[1]); Wire.write((uint8_t)a[2]);
      out.printf("write %s\n", Wire.endTransmission() == 0 ? "ok" : "fehler");
    } else {
      out.println("i2c scan | i2c r <addr> <reg> | i2c w <addr> <reg> <val>");
    }
#else
    out.println("dieses Board hat kein I2C hinterlegt.");
#endif
  } else if (cmd == "set") {
    int s2 = rest.indexOf(' ');
    String key = (s2 < 0) ? rest : rest.substring(0, s2);
    int val = (s2 < 0) ? 0 : rest.substring(s2 + 1).toInt();
    key.toLowerCase();
    if      (key == "poll"       && val >= 1 && val <= 1440) cfgPollMin = val;
    else if (key == "nightstart" && val >= 0 && val <= 23)   cfgNightStart = val;
    else if (key == "nightend"   && val >= 0 && val <= 23)   cfgNightEnd = val;
    else if (key == "vol"        && val >= 0 && val <= 100)  cfgVolume = val;
    else { out.println("set poll|nightstart|nightend|vol <wert>"); return false; }
    saveConfigNVS();
    out.printf("gesetzt: %s = %d\n", key.c_str(), val);
  } else if (cmd == "wifi") {
    if (rest == "clear") {
      prefs.begin("wifi", false); prefs.clear(); prefs.end();
      out.println("WLAN-Daten geloescht. Beim naechsten Start kommt das Einrichtungs-Portal.");
    } else {
      out.println("starte WLAN-Einrichtung (AP 'Wichtel-Setup')...");
      runWifiSetup();
    }
  } else if (cmd == "sleep") {
    out.println("gehe schlafen. tschuess!"); return true;
  } else if (cmd == "reboot") {
    out.println("reboot..."); delay(200); ESP.restart();
  } else if (cmd == "exit") {
    out.println("bye"); return true;
  } else {
    out.println("unbekannt - 'help' fuer Befehle");
  }
  return false;
}

// Konsolen-Sitzung: WLAN + Telnet-Server (Port 23) + USB-Seriell parallel.
void runConsole() {
  connectWiFi();
#if HAS_I2C
  Wire.begin(I2C_SDA, I2C_SCL);
#endif
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("kein WLAN");
  epdBegin(); showMood(MOOD_WAIT, "Terminal", ip.c_str()); epdEnd();

  Serial.println();
  Serial.println("=== Wichtel Max Terminal ===");
  Serial.print("Telnet:  telnet "); Serial.println(ip);
  Serial.println("oder hier per USB-Seriell. 'help' fuer Befehle.");
  Serial.print("> ");

  WiFiServer server(CONSOLE_PORT);
  if (WiFi.status() == WL_CONNECTED) server.begin();
  WiFiClient client;
  String sbuf, cbuf;
  unsigned long lastActive = millis();
  bool quit = false;

  while (!quit) {
    if (WiFi.status() == WL_CONNECTED && server.hasClient()) {
      if (client && client.connected()) client.stop();
      client = server.available();
      client.println("=== Wichtel Max Terminal ===  'help' fuer Befehle");
      client.print("> ");
      lastActive = millis();
    }
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') { if (sbuf.length()) { if (consoleExec(sbuf, Serial)) quit = true; sbuf = ""; Serial.print("> "); lastActive = millis(); } }
      else if (c >= 32) sbuf += c;
    }
    if (client && client.connected()) {
      while (client.available()) {
        char c = client.read();
        if (c == '\n' || c == '\r') { if (cbuf.length()) { if (consoleExec(cbuf, client)) quit = true; cbuf = ""; client.print("> "); lastActive = millis(); } }
        else if (c >= 32) cbuf += c;
      }
    }
    if (millis() - lastActive > CONSOLE_TIMEOUT_MS) { Serial.println("Timeout - schlafe."); quit = true; }
    delay(10);
  }
  if (client) client.stop();
  server.end();
}

// ---- Fernsteuerung: Befehle + Config vom Backend holen und ausführen -----
// Holt offene Befehle (GET /api/pull), wendet die Fern-Konfiguration an, führt
// die Konsolen-Befehle aus und meldet die Ausgabe zurück (POST /api/log).
void pullRemote() {
  if (WiFi.status() != WL_CONNECTED) return;
  String base = apiBase();

  ApiClient net2; apiClientPrep(net2); HTTPClient http;
  if (!http.begin(net2, base + "/api/pull")) return;
  http.setTimeout(HTTP_TIMEOUT);
  if (http.GET() != 200) { http.end(); return; }
  String body = http.getString();
  http.end();

  JsonDocument d;
  if (deserializeJson(d, body)) return;

  // Fern-Konfiguration anwenden
  JsonObject c = d["config"];
  if (!c.isNull()) {
    int v; bool ch = false;
    v = c["pollMin"]    | cfgPollMin;    if (v >= 1 && v <= 1440 && v != cfgPollMin)    { cfgPollMin = v; ch = true; }
    v = c["nightStart"] | cfgNightStart; if (v >= 0 && v <= 23  && v != cfgNightStart) { cfgNightStart = v; ch = true; }
    v = c["nightEnd"]   | cfgNightEnd;   if (v >= 0 && v <= 23  && v != cfgNightEnd)   { cfgNightEnd = v; ch = true; }
    v = c["volume"]     | cfgVolume;     if (v >= 0 && v <= 100 && v != cfgVolume)     { cfgVolume = v; ch = true; }
    if (ch) saveConfigNVS();
  }

  // Befehle ausführen und Ausgabe sammeln
  JsonArray cmds = d["commands"].as<JsonArray>();
  if (cmds.isNull() || cmds.size() == 0) return;
  StrPrint sp;
  for (JsonVariant cv : cmds) {
    String cmd = String((const char *)(cv | ""));
    if (!cmd.length()) continue;
    sp.print("$ "); sp.println(cmd);
    consoleExec(cmd, sp);
  }
  if (sp.s.length()) {
    ApiClient net3; apiClientPrep(net3); HTTPClient h2;
    if (h2.begin(net3, base + "/api/log")) {
      h2.addHeader("Content-Type", "application/json");
      JsonDocument o; o["text"] = sp.s;
      String pl; serializeJson(o, pl);
      h2.setTimeout(HTTP_TIMEOUT);
      h2.POST(pl);
      h2.end();
    }
  }
}

#if BOOT_DIAG
// Diagnose (nur bei BOOT_DIAG=1): prueft Tasten + scannt den I2C-Bus alle 2 s.
void bootDiag() {
#if HAS_I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  auto scan = []() {
    Serial.print("I2C-Bus: "); int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf("0x%02X ", a); found++; }
    }
    Serial.printf(" -> %d Geraet(e)\n", found);
  };
#endif
  Serial.println(F("\n==== BOOT-DIAG START ===="));
  Serial.printf("wake-cause: %d (0=Kaltstart,4=Timer,7=Taste)\n", (int)esp_sleep_get_wakeup_cause());
  unsigned long t0 = millis(), lastBeat = 0;
  while (millis() - t0 < 40000) {
    if (millis() - lastBeat >= 2000) {
      lastBeat = millis();
#if HAS_I2C
      Serial.print("[scan] "); scan();
#endif
#if HAS_BACK_BUTTON
      Serial.printf("[t=%lus] BOOT(GPIO%d)=%s  PWR(GPIO%d)=%s\n",
        (millis() - t0) / 1000,
        BTN_WAKE,  digitalRead(BTN_WAKE)  == LOW ? "GEDRUECKT" : "frei",
        BTN_POWER, digitalRead(BTN_POWER) == LOW ? "GEDRUECKT" : "frei");
#else
      Serial.printf("[t=%lus] BTN(GPIO%d)=%s\n", (millis() - t0) / 1000,
        BTN_WAKE, digitalRead(BTN_WAKE) == LOW ? "GEDRUECKT" : "frei");
#endif
    }
    delay(60);
  }
  Serial.println(F("==== BOOT-DIAG Ende ===="));
}
#endif

// ---- Setup (läuft bei jedem Aufwachen neu) -------------------------------
void setup() {
  Serial.begin(115200);
  if (!cfgLoaded) loadConfigNVS();     // Fern-Konfiguration (Poll/Nacht/Vol) aus NVS
  loadLastRead();                      // "gelesen"-Merker

#if HAS_POWER_LATCH
  latchPowerOn();
#endif

  pinMode(BTN_WAKE, INPUT_PULLUP);
#if HAS_BACK_BUTTON
  pinMode(BTN_POWER, INPUT_PULLUP);
  #define CONSOLE_BTN BTN_POWER
#else
  #define CONSOLE_BTN BTN_WAKE
#endif

#if BOOT_DIAG
  bootDiag();                          // TEMPORAER: Diagnose vor allem anderen
#endif

  // ---- Terminal-Modus: Konsolen-Taste beim Aufwachen ~2 s halten ----
  if (digitalRead(CONSOLE_BTN) == LOW) {
    unsigned long tc = millis(); bool held = true;
    while (millis() - tc < 2000) { if (digitalRead(CONSOLE_BTN) != LOW) { held = false; break; } delay(20); }
    if (held) { runConsole(); goToSleep(); }   // goToSleep kehrt nicht zurück
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool timerWake = (cause == ESP_SLEEP_WAKEUP_TIMER);
  bool interactive = !timerWake;   // Tastendruck oder Kaltstart -> Kind ist da

#if HAS_AUDIO
  // Begrüßungston beim echten Einschalten (Power-On/Reset) -> auch Audio-Selbsttest.
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) audioSelfTest();
#endif

  // Nachtruhe: reines Timer-Aufwachen nachts -> ohne WLAN weiterschlafen.
  if (timerWake && timeValid && isNightNow()) goToSleep();

  // ---- Daten holen (WLAN) oder offline aus NVS laden ----
  bool online = connectWiFi();
  // Einrichtungs-Portal NUR, wenn noch nie WLAN eingerichtet wurde. Sind Daten
  // gespeichert, die Verbindung scheitert aber (WLAN-Hickser), NICHT ins Portal
  // springen -> sonst sitzt man bei jedem Fehlversuch fest. Stattdessen offline
  // weiterarbeiten (Cache zeigen); beim naechsten Aufwachen neuer Versuch.
  // (WLAN spaeter aendern: im Terminal "wifi" bzw. "wifi clear".)
  // Portal, wenn noch nie eingerichtet ODER bei echtem Kaltstart (Power-On) das
  // WLAN nicht klappt -> erlaubt Neu-Einrichten nach Standortwechsel, ohne bei
  // jedem Button-Wecken in der Falle zu landen.
  bool coldBoot = (cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  if (!online && interactive && (!hasStoredWiFi() || coldBoot)) {
    runWifiSetup();                             // speichert -> Neustart; sonst weiter offline
    online = connectWiFi();
  }
  if (online) {
    httpHeartbeat(true);                         // Online-Status ans Backend (HTTP)
#if USE_MQTT_STATUS
    mqttStatus(true);
#endif
    syncTimeNTP();
    checkOTA();                                 // ggf. neue Firmware aus der Ferne (startet neu)
    pullRemote();                               // Fern-Befehle + Config anwenden
    if (!httpFetchState()) loadArchiveNVS();   // Backend leer/Fehler -> Cache
  } else {
    loadArchiveNVS();
  }

  bool newTasks = (tasksN > 0) && (taskSig() != lastTaskSig);
  bool newMsg   = (archiveN > 0) && (hashMsg(archive[0].text, archive[0].from) != lastShownHash);

#if HAS_BACK_BUTTON
  // ---- Bedienung: Screensaver -> Hauptmenü -> Punkte ----
  if (interactive) {
    // WLAN nur anlassen, solange Aufgaben quittiert werden könnten.
    if (online) {
#if USE_MQTT_STATUS
      if (mqtt.connected()) { mqttStatus(false); mqtt.disconnect(); }
#endif
      if (tasksN == 0) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
    }
    runScreensaver();                              // Ruhebild -> Menü -> ... -> Schlaf
    if (archiveN > 0) { lastShownHash = hashMsg(archive[0].text, archive[0].from); navSig = archive[0].ts; }
    lastTaskSig = taskSig();
  } else {
    // Timer-Aufwachen: nur wenn etwas Neues da ist -> Ruhebild zeigen + Ton.
    if (newTasks || newMsg) {
      epdBegin(); renderScreensaver(); epdEnd();
      notifyBeep(newTasks ? 3 : 2);
#if HAS_AUDIO
      if (newTasks) audioTask(); else audioMessage();
#endif
      if (archiveN > 0) lastShownHash = hashMsg(archive[0].text, archive[0].from);
      lastTaskSig = taskSig();
    }
  }
#else
  // ---- Einknopf-Fallback (CrowPanel): altes Verhalten ----
  if (archiveN == 0 && tasksN == 0) { epdBegin(); renderIdle(); epdEnd(); goToSleep(); }
  uint32_t sig = (archiveN > 0) ? archive[0].ts : 0;
  if (archiveN > 0) {
    if (!timerWake && !newMsg && sig == navSig && navMsg < archiveN) { curMsg = navMsg; curPage = navPage; }
    else { curMsg = 0; curPage = 0; }
  }
  if (interactive) {
    if (tasksN > 0) { runTasks(); lastTaskSig = taskSig(); }
    if (archiveN > 0) {
      runReader();
      navMsg = curMsg; navPage = curPage; navSig = sig;
      lastShownHash = hashMsg(archive[0].text, archive[0].from);
    }
  } else {
    if (newTasks) { epdBegin(); renderTaskAlert(tasksN); epdEnd(); notifyBeep(3); lastTaskSig = taskSig(); }
    else if (newMsg) {
      curMsg = 0; curPage = 0;
      epdBegin(); renderMessageAlert(); epdEnd(); notifyBeep(2);
      navMsg = 0; navPage = 0; navSig = sig;
      lastShownHash = hashMsg(archive[0].text, archive[0].from);
    }
  }
#endif

  goToSleep();
}

void loop() { /* nie erreicht – Gerät schläft nach setup() */ }
