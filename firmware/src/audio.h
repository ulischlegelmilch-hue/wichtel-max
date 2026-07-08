#pragma once
// Minimaler Audio-Treiber für den Onboard-ES8311 + Lautsprecher (Waveshare
// ESP32-S3-(Touch-)ePaper-1.54). Fixe Konfig: 16 kHz, 16-bit, MCLK 4,096 MHz.
// Register-Init 1:1 aus Waveshares es8311.c (open + config_sample(16k) + start).
// Ziel: einfache Töne/Melodien über i2s_write. Arduino-Core 2.0.17 -> legacy I2S.
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "board.h"

#if HAS_AUDIO

#define AUDIO_I2S_PORT   I2S_NUM_0
#define AUDIO_RATE       16000
#define AUDIO_MCLK       (AUDIO_RATE * 256)   // 4.096 MHz (mclk_div = 256)

static bool s_audioReady = false;

static void es8311W(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t es8311R(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xEE;
  if (Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return 0xEE;
  return Wire.read();
}

// ES8311 für 16 kHz / 16-bit I2S, Slave, MCLK vom ESP, nur DAC (Lautsprecher).
static void es8311Setup() {
  // --- open() ---
  es8311W(0x44, 0x08); es8311W(0x44, 0x08);   // I2C-Störfestigkeit (2x wie Waveshare)
  es8311W(0x01, 0x30);
  es8311W(0x02, 0x00);
  es8311W(0x03, 0x10);
  es8311W(0x16, 0x24);
  es8311W(0x04, 0x10);
  es8311W(0x05, 0x00);
  es8311W(0x0B, 0x00);
  es8311W(0x0C, 0x00);
  es8311W(0x10, 0x1F);
  es8311W(0x11, 0x7F);
  es8311W(0x00, 0x80);   // Slave-Mode (0x80; Master wäre |0x40)

  // --- config_sample() für mclk=4.096MHz, rate=16k ---
  es8311W(0x02, 0x00);   // pre_div=1, pre_multi=1
  es8311W(0x05, 0x00);   // adc_div=1, dac_div=1
  es8311W(0x03, 0x10);   // adc_osr=0x10
  es8311W(0x04, 0x20);   // dac_osr=0x20
  es8311W(0x07, 0x00);   // lrck_h
  es8311W(0x08, 0xFF);   // lrck_l
  es8311W(0x06, 0x03);   // bclk_div=4 -> (4-1)=3

  // --- Datenformat DAC/ADC: I2S, 16-bit ---
  es8311W(0x09, 0x0C);   // SDPIN: I2S, 16-bit
  es8311W(0x0A, 0x0C);   // SDPOUT: I2S, 16-bit

  // --- start() (DAC / Lautsprecher, use_mclk) ---
  es8311W(0x00, 0x80);   // reset/csm: slave
  es8311W(0x01, 0x3F);   // clk manager: use_mclk
  es8311W(0x17, 0xBF);   // ADC volume (egal, nur DAC genutzt)
  es8311W(0x0E, 0x02);
  es8311W(0x12, 0x00);   // DAC power up
  es8311W(0x14, 0x1A);
  es8311W(0x0D, 0x01);   // power up
  es8311W(0x15, 0x40);
  es8311W(0x37, 0x08);   // DAC ramprate
  es8311W(0x45, 0x00);

  es8311W(0x32, 0xFF);   // DAC-Lautstärke MAX (Test)
  es8311W(0x31, 0x00);   // DAC unmute
}

static void audioInit() {
  if (s_audioReady) return;
#if HAS_I2C
  Wire.begin(I2C_SDA, I2C_SCL);
#endif
  pinMode(AUDIO_PWR, OUTPUT); digitalWrite(AUDIO_PWR, HIGH);   // Audio-Versorgung an
  delay(10);

  // I2S: Master TX, liefert MCLK für den Codec.
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = AUDIO_MCLK;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // MCLK = 256 x Fs sauber ausgeben
  esp_err_t ir = i2s_driver_install(AUDIO_I2S_PORT, &cfg, 0, NULL);

  i2s_pin_config_t pins = {};
  pins.mck_io_num  = I2S_MCLK;
  pins.bck_io_num  = I2S_BCLK;
  pins.ws_io_num   = I2S_WS;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(AUDIO_I2S_PORT, &pins);
  i2s_zero_dma_buffer(AUDIO_I2S_PORT);

  delay(10);
  es8311Setup();                                   // Codec konfigurieren (MCLK läuft schon)

  pinMode(AUDIO_PA, OUTPUT); digitalWrite(AUDIO_PA, HIGH);     // Endstufe an
  s_audioReady = true;

  // --- Diagnose ---
  Serial.printf("[AUDIO] i2s_install=%d  ES8311 ID: FD=%02X FE=%02X FF(ver)=%02X  REG32(vol)=%02X REG31(mute)=%02X\n",
    (int)ir, es8311R(0xFD), es8311R(0xFE), es8311R(0xFF), es8311R(0x32), es8311R(0x31));
  Serial.printf("[AUDIO] pins: PWR=%d PA=%d MCLK=%d BCLK=%d WS=%d DOUT=%d addr=0x%02X\n",
    AUDIO_PWR, AUDIO_PA, I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, ES8311_ADDR);
}

static void audioOff() {
  if (!s_audioReady) return;
  digitalWrite(AUDIO_PA, LOW);
  i2s_driver_uninstall(AUDIO_I2S_PORT);
  digitalWrite(AUDIO_PWR, LOW);
  s_audioReady = false;
}

// Einen Ton (Sinus) der Frequenz freq für ms Millisekunden abspielen.
static void audioTone(int freq, int ms, float vol = 0.6f) {
  if (!s_audioReady) audioInit();
  const int N = 256;
  int16_t buf[N];
  int total = (int)((long)AUDIO_RATE * ms / 1000);
  float phase = 0, step = 2.0f * PI * freq / AUDIO_RATE;
  int amp = (int)(30000 * vol);
  size_t wr;
  int done = 0; bool first = true;
  while (done < total) {
    int chunk = (total - done > N) ? N : (total - done);
    for (int i = 0; i < chunk; i++) {
      buf[i] = (int16_t)(sinf(phase) * amp);
      phase += step; if (phase > 2 * PI) phase -= 2 * PI;
    }
    i2s_write(AUDIO_I2S_PORT, buf, chunk * sizeof(int16_t), &wr, portMAX_DELAY);
    if (first) { Serial.printf("[AUDIO] i2s_write chunk=%d wrote=%u\n", chunk * 2, (unsigned)wr); first = false; }
    done += chunk;
  }
}

// Lauter Selbsttest-Ton beim Einschalten.
static void audioSelfTest() {
  audioInit();
  digitalWrite(AUDIO_PWR, HIGH); digitalWrite(AUDIO_PA, HIGH);
  Serial.println("[AUDIO] Selbsttest: 1 kHz, 2 s, laut");
  audioTone(1000, 2000, 0.95f);
  audioOff();
}

static void audioSilence(int ms) {
  if (!s_audioReady) return;
  const int N = 256; int16_t buf[N] = {0};
  int total = (int)((long)AUDIO_RATE * ms / 1000); size_t wr; int done = 0;
  while (done < total) { int c = (total-done>N)?N:(total-done); i2s_write(AUDIO_I2S_PORT, buf, c*sizeof(int16_t), &wr, portMAX_DELAY); done += c; }
}

// Kleine Melodien für Ereignisse.
static void audioMessage() { audioInit(); audioTone(880,120); audioTone(1175,180); audioOff(); }   // neue Nachricht
static void audioTask()    { audioInit(); audioTone(660,120); audioTone(660,120); audioTone(990,200); audioOff(); } // neue Aufgabe
static void audioDone()    { audioInit(); audioTone(784,110); audioTone(988,110); audioTone(1319,220); audioOff(); } // erledigt
static void audioClick()   { audioInit(); audioTone(1500,25,0.4f); audioOff(); }                    // Knopf

#endif // HAS_AUDIO
