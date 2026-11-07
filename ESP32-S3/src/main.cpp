// =====================================================================
// EMBEDDED WEATHER STATION - ESP32-S3 + BMP280 + ST7789 TFT + KY-023
// =====================================================================
// What this program does:
//   - Reads pressure and temperature from a BMP280 sensor over I2C
//   - Draws a menu and data pages on an ST7789 TFT over SPI
//   - Uses the KY-023 joystick to scroll the menu and open/close pages
//   - Uses millis() timers instead of delay() in loop(), so the
//     navigation never blocks (non-blocking design)
//
// No display or sensor libraries are used: the ST7789 and BMP280 are
// driven directly with their command/register sets.
// =====================================================================

#include <Arduino.h>   // Core Arduino functions (pinMode, analogRead, millis...)
#include <Wire.h>      // I2C bus, used to talk to the BMP280
#include <SPI.h>       // SPI bus, used to talk to the TFT display


// =====================================================================
// SECTION 1: PIN DEFINITIONS
// =====================================================================
// BMP280 sensor (I2C)
#define PIN_SDA  8     // I2C data line
#define PIN_SCL  9     // I2C clock line

// TFT display (SPI)
#define TFT_CS   4     // Chip select: LOW = display is listening
#define TFT_DC   5     // Data/Command: LOW = command byte, HIGH = data byte
#define TFT_RST  6     // Hardware reset line for the display
#define TFT_SCK  12    // SPI clock
#define TFT_MOSI 11    // SPI data from ESP32 to display

// KY-023 joystick
// (If you had to swap the X and Y pins to make your stick work, keep
//  your swapped values here.)
#define JOY_X    2     // Horizontal axis, analog (ADC1), unused for now
#define JOY_Y    1     // Vertical axis, analog (ADC1), scrolls the menu
#define JOY_SW   7     // Push button, active LOW (pressed = LOW)


// =====================================================================
// SECTION 2: CONFIGURATION SETTINGS
// =====================================================================
// SPI mode and speed for the display. 10 MHz is safe on breadboard wires.
// Keep whichever SPI mode worked for you (MODE0 or MODE3).
#define TFT_MODE SPI_MODE0
static const SPISettings tftSpi(10000000, MSBFIRST, TFT_MODE);

// Set to 1 if pushing the stick up moves the menu highlight down
#define JOY_INVERT 0

// 16-bit RGB565 colour values used for drawing
#define COL_BLACK  0x0000
#define COL_WHITE  0xFFFF
#define COL_RED    0xF800
#define COL_GREEN  0x07E0
#define COL_BLUE   0x001F
#define COL_CYAN   0x07FF
#define COL_YELLOW 0xFFE0


// =====================================================================
// SECTION 3: ST7789 DISPLAY DRIVER (LOW-LEVEL SPI)
// =====================================================================

// Send a single command byte to the display (DC pin LOW)
void tftCmd(uint8_t cmd) {
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, LOW);     // LOW tells the display this byte is a command
  digitalWrite(TFT_CS, LOW);     // Select the display
  SPI.transfer(cmd);
  digitalWrite(TFT_CS, HIGH);    // Deselect the display
  SPI.endTransaction();
}

// Send a block of data bytes to the display (DC pin HIGH)
void tftData(const uint8_t *d, size_t n) {
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, HIGH);    // HIGH tells the display these bytes are data
  digitalWrite(TFT_CS, LOW);
  SPI.writeBytes(d, n);
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

// Send a command followed by its parameter bytes
void tftCmdData(uint8_t cmd, std::initializer_list<uint8_t> data) {
  tftCmd(cmd);
  if (data.size()) tftData(data.begin(), data.size());
}

// Reset the display and configure it for 16-bit colour, portrait mode
void tftInit() {
  // Set up the control pins; CS idles HIGH (display not selected)
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);

  // Hardware reset pulse
  digitalWrite(TFT_RST, HIGH); delay(50);
  digitalWrite(TFT_RST, LOW);  delay(50);
  digitalWrite(TFT_RST, HIGH); delay(150);

  tftCmd(0x01); delay(150);          // Software reset
  tftCmd(0x11); delay(120);          // Sleep out (wake the panel)
  tftCmdData(0x3A, {0x55});          // Pixel format: 16 bits per pixel
  tftCmdData(0x36, {0x00});          // Memory access: portrait orientation
  tftCmd(0x21);                      // Inversion on (delete if colours look inverted)
  tftCmd(0x13); delay(10);           // Normal display mode
  tftCmd(0x29); delay(120);          // Display on
}

// Choose the rectangle of the screen that the next pixel data will fill
void tftSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  tftCmdData(0x2A, {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1}); // Column range
  tftCmdData(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1}); // Row range
  tftCmd(0x2C);                      // Start writing pixels to memory
}

// Fill the whole 240x320 screen with one colour
void tftFill(uint16_t colour) {
  // Build a 512-byte strip of the colour, then send it 300 times
  static uint8_t buf[512];
  for (int i = 0; i < 512; i += 2) {
    buf[i] = colour >> 8;            // High byte first
    buf[i + 1] = colour & 0xFF;
  }
  tftSetWindow(0, 0, 239, 319);
  SPI.beginTransaction(tftSpi);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (int i = 0; i < 300; i++) SPI.writeBytes(buf, 512);   // 240*320*2 bytes total
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}


// =====================================================================
// SECTION 4: TEXT RENDERING (TINY 5x7 BITMAP FONT)
// =====================================================================
// Each glyph is 5 columns wide. Every byte is one column and bit 0
// is the top row of the character.
struct Glyph { char c; uint8_t col[5]; };
static const Glyph FONT[] = {
  {'0', {0x3E,0x51,0x49,0x45,0x3E}}, {'1', {0x00,0x42,0x7F,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4B,0x31}},
  {'4', {0x18,0x14,0x12,0x7F,0x10}}, {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3C,0x4A,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1E}},
  {'.', {0x00,0x60,0x60,0x00,0x00}}, {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'*', {0x00,0x06,0x09,0x09,0x06}},   // Used as the degree sign
  {'=', {0x14,0x14,0x14,0x14,0x14}}, {'>', {0x00,0x41,0x22,0x14,0x08}},
  {'A', {0x7E,0x11,0x11,0x11,0x7E}}, {'B', {0x7F,0x49,0x49,0x49,0x36}},
  {'C', {0x3E,0x41,0x41,0x41,0x22}}, {'E', {0x7F,0x49,0x49,0x49,0x41}},
  {'H', {0x7F,0x08,0x08,0x08,0x7F}}, {'I', {0x00,0x41,0x7F,0x41,0x00}},
  {'K', {0x7F,0x08,0x14,0x22,0x41}}, {'L', {0x7F,0x40,0x40,0x40,0x40}},
  {'M', {0x7F,0x02,0x0C,0x02,0x7F}}, {'N', {0x7F,0x04,0x08,0x10,0x7F}},
  {'O', {0x3E,0x41,0x41,0x41,0x3E}}, {'P', {0x7F,0x09,0x09,0x09,0x06}},
  {'R', {0x7F,0x09,0x19,0x29,0x46}}, {'S', {0x46,0x49,0x49,0x49,0x31}},
  {'T', {0x01,0x01,0x7F,0x01,0x01}}, {'U', {0x3F,0x40,0x40,0x40,0x3F}},
};

// Look up a character in the font table (returns nullptr for space/unknown)
const uint8_t *findGlyph(char c) {
  for (const Glyph &g : FONT) if (g.c == c) return g.col;
  return nullptr;
}

// Draw one character in a (6*scale) x (8*scale) cell, with its own background colour
void drawChar(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t fg, uint16_t bg) {
  static uint8_t buf[6 * 8 * 8 * 8 * 2];      // Pixel buffer, supports scale up to 8
  if (scale > 8) scale = 8;
  const uint8_t *g = findGlyph(c);
  const int w = 6 * scale, h = 8 * scale;

  // Build the character pixel by pixel, enlarging each font dot by 'scale'
  for (int py = 0; py < h; py++) {
    for (int px = 0; px < w; px++) {
      int col = px / scale, row = py / scale;
      bool on = g && col < 5 && row < 7 && ((g[col] >> row) & 1);
      uint16_t colour = on ? fg : bg;
      int i = (py * w + px) * 2;
      buf[i] = colour >> 8;
      buf[i + 1] = colour & 0xFF;
    }
  }
  tftSetWindow(x, y, x + w - 1, y + h - 1);   // Aim at the character's cell
  tftData(buf, w * h * 2);                    // Send the pixels
}

// Draw a whole string by drawing each character one cell to the right
void drawString(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint16_t fg, uint16_t bg) {
  while (*s) {
    drawChar(x, y, *s++, scale, fg, bg);
    x += 6 * scale;
  }
}


// =====================================================================
// SECTION 5: BMP280 SENSOR DRIVER (LOW-LEVEL I2C)
// =====================================================================
// Calibration constants stored inside every BMP280 at the factory.
// They are used in the datasheet formulas to turn raw readings into
// real temperature and pressure values.
uint8_t bmpAddr = 0;                         // I2C address found at startup (0x76 or 0x77)
uint16_t dig_T1, dig_P1;
int16_t dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

// Read 'n' bytes starting at register 'reg' from the sensor
bool i2cRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(bmpAddr);
  Wire.write(reg);                           // Tell the sensor which register we want
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(bmpAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Find the sensor, check its ID, load calibration data and start measuring
bool bmpInit() {
  for (uint8_t a : {0x76, 0x77}) {           // Try both possible addresses
    bmpAddr = a;
    uint8_t id;
    if (i2cRead(0xD0, &id, 1)) {             // Register 0xD0 holds the chip ID
      Serial.printf("I2C device at 0x%02X, chip ID 0x%02X (0x58 = BMP280)\n", a, id);
      if (id != 0x58 && id != 0x60) return false;   // 0x58 = BMP280, 0x60 = BME280

      // Read the 24 calibration bytes (registers 0x88 to 0x9F)
      uint8_t c[24];
      if (!i2cRead(0x88, c, 24)) return false;
      auto u16 = [&](int i) { return (uint16_t)(c[i] | (c[i + 1] << 8)); };
      auto s16 = [&](int i) { return (int16_t)u16(i); };
      dig_T1 = u16(0);  dig_T2 = s16(2);  dig_T3 = s16(4);
      dig_P1 = u16(6);  dig_P2 = s16(8);  dig_P3 = s16(10); dig_P4 = s16(12); dig_P5 = s16(14);
      dig_P6 = s16(16); dig_P7 = s16(18); dig_P8 = s16(20); dig_P9 = s16(22);

      // Configure the sensor
      Wire.beginTransmission(bmpAddr); Wire.write(0xF5); Wire.write(0xA0); Wire.endTransmission(); // Config: filter and standby time
      Wire.beginTransmission(bmpAddr); Wire.write(0xF4); Wire.write(0x27); Wire.endTransmission(); // Normal mode, x1 oversampling
      return true;
    }
  }
  return false;                              // No sensor found at either address
}

// Read raw values and convert them with the datasheet compensation formulas
bool bmpRead(float &tempC, float &pressHpa) {
  uint8_t d[6];
  if (!i2cRead(0xF7, d, 6)) return false;    // 3 pressure bytes then 3 temperature bytes
  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);

  // Temperature compensation (also produces tFine, which pressure needs)
  double v1 = (adcT / 16384.0 - dig_T1 / 1024.0) * dig_T2;
  double v2 = (adcT / 131072.0 - dig_T1 / 8192.0);
  v2 = v2 * v2 * dig_T3;
  double tFine = v1 + v2;
  tempC = tFine / 5120.0;

  // Pressure compensation (result is in Pa)
  v1 = tFine / 2.0 - 64000.0;
  v2 = v1 * v1 * dig_P6 / 32768.0;
  v2 = v2 + v1 * dig_P5 * 2.0;
  v2 = v2 / 4.0 + dig_P4 * 65536.0;
  v1 = (dig_P3 * v1 * v1 / 524288.0 + dig_P2 * v1) / 524288.0;
  v1 = (1.0 + v1 / 32768.0) * dig_P1;
  if (v1 == 0) return false;                 // Avoid dividing by zero
  double p = 1048576.0 - adcP;
  p = (p - v2 / 4096.0) * 6250.0 / v1;
  v1 = dig_P9 * p * p / 2147483648.0;
  v2 = p * dig_P8 / 32768.0;
  p = p + (v1 + v2 + dig_P7) / 16.0;
  pressHpa = p / 100.0;                      // Pa -> hPa (1 hPa = 1 mbar)
  return true;
}


// =====================================================================
// SECTION 6: UI STATE AND GLOBAL VARIABLES
// =====================================================================
// The screens the user can be on
enum Screen { SCR_MENU = 0, SCR_HPA, SCR_MBAR, SCR_TEMP };
Screen screen = SCR_MENU;                    // Start on the main menu

uint8_t selected = 0;                        // Highlighted menu item (0 to 2)
const uint8_t NUM_ITEMS = 3;
const char *MENU_ITEMS[NUM_ITEMS] = {"PRESSURE HPA", "PRESSURE MBAR", "TEMPERATURE"};

bool bmpOk = false;                          // True if the sensor was found at startup
bool sensorOk = false;                       // True if the latest reading succeeded
float tempC = 0, pressHpa = 0;               // Latest sensor values

int joyCentre = 2048;                        // Joystick resting value, measured at startup


// =====================================================================
// SECTION 7: SCREEN DRAWING FUNCTIONS
// =====================================================================

// Draw one menu row; the selected row gets a blue highlight bar
void drawMenuItem(uint8_t i) {
  char line[24];
  snprintf(line, sizeof(line), " %-17s", MENU_ITEMS[i]);   // Fixed width so the bar is a clean block
  bool sel = (i == selected);
  drawString(12, 90 + i * 40, line, 2, COL_WHITE, sel ? COL_BLUE : COL_BLACK);
}

// Draw the full main menu
void drawMenu() {
  tftFill(COL_BLACK);
  drawString(72, 20, "MENU", 4, COL_CYAN, COL_BLACK);
  for (uint8_t i = 0; i < NUM_ITEMS; i++) drawMenuItem(i);
  drawString(36, 290, "CLICK = OPEN", 2, COL_WHITE, COL_BLACK);
}

// Draw the parts of a data page that never change (titles and unit labels)
void drawPageStatic() {
  tftFill(COL_BLACK);
  switch (screen) {
    case SCR_HPA:
      drawString(48, 30, "PRESSURE", 3, COL_CYAN, COL_BLACK);
      drawString(84, 190, "HPA", 4, COL_YELLOW, COL_BLACK);
      break;
    case SCR_MBAR:
      drawString(48, 30, "PRESSURE", 3, COL_CYAN, COL_BLACK);
      drawString(48, 190, "MBAR", 6, COL_YELLOW, COL_BLACK);
      break;
    case SCR_TEMP:
      drawString(21, 30, "TEMPERATURE", 3, COL_CYAN, COL_BLACK);
      break;
    default: break;
  }
  drawString(36, 290, "CLICK = BACK", 2, COL_WHITE, COL_BLACK);
}

// Draw the live reading for the current page (called every sensor update)
void drawValue() {
  char buf[16];
  switch (screen) {
    case SCR_HPA:                            // Pressure in hPa, two decimals
      if (sensorOk) snprintf(buf, sizeof(buf), "%7.2f", pressHpa);
      else          snprintf(buf, sizeof(buf), "%7s", "ERR");
      drawString(15, 110, buf, 5, COL_GREEN, COL_BLACK);
      if (sensorOk) {                        // Smaller secondary line showing the same value in Pa
        char pa[20];
        snprintf(pa, sizeof(pa), "%8.0f PA", pressHpa * 100.0f);
        drawString(24, 170, pa, 2, COL_WHITE, COL_BLACK);
      }
      break;
    case SCR_MBAR:                           // 1 hPa = 1 mbar; shown in a large font
      if (sensorOk) snprintf(buf, sizeof(buf), "%6.1f", pressHpa);
      else          snprintf(buf, sizeof(buf), "%6s", "ERR");
      drawString(12, 100, buf, 6, COL_GREEN, COL_BLACK);
      break;
    case SCR_TEMP:                           // Temperature in degrees Celsius
      if (sensorOk) snprintf(buf, sizeof(buf), "%5.1f*C", tempC);
      else          snprintf(buf, sizeof(buf), "%7s", "ERR");
      drawString(15, 130, buf, 5, COL_GREEN, COL_BLACK);
      break;
    default: break;
  }
}


// =====================================================================
// SECTION 8: JOYSTICK HANDLING (NON-BLOCKING)
// =====================================================================

// Convert the Y axis reading into a direction: -1 = up, +1 = down, 0 = centred
int8_t readJoyDir() {
  int y = analogRead(JOY_Y);
  int8_t dir = 0;
  if (y < joyCentre - 800) dir = -1;         // Pushed far enough below centre
  else if (y > joyCentre + 800) dir = 1;     // Pushed far enough above centre
  return JOY_INVERT ? -dir : dir;
}

// Called every loop: moves the menu highlight and handles the button
void handleJoystick(unsigned long now) {
  // ---- Direction: act once per push, re-arm when the stick returns to centre ----
  static int8_t lastDir = 0;
  static unsigned long lastMove = 0;
  int8_t dir = readJoyDir();
  if (dir != 0 && dir != lastDir && now - lastMove > 150) {
    lastMove = now;
    if (screen == SCR_MENU) {                // Scrolling only works on the menu
      uint8_t old = selected;
      selected = (selected + NUM_ITEMS + dir) % NUM_ITEMS;   // Wraps around top/bottom
      drawMenuItem(old);                     // Un-highlight the old row
      drawMenuItem(selected);                // Highlight the new row
      Serial.printf("Menu: selected %u\n", selected);
    }
  }
  lastDir = dir;

  // ---- Button: debounced, acts once on the press edge ----
  static bool lastRaw = false, stable = false;
  static unsigned long lastChange = 0;
  bool raw = (digitalRead(JOY_SW) == LOW);   // Pressed = LOW (internal pull-up)
  if (raw != lastRaw) { lastChange = now; lastRaw = raw; }
  if (now - lastChange > 30 && raw != stable) {   // Must be steady for 30 ms (debounce)
    stable = raw;
    if (stable) {                            // Button has just been pressed
      if (screen == SCR_MENU) {              // On the menu: open the highlighted page
        screen = (Screen)(SCR_HPA + selected);
        Serial.printf("Open page %u\n", selected);
        drawPageStatic();
        drawValue();
      } else {                               // On a page: go back to the menu
        screen = SCR_MENU;
        Serial.println("Back to menu");
        drawMenu();
      }
    }
  }
}


// =====================================================================
// SECTION 9: SETUP (RUNS ONCE AT STARTUP)
// =====================================================================
void setup() {
  // Serial port for debugging
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- Weather station: menu test ---");

  // Joystick: button uses the internal pull-up, ADC set to 12 bits (0-4095)
  pinMode(JOY_SW, INPUT_PULLUP);
  analogReadResolution(12);

  // Calibrate the joystick: average 32 readings to find its resting value
  long sum = 0;
  for (int i = 0; i < 32; i++) { sum += analogRead(JOY_Y); delay(5); }
  joyCentre = sum / 32;
  Serial.printf("Joystick centre = %d\n", joyCentre);

  // Start I2C and look for the BMP280
  Wire.begin(PIN_SDA, PIN_SCL);
  bmpOk = bmpInit();
  Serial.println(bmpOk ? "BMP280 OK" : "BMP280 NOT FOUND");
  delay(100);

  // Start SPI and initialise the display (CS is driven manually, so -1)
  SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);
  tftInit();
  Serial.println("TFT init done");

  // Show the main menu
  drawMenu();
}


// =====================================================================
// SECTION 10: MAIN LOOP (RUNS FOREVER, NEVER BLOCKS)
// =====================================================================
void loop() {
  unsigned long now = millis();              // Current time in milliseconds

  // ---- Sensor refresh every 500 ms (timer based, no delay) ----
  static unsigned long lastSensor = 0;
  if (now - lastSensor >= 500) {
    lastSensor = now;
    sensorOk = bmpOk && bmpRead(tempC, pressHpa);   // Read the BMP280
    if (screen != SCR_MENU) drawValue();            // Update the page being shown
    Serial.printf("P=%.2f hPa  T=%.2f C  | joyY=%d joyX=%d SW=%d\n",
                  pressHpa, tempC, analogRead(JOY_Y), analogRead(JOY_X),
                  digitalRead(JOY_SW) == LOW);      // Debug line for the serial monitor
  }

  // ---- Check the joystick on every pass through the loop ----
  handleJoystick(now);
}
