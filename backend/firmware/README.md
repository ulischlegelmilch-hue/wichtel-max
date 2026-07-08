# OTA-Firmware

Hier liegt die Firmware, die das Wichtel-Gerät per WLAN automatisch nachlädt.

- `firmware.bin` – die aktuelle Firmware (wird beim Veröffentlichen hierher kopiert)
- `manifest.json` – enthält die `version`. Das Gerät lädt nur eine .bin, deren
  Version größer ist als seine eigene (`FW_VERSION` in `firmware/src/main.cpp`).

## Neue Version veröffentlichen
1. In `firmware/src/main.cpp` `#define FW_VERSION` hochzählen und bauen (`pio run`).
2. Vom Projektordner `backend/`:  `node publish-firmware.mjs`
   (nimmt die zuletzt gebaute Waveshare-.bin und erhöht die Version).
3. Beim nächsten Aufwachen (oder Tastendruck) holt sich das Gerät das Update.
