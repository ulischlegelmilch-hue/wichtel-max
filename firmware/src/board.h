#pragma once
// Wichtel Max – Board-Auswahl über PlatformIO build_flags (siehe platformio.ini):
//   -D BOARD_WAVESHARE_154  Waveshare ESP32-S3-ePaper-1.54 (200x200, SSD1681)  [Standard]
//   -D BOARD_CROWPANEL_42   Elecrow CrowPanel ESP32-S3 4.2" (400x300, SSD1683)
//
// Pins der Waveshare-Platine 1:1 aus dem offiziellen Waveshare-Demo
// (ESP32-S3-ePaper-1.54, 12_RTC_Sleep_Test/main/user_config.h) übernommen.

#if defined(BOARD_WAVESHARE_154)
  #define BOARD_NAME  "Waveshare 1.54\" 200x200"
  // --- E-Paper SPI + Steuerleitungen ---
  #define EPD_SCK    12
  #define EPD_MOSI   13
  #define EPD_CS     11
  #define EPD_DC     10
  #define EPD_RST     9
  #define EPD_BUSY    8
  // --- E-Paper-Stromversorgung (Load-Switch, INVERTIERT ggü. CrowPanel!) ---
  //     LOW = Panel an, HIGH = Panel stromlos (Bild bleibt stehen)
  #define EPD_PWR     6
  #define EPD_PWR_ON_LEVEL   LOW
  #define EPD_PWR_OFF_LEVEL  HIGH
  // --- Soft-Power-Latch: HIGH hält das Board im Akkubetrieb am Leben.
  //     MUSS über den Tiefschlaf gehalten werden, sonst schaltet sich das
  //     Gerät am Akku komplett aus. (Am USB egal – USB versorgt weiter.) ---
  #define HAS_POWER_LATCH 1
  #define VBAT_PWR   17
  // --- Taster ---
  #define BTN_WAKE    0   // BOOT-Taster -> "Weiter" / weckt via ext1
  #define BTN_POWER  18   // PWR-Taster  -> "Zurück" (lang = Aus) / weckt via ext1
  #define HAS_BACK_BUTTON 1   // zweite Taste (PWR) für "Zurück" vorhanden
  #define EPD_ROTATION 0
  // --- I2C (Audio, RTC, Sensor, Touch) ---
  #define I2C_SDA    47
  #define I2C_SCL    48
  #define HAS_I2C 1
  // I2C-Adressen (per Scan am echten Board bestaetigt):
  #define RTC_ADDR      0x51    // PCF85063 Echtzeituhr
  #define SHTC3_ADDR    0x70    // Temperatur-/Feuchtesensor (Bonus-Feature)
  // Hinweis: kapazitiver Touch an diesem Board NICHT ansprechbar
  // (2026-07-05 gruendlich verifiziert) -> Bedienung nur ueber die 2 Knoepfe.
  // --- Onboard-Audio (ES8311 + Lautsprecher am MX1.25-Header) ---
  //     Pins verifiziert aus Waveshares codec_board board_cfg.h ("S3_ePaper_1_54").
  #define HAS_AUDIO   1
  #define AUDIO_PWR   42        // Audio-Stromversorgung (HIGH = an)
  #define ES8311_ADDR 0x18      // I2C-Adresse des Codecs
  #define I2S_MCLK    14
  #define I2S_BCLK    15
  #define I2S_WS      38
  #define I2S_DOUT    45        // ESP -> Codec (Wiedergabe)
  #define I2S_DIN     16        // Codec -> ESP (Mikrofon, hier ungenutzt)
  #define AUDIO_PA    46        // Endstufen-Enable (HIGH = an)
  // --- Batteriemessung (ADC1_CH3 = GPIO4, 2:1-Spannungsteiler) ---
  #define HAS_BATTERY   1
  #define BATT_ADC_PIN  4
  #define BATT_DIVIDER  2.0f

#elif defined(BOARD_CROWPANEL_42)
  #define BOARD_NAME  "CrowPanel 4.2\" 400x300"
  #define EPD_SCK    12
  #define EPD_MOSI   11
  #define EPD_CS     45
  #define EPD_DC     46
  #define EPD_RST    47
  #define EPD_BUSY   48
  #define EPD_PWR     7
  #define EPD_PWR_ON_LEVEL   HIGH   // CrowPanel: HIGH = Panel an
  #define EPD_PWR_OFF_LEVEL  LOW
  #define HAS_POWER_LATCH 0
  #define BTN_WAKE    2             // MENU-Taster weckt (ext0)
  #define HAS_BACK_BUTTON 0         // nur eine Taste: kurz = Weiter, lang = neueste
  #define EPD_ROTATION 0
  #define HAS_I2C 0
  #define HAS_AUDIO 0               // CrowPanel: kein ES8311-Onboard-Audio hinterlegt
  #define HAS_BATTERY 0

#else
  #error "Kein Board gewaehlt: -D BOARD_WAVESHARE_154 oder -D BOARD_CROWPANEL_42 in platformio.ini setzen"
#endif
