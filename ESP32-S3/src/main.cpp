// // Test program for a 240x320 ST7789 TFT display and BMP280 temperature sensor

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

// ---------- Pins ----------
#define PIN_SDA  8
#define PIN_SCL  9
#define TFT_CS   4
#define TFT_DC   5
#define TFT_RST  6
#define TFT_SCK  12
#define TFT_MOSI 11

// Try SPI_MODE3 if the screen stays blank with MODE0
#define TFT_MODE SPI_MODE0
static const SPISettings tftSpi(10000000, MSBFIRST, TFT_MODE);

#define COL_BLACK 0x0000
#define COL_WHITE 0xFFFF
#define COL_RED   0xF800
#define COL_GREEN 0x07E0
#define COL_BLUE  0x001F

// ---------- ST7789 raw driver ----------
void tftCmd(uint8_t cmd) {
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  SPI.transfer(cmd);
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

void tftData(const uint8_t *d, size_t n) {
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  SPI.writeBytes(d, n);
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

void tftCmdData(uint8_t cmd, std::initializer_list<uint8_t> data) {
  tftCmd(cmd);
  if (data.size()) tftData(data.begin(), data.size());
}

void tftInit() {
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);

  digitalWrite(TFT_RST, HIGH); delay(50);
  digitalWrite(TFT_RST, LOW);  delay(50);
  digitalWrite(TFT_RST, HIGH); delay(150);

  tftCmd(0x01); delay(150);          // software reset
  tftCmd(0x11); delay(120);          // sleep out
  tftCmdData(0x3A, {0x55});          // 16-bit colour
  tftCmdData(0x36, {0x00});          // portrait orientation
  tftCmd(0x21);                      // inversion on (delete if colours look inverted)
  tftCmd(0x13); delay(10);           // normal display mode
  tftCmd(0x29); delay(120);          // display on
}

void tftSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  tftCmdData(0x2A, {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1});
  tftCmdData(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1});
  tftCmd(0x2C);
}

void tftFill(uint16_t colour) {
  static uint8_t buf[512];
  for (int i = 0; i < 512; i += 2) {
    buf[i] = colour >> 8;
    buf[i + 1] = colour & 0xFF;
  }
  tftSetWindow(0, 0, 239, 319);
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (int i = 0; i < 300; i++) SPI.writeBytes(buf, 512);   // 240*320*2 bytes
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

// ---------- Tiny 5x7 font ----------
// Each glyph is 5 columns; bit 0 is the top row.
struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
  {'0', {0x3E,0x51,0x49,0x45,0x3E}}, {'1', {0x00,0x42,0x7F,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4B,0x31}},
  {'4', {0x18,0x14,0x12,0x7F,0x10}}, {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3C,0x4A,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1E}},
  {'.', {0x00,0x60,0x60,0x00,0x00}}, {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'*', {0x00,0x06,0x09,0x09,0x06}},                                   // degree sign
  {'C', {0x3E,0x41,0x41,0x41,0x22}}, {'T', {0x01,0x01,0x7F,0x01,0x01}},
  {'E', {0x7F,0x49,0x49,0x49,0x41}}, {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
  {'P', {0x7F,0x09,0x09,0x09,0x06}}, {'R', {0x7F,0x09,0x19,0x29,0x46}},
};

const uint8_t *findGlyph(char c) {
  for (const Glyph &g : FONT) if (g.c == c) return g.col;
  return nullptr;   // unknown char or space draws as blank
}

// Draws one character in a (6*scale) x (8*scale) cell with its own background
void drawChar(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg, uint16_t bg) {
  static uint8_t buf[6 * 8 * 5 * 5 * 2];      // supports scale up to 5
  const uint8_t *g = findGlyph(c);
  const int w = 6 * scale, h = 8 * scale;

  for (int py = 0; py < h; py++) {
    for (int px = 0; px < w; px++) {
      int col = px / scale, row = py / scale;
      bool on = g && col < 5 && row < 7 && (g[col] >> row) & 1;
      uint16_t colour = on ? fg : bg;
      int i = (py * w + px) * 2;
      buf[i] = colour >> 8;
      buf[i + 1] = colour & 0xFF;
    }
  }
  tftSetWindow(x, y, x + w - 1, y + h - 1);
  tftData(buf, w * h * 2);
}

void drawString(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg) {
  while (*s) {
    drawChar(x, y, *s++, scale, fg, bg);
    x += 6 * scale;
  }
}

// ---------- BMP280 (raw I2C) ----------
uint8_t bmpAddr = 0;
uint16_t dig_T1, dig_P1;
int16_t dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

bool i2cRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(bmpAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(bmpAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool bmpInit() {
  for (uint8_t a : {0x76, 0x77}) {
    bmpAddr = a;
    uint8_t id;
    if (i2cRead(0xD0, &id, 1)) {
      Serial.printf("I2C device at 0x%02X, chip ID 0x%02X (0x58 = BMP280)\n", a, id);
      if (id != 0x58 && id != 0x60) return false;

      uint8_t c[24];
      if (!i2cRead(0x88, c, 24)) return false;
      auto u16 = [&](int i) { return (uint16_t)(c[i] | (c[i + 1] << 8)); };
      auto s16 = [&](int i) { return (int16_t)u16(i); };
      dig_T1 = u16(0);  dig_T2 = s16(2);  dig_T3 = s16(4);
      dig_P1 = u16(6);  dig_P2 = s16(8);  dig_P3 = s16(10); dig_P4 = s16(12); dig_P5 = s16(14);
      dig_P6 = s16(16); dig_P7 = s16(18); dig_P8 = s16(20); dig_P9 = s16(22);

      Wire.beginTransmission(bmpAddr); Wire.write(0xF5); Wire.write(0xA0); Wire.endTransmission();
      Wire.beginTransmission(bmpAddr); Wire.write(0xF4); Wire.write(0x27); Wire.endTransmission();
      return true;
    }
  }
  return false;
}

bool bmpReadTemp(float &tempC) {
  uint8_t d[3];
  if (!i2cRead(0xFA, d, 3)) return false;
  int32_t adcT = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  double v1 = (adcT / 16384.0 - dig_T1 / 1024.0) * dig_T2;
  double v2 = (adcT / 131072.0 - dig_T1 / 8192.0);
  v2 = v2 * v2 * dig_T3;
  tempC = (v1 + v2) / 5120.0;
  return true;
}

// ---------- Main ----------
bool bmpOk = false;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- Temperature display test ---");

  Wire.begin(PIN_SDA, PIN_SCL);
  bmpOk = bmpInit();
  Serial.println(bmpOk ? "BMP280 OK" : "BMP280 NOT FOUND");
  delay(100);

  SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);   // CS is driven manually
  tftInit();
  Serial.println("TFT init done");

  // Colour test so you can see the panel working
  const uint16_t cols[] = {COL_RED, COL_GREEN, COL_BLUE};
  for (uint16_t c : cols) { tftFill(c); delay(500); }

  tftFill(COL_BLACK);
  drawString(20, 40, "TEMP", 5, COL_WHITE, COL_BLACK);
}

void loop() {
  float t = 0;
  char line[16];

  if (bmpOk && bmpReadTemp(t)) {
    snprintf(line, sizeof(line), "%5.1f*C", t);
    Serial.printf("Temperature: %.2f C\n", t);
  } else {
    snprintf(line, sizeof(line), "ERR");
    Serial.println("Sensor read failed");
  }
  drawString(15, 130, line, 5, COL_GREEN, COL_BLACK);
  delay(1000);
}