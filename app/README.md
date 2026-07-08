# 📱 Wichtel Max – Papa-App (Android, Flutter)

Kleine Android-App, mit der du Nachrichten & Aufgaben an den Wichtel schickst,
Max' Antworten und den Akkustand siehst und das Gerät fernsteuerst. Sie ist ein
**Client** zum vorhandenen Backend (dieselbe REST-API wie die Web-App).

## Tabs
- **Nachricht** – Text + Absender + Schnell-Sprüche senden
- **Aufgaben** – anlegen (mit **Zeitraum**: einmalig / jeden Tag / Woche / Monat),
  abhaken, löschen; ⭐ Sterne-Zähler
- **Max** – Geräte-Status (online, 🔋 Akku, Firmware/Update) + „💬 Max hat geantwortet"
- **⚙️ Einstellungen** (oben rechts) – **Backend-Adresse** eintragen + Fernsteuerungs-Befehle

## Bauen / Installieren
Voraussetzung: Flutter-SDK (hast du). Im Ordner `app`:

```bash
flutter pub get
flutter run                 # auf angestecktem Handy/Emulator starten
# oder eine APK bauen:
flutter build apk --release # Ergebnis: build/app/outputs/flutter-apk/app-release.apk
```
Die fertige `app-release.apk` einfach aufs Handy kopieren und installieren
(„Installation aus unbekannten Quellen" erlauben).

> Hinweis: In `android/app/src/main/AndroidManifest.xml` ist
> `android:usesCleartextTraffic="true"` gesetzt, damit die App auch per **http://**
> mit dem lokalen Backend reden darf. Bei einer Cloud-Adresse mit **https://** ist
> das nicht nötig.

## Erste Einrichtung in der App
Beim ersten Start nach der **Backend-Adresse** fragen:
- **Zum Testen im Heimnetz:** die **LAN-IP des Backend-PCs** + Port, z. B.
  `http://192.168.1.50:8080` (nicht `localhost` – das wäre das Handy selbst!).
- **Von überall (Ziel):** sobald das Backend in der Cloud liegt, dessen
  `https://…`-Adresse eintragen.

## „Von überall" – der nächste Schritt
Damit du **außer Haus** senden kannst, muss das Backend im **Internet** erreichbar
sein (Cloud), nicht nur im Heimnetz. Die App ist dafür schon vorbereitet – man
trägt dann einfach die Cloud-Adresse in den Einstellungen ein.
