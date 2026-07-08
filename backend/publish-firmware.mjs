#!/usr/bin/env node
// Firmware fürs OTA-Fernupdate veröffentlichen.
//
//   node publish-firmware.mjs [pfad/zur/firmware.bin] [version]
//
// Ohne Pfad wird die zuletzt gebaute Waveshare-Firmware genommen. Ohne Version
// wird die aktuelle Version in firmware/manifest.json um 1 erhöht.
// WICHTIG: In der Firmware muss FW_VERSION zur hier gesetzten Version passen
// (main.cpp, #define FW_VERSION) – sonst lädt das Gerät ewig dieselbe .bin neu.

import { fileURLToPath } from "url";
import { dirname, join } from "path";
import { copyFileSync, existsSync, readFileSync, writeFileSync, mkdirSync, statSync } from "fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const FW_DIR = join(__dirname, "firmware");
const FW_BIN = join(FW_DIR, "firmware.bin");
const FW_MANIFEST = join(FW_DIR, "manifest.json");

const DEFAULT_BIN = join(__dirname, "..", "firmware", ".pio", "build",
                         "waveshare-esp32s3-154", "firmware.bin");

const srcArg = process.argv[2];
const verArg = process.argv[3];
const src = srcArg || DEFAULT_BIN;

if (!existsSync(src)) {
  console.error("Firmware-Datei nicht gefunden:", src);
  console.error("Erst bauen (pio run) oder Pfad angeben.");
  process.exit(1);
}

mkdirSync(FW_DIR, { recursive: true });

let version;
if (verArg) {
  version = Number(verArg);
} else {
  let cur = 0;
  if (existsSync(FW_MANIFEST)) {
    try { cur = Number(JSON.parse(readFileSync(FW_MANIFEST, "utf8")).version) || 0; } catch { /* egal */ }
  }
  version = cur + 1;
}

copyFileSync(src, FW_BIN);
writeFileSync(FW_MANIFEST, JSON.stringify({ version, published: new Date().toISOString() }, null, 2));

console.log(`Firmware veröffentlicht: Version ${version}, ${statSync(FW_BIN).size} Bytes`);
console.log(`Quelle: ${src}`);
console.log(`Denk dran: in firmware/src/main.cpp muss #define FW_VERSION ${version} stehen.`);
