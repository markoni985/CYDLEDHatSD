/*
  CYD Badge Controller - ESP32-2432S028R
  LANDSCAPE 320x240

  Screen layout (landscape):
  ??????????????????????????????????????????????????  y=0
  ?  Input bar (text + status)                      ?  y=0..30
  ?  [BLE][PRD][CLK]  *Color*  [Mode]              ?  y=31..52
  ??????????????????????????????????????????????????  y=53
  ?  1 2 3 4 5 6 7 8 9 0                           ?  row 0
  ?  Q W E R T Y U I O P                           ?  row 1
  ?  A S D F G H J K L                             ?  row 2
  ?  Z X C V B N M  [DEL]                          ?  row 3
  ?  [SPC............] [CLR] [SND]                  ?  row 4
  ??????????????????????????????????????????????????  y=239

  SD card config.txt - one key=value per line:
    MAC=00:41:20:03:16:05
    TZ=CST6CDT,M3.2.0,M11.1.0
    SSID=TCZ
    PASS=qwertyuiop123

  Libs: TFT_eSPI, XPT2046_Touchscreen, NimBLE-Arduino, SD (ESP32 built-in)

  User_Setup.h for CYD2USB:
    #define ILI9341_DRIVER
    #define TFT_MISO 12  #define TFT_MOSI 13  #define TFT_SCLK 14
    #define TFT_CS   15  #define TFT_DC    2  #define TFT_RST  -1
    #define TFT_BL   21  #define TFT_BACKLIGHT_ON HIGH
    #define SPI_FREQUENCY 40000000
*/

// ================= STRUCTS (must be before includes) =================
typedef struct { int x; int y; bool valid; } TouchPoint;

// Special key IDs
#define K_NONE      0
#define K_BACKSPACE 1
#define K_CLEAR     2
#define K_SEND      3
#define K_COLOR     4
#define K_MODE      5
#define K_SPACE     6
#define K_SCR_BLE   7
#define K_SCR_PRD   8
#define K_SCR_CLK   9

typedef struct {
  int  x, y, w, h;
  char label[6];
  char value;
  int  special;
} Key;

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <NimBLEDevice.h>
#include "mbedtls/aes.h"
#include <WiFi.h>
#include "time.h"
#include <SD.h>
#include <FS.h>

// ================= HARDWARE =================
#define TOUCH_CS_PIN   33
#define TOUCH_IRQ_PIN  36
#define TOUCH_CLK_PIN  25
#define TOUCH_MISO_PIN 39
#define TOUCH_MOSI_PIN 32
#define LCD_BL_PIN     21
#define SD_CS_PIN       5   // CYD SD CS - adjust if different

// Touch calibration for LANDSCAPE rotation=1
// Raw X/Y axes swap in landscape; these may need trimming on your unit
#define TOUCH_X_MIN   300
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN  3700
#define TOUCH_Y_MAX   200

#define SCR_W 320
#define SCR_H 240

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// ================= COLORS =================
#define C_BG      0x0000
#define C_WHITE   0xFFFF
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_GREY    0x4208
#define C_DKGREY  0x2104
#define C_KEYBG   0x2965
#define C_KEYHI   0x5ACB
#define C_SHADOW  0x18C3
#define C_ACCENT  0xFD20   // orange - SEND
#define C_DARKRED 0x8000   // predator glow backing

// ================= CONFIG (from SD) =================
String cfgMAC  = "";
String cfgTZ   = "";
String cfgSSID = "";
String cfgPASS = "";
String cfgNTP  = "pool.ntp.org";   // default if not in file

void loadConfig() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD mount failed - using defaults");
    return;
  }
  File f = SD.open("/config.txt", FILE_READ);
  if (!f) {
    Serial.println("config.txt not found - using defaults");
    return;
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 1) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim(); val.trim();
    if      (key == "MAC")  cfgMAC  = val;
    else if (key == "TZ")   cfgTZ   = val;
    else if (key == "SSID") cfgSSID = val;
    else if (key == "PASS") cfgPASS = val;
    else if (key == "NTP")  cfgNTP  = val;
  }
  f.close();
  Serial.printf("Config: MAC=%s TZ=%s SSID=%s NTP=%s\n",
                cfgMAC.c_str(), cfgTZ.c_str(), cfgSSID.c_str(), cfgNTP.c_str());
}

// (structs moved to top, before includes)

#define MAX_KEYS 64
Key keys[MAX_KEYS];
int numKeys = 0;

// ================= STATE =================
String inputText       = "";
int selectedColorIndex = 0;
int selectedModeIndex  = 1;
int previewOffset      = 0;
String statusMsg       = "READY";

enum ScreenMode { MODE_BLE, MODE_PREDATOR, MODE_CLOCK };
ScreenMode currentMode = MODE_BLE;

const char* modeNames[]  = {"FIX","SCR>","<SCR"};
uint8_t     modeValues[] = {1, 3, 4};

const char* colorNames[]   = {"RED","GRN","BLU","YEL","WHT"};
uint8_t     colorsRGB[][3] = {
  {255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,255,255}
};

// ================= BLE =================
#define CHAR_CMD    "d44bc439-abfd-45a2-b575-925416129600"
#define CHAR_UPLOAD "d44bc439-abfd-45a2-b575-92541612960a"
#define CHAR_NOTIFY "d44bc439-abfd-45a2-b575-925416129601"
#define CHAR_FD02   "0000fd02-0000-1000-8000-00805f9b34fb"

NimBLEClient*              bleClient   = nullptr;
NimBLERemoteCharacteristic *cmdChar    = nullptr;
NimBLERemoteCharacteristic *uploadChar = nullptr;
NimBLERemoteCharacteristic *notifyChar = nullptr;
NimBLERemoteCharacteristic *fd02Char   = nullptr;

struct Resp { uint8_t data[64]; size_t len; };
QueueHandle_t respQueue;

// ================= FONT (5x8 for badge rendering) =================
const uint8_t font5x8[][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
  {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
  {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
  {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
  {0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
  {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
  {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
  {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
  {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
  {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
  {0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
  {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},
  {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
  {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
  {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},
  {0x61,0x51,0x49,0x45,0x43}
};

// ================= KEYBOARD BUILDER (landscape) =================
// Landscape: 320px wide, keyboard starts y=53, ends y=239 ? 186px height
// 5 rows x (36px key + 1px gap) = 185px - fits perfectly
// 10 keys per row ? key width = 32px

void buildKeys() {
  numKeys = 0;

  const int KW  = 32;   // key width
  const int KH  = 36;   // key height
  const int GAP = 1;    // gap between keys
  const int Y0  = 53;   // top of keyboard

  auto addKey = [&](int col, int row, int spanCols, const char* lbl, char val, int spec) {
    Key k;
    k.x = col * (KW + GAP);
    k.y = Y0 + row * (KH + GAP);
    k.w = spanCols * (KW + GAP) - GAP;
    k.h = KH;
    strncpy(k.label, lbl, 5); k.label[5] = 0;
    k.value   = val;
    k.special = spec;
    keys[numKeys++] = k;
  };

  // Row 0: 1 2 3 4 5 6 7 8 9 0  (10 keys x 32px = 320px)
  const char* nums = "1234567890";
  for (int i = 0; i < 10; i++) {
    char lbl[2] = {nums[i], 0};
    addKey(i, 0, 1, lbl, nums[i], K_NONE);
  }

  // Row 1: Q W E R T Y U I O P
  const char* row1 = "QWERTYUIOP";
  for (int i = 0; i < 10; i++) {
    char lbl[2] = {row1[i], 0};
    addKey(i, 1, 1, lbl, row1[i], K_NONE);
  }

  // Row 2: A S D F G H J K L  (9 keys, centred - offset ? key)
  const char* row2 = "ASDFGHJKL";
  for (int i = 0; i < 9; i++) {
    Key k;
    k.x = (KW + GAP) / 2 + i * (KW + GAP);
    k.y = Y0 + 2 * (KH + GAP);
    k.w = KW;
    k.h = KH;
    k.label[0] = row2[i]; k.label[1] = 0;
    k.value   = row2[i];
    k.special = K_NONE;
    keys[numKeys++] = k;
  }

  // Row 3: Z X C V B N M (7 keys) + DEL (right-fills to edge)
  const char* row3 = "ZXCVBNM";
  for (int i = 0; i < 7; i++) {
    char lbl[2] = {row3[i], 0};
    addKey(i, 3, 1, lbl, row3[i], K_NONE);
  }
  {
    Key k;
    k.x = 7 * (KW + GAP);
    k.y = Y0 + 3 * (KH + GAP);
    k.w = SCR_W - 7 * (KW + GAP);
    k.h = KH;
    strncpy(k.label, "DEL", 5);
    k.value = 0; k.special = K_BACKSPACE;
    keys[numKeys++] = k;
  }

  // Row 4: SPC (6 cols) | CLR (2 cols) | SND (2 cols)
  int row4y = Y0 + 4 * (KH + GAP);
  {
    Key k; k.x=0; k.y=row4y; k.w=6*(KW+GAP)-GAP; k.h=KH;
    strncpy(k.label,"SPC",5); k.value=' '; k.special=K_SPACE;
    keys[numKeys++]=k;
  }
  {
    Key k; k.x=6*(KW+GAP); k.y=row4y; k.w=2*(KW+GAP)-GAP; k.h=KH;
    strncpy(k.label,"CLR",5); k.value=0; k.special=K_CLEAR;
    keys[numKeys++]=k;
  }
  {
    Key k; k.x=8*(KW+GAP); k.y=row4y; k.w=SCR_W-8*(KW+GAP); k.h=KH;
    strncpy(k.label,"SND",5); k.value=0; k.special=K_SEND;
    keys[numKeys++]=k;
  }
}

// ================= TOUCH =================
TouchPoint getTouch() {
  TouchPoint p = {0, 0, false};
  if (!touch.tirqTouched() || !touch.touched()) return p;
  TS_Point raw = touch.getPoint();
  if (raw.z < 400) return p;
  // Landscape rotation=1: raw X?screen X, raw Y?screen Y (flip Y axis)
  int sx = map(raw.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCR_W);
  int sy = map(raw.y, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, SCR_H);  // note: Y flipped
  p.x = constrain(sx, 0, SCR_W - 1);
  p.y = constrain(sy, 0, SCR_H - 1);
  p.valid = true;
  return p;
}

// ================= AES =================
void aes_op(uint8_t *in, uint8_t *out, int mode) {
  uint8_t KEY[16] = {
    0x32,0x67,0x2f,0x79,0x74,0xad,0x43,0x45,
    0x1d,0x9c,0x6c,0x89,0x4a,0x0e,0x87,0x64
  };
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mode == MBEDTLS_AES_ENCRYPT) mbedtls_aes_setkey_enc(&aes, KEY, 128);
  else                              mbedtls_aes_setkey_dec(&aes, KEY, 128);
  mbedtls_aes_crypt_ecb(&aes, mode, in, out);
  mbedtls_aes_free(&aes);
}

// ================= BLE =================
void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  uint8_t dec[64] = {0};
  for (size_t i = 0; i + 16 <= len; i += 16)
    aes_op(data + i, dec + i, MBEDTLS_AES_DECRYPT);
  Resp r; r.len = min(len, (size_t)64);
  memcpy(r.data, dec, r.len);
  xQueueSend(respQueue, &r, 0);
}

bool waitFor(const char* token, uint32_t timeout = 5000) {
  Resp r; uint32_t start = millis();
  while (millis() - start < timeout)
    if (xQueueReceive(respQueue, &r, 10) == pdTRUE)
      if (memmem(r.data, r.len, token, strlen(token))) return true;
  return false;
}

void resetBLE() {
  cmdChar = uploadChar = notifyChar = fd02Char = nullptr;
  if (bleClient) {
    if (bleClient->isConnected()) bleClient->disconnect();
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
  }
  Resp r;
  while (xQueueReceive(respQueue, &r, 0) == pdTRUE) {}
}

bool connectBLE() {
  bleClient = NimBLEDevice::createClient();
  NimBLEAddress addr(cfgMAC.c_str(), BLE_ADDR_PUBLIC);
  if (!bleClient->connect(addr)) return false;
  auto services = bleClient->getServices(true);
  for (auto svc : services) {
    if (!cmdChar)    cmdChar    = svc->getCharacteristic(CHAR_CMD);
    if (!uploadChar) uploadChar = svc->getCharacteristic(CHAR_UPLOAD);
    if (!notifyChar) notifyChar = svc->getCharacteristic(CHAR_NOTIFY);
    if (!fd02Char)   fd02Char   = svc->getCharacteristic(CHAR_FD02);
  }
  if (!cmdChar || !uploadChar) return false;
  if (notifyChar) notifyChar->subscribe(true, notifyCB);
  if (fd02Char)   fd02Char->subscribe(true, notifyCB);
  delay(500);
  return true;
}

// ================= TEXT ? BADGE COLUMNS =================
uint8_t reverseBits(uint8_t b) {
  b = (b&0xF0)>>4|(b&0x0F)<<4;
  b = (b&0xCC)>>2|(b&0x33)<<2;
  b = (b&0xAA)>>1|(b&0x55)<<1;
  return b;
}

int textToColumns(const char* text, uint8_t* out, int maxLen) {
  int idx = 0;
  for (int i = 0; i < (int)strlen(text); i++) {
    int fi = toupper(text[i]) - 32;
    if (fi < 0 || fi > 58) fi = 0;
    for (int c = 0; c < 5; c++) {
      uint16_t col16 = (uint16_t)reverseBits(font5x8[fi][c]) << 4;
      if (idx + 2 >= maxLen) return idx;
      out[idx++] = col16 >> 8;
      out[idx++] = col16 & 0xFF;
    }
    if (i < (int)strlen(text) - 1) {
      if (idx + 2 >= maxLen) return idx;
      out[idx++] = 0; out[idx++] = 0;
    }
  }
  return idx;
}

bool sendTextNow() {
  static uint8_t text_bytes[2048];
  int text_len = textToColumns(inputText.c_str(), text_bytes, sizeof(text_bytes));
  if (text_len <= 0) return false;
  int cols = text_len / 2, total_len = text_len + cols * 3;
  uint8_t* payload = (uint8_t*)malloc(total_len);
  if (!payload) return false;
  memcpy(payload, text_bytes, text_len);
  for (int i = 0; i < cols; i++) {
    payload[text_len + i*3+0] = colorsRGB[selectedColorIndex][0];
    payload[text_len + i*3+1] = colorsRGB[selectedColorIndex][1];
    payload[text_len + i*3+2] = colorsRGB[selectedColorIndex][2];
  }
  uint8_t b[16]={0}, enc[16];
  b[0]=9; b[1]='D'; b[2]='A'; b[3]='T'; b[4]='S';
  b[5]=total_len>>8; b[6]=total_len&0xFF;
  b[7]=text_len>>8;  b[8]=text_len&0xFF;
  aes_op(b, enc, MBEDTLS_AES_ENCRYPT);
  cmdChar->writeValue(enc, 16, false);
  if (!waitFor("DATSOK")) { free(payload); return false; }
  for (int off=0, idx=0; off<total_len; off+=98, idx++) {
    int sz = min(98, total_len-off);
    uint8_t chunk[100]; chunk[0]=sz+1; chunk[1]=idx&0xFF;
    memcpy(&chunk[2], &payload[off], sz);
    uploadChar->writeValue(chunk, sz+2, false);
    if (!waitFor("REOK")) { free(payload); return false; }
  }
  uint8_t cp[16]={5,'D','A','T','C','P'};
  aes_op(cp, enc, MBEDTLS_AES_ENCRYPT);
  cmdChar->writeValue(enc, 16, false); waitFor("DATCPOK");
  uint8_t m[16]={5,'M','O','D','E', modeValues[selectedModeIndex]};
  aes_op(m, enc, MBEDTLS_AES_ENCRYPT);
  cmdChar->writeValue(enc, 16, false); waitFor("MODEOK", 2000);
  free(payload); return true;
}

// ================= UI HELPERS =================
uint16_t getColor565() {
  return tft.color565(colorsRGB[selectedColorIndex][0],
                      colorsRGB[selectedColorIndex][1],
                      colorsRGB[selectedColorIndex][2]);
}

// ---- Info bar (y 0..30) ----
void drawInfoBar() {
  tft.fillRect(0, 0, SCR_W, 31, C_BG);
  tft.drawRoundRect(0, 1, SCR_W - 80, 28, 3, C_GREY);
  tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(4, 7);
  tft.print(">");
  tft.setCursor(20, 7);
  String disp = inputText;
  if (disp.length() > 17) disp = disp.substring(disp.length() - 17);
  tft.print(disp);
  tft.print("_");

  uint16_t sc = C_GREEN;
  if (statusMsg.startsWith("BLE") || statusMsg == "SEND FAIL") sc = C_RED;
  else if (statusMsg == "CONNECTING..." || statusMsg == "SENDING...") sc = C_YELLOW;
  tft.setTextSize(1);
  tft.setTextColor(sc, C_BG);
  int sw = statusMsg.length() * 6;
  tft.setCursor(SCR_W - sw - 2, 12);
  tft.print(statusMsg);
}

// ---- Tab bar (y 31..52): [BLE][PRD][CLK]  *color*  [mode]  ...preview... ----
void drawTabBar() {
  tft.fillRect(0, 31, SCR_W, 22, C_DKGREY);

  struct { const char* lbl; ScreenMode m; uint16_t col; } tabs[] = {
    {"BLE", MODE_BLE,      C_CYAN},
    {"PRD", MODE_PREDATOR, C_RED},
    {"CLK", MODE_CLOCK,    C_GREEN},
  };
  for (int i = 0; i < 3; i++) {
    bool active = (currentMode == tabs[i].m);
    int tx = 2 + i * 44;
    tft.fillRoundRect(tx, 33, 40, 18, 3, active ? tabs[i].col : C_GREY);
    tft.setTextColor(active ? C_BG : C_WHITE, active ? tabs[i].col : C_GREY);
    tft.setTextSize(1);
    tft.setCursor(tx + (40 - strlen(tabs[i].lbl)*6)/2, 39);
    tft.print(tabs[i].lbl);
  }

  // Color swatch
  tft.fillRect(138, 33, 34, 18, getColor565());
  tft.drawRect(138, 33, 34, 18, C_WHITE);
  tft.setTextColor(C_BG, getColor565());
  tft.setTextSize(1);
  tft.setCursor(141, 39);
  tft.print(colorNames[selectedColorIndex]);

  // Mode button
  tft.fillRoundRect(176, 33, 38, 18, 3, C_GREY);
  tft.setTextColor(C_CYAN, C_GREY);
  tft.setTextSize(1);
  tft.setCursor(179, 39);
  tft.print(modeNames[selectedModeIndex]);

  // Preview area (right of mode button)
  tft.fillRect(218, 31, SCR_W - 218, 22, C_BG);
}

void drawPreviewStrip() {
  if (inputText.length() == 0) return;
  String text = inputText + "   ";
  int len = text.length();
  tft.fillRect(219, 33, SCR_W - 220, 18, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(getColor565(), C_BG);
  // Show up to 13 chars scrolling
  for (int i = 0; i < 13 && i < len; i++) {
    tft.setCursor(220 + i * 7, 39);
    tft.print(text[(previewOffset + i) % len]);
  }
}

// ---- Draw one key ----
void drawKey(const Key& k, bool pressed = false) {
  uint16_t face, border, textCol;
  if (k.special == K_SEND) {
    face = pressed ? C_RED : C_ACCENT; border = C_RED; textCol = C_BG;
  } else if (k.special == K_BACKSPACE) {
    face = pressed ? C_GREY : C_KEYBG; border = C_RED; textCol = C_RED;
  } else if (k.special == K_CLEAR) {
    face = pressed ? C_GREY : C_KEYBG; border = C_YELLOW; textCol = C_YELLOW;
  } else if (k.special == K_SPACE) {
    face = pressed ? C_KEYHI : C_KEYBG; border = C_GREY; textCol = C_WHITE;
  } else {
    face = pressed ? C_KEYHI : C_KEYBG; border = C_SHADOW; textCol = C_WHITE;
  }
  if (!pressed) tft.fillRoundRect(k.x+1, k.y+2, k.w, k.h, 3, C_SHADOW);
  tft.fillRoundRect(k.x, k.y, k.w, k.h, 3, face);
  tft.drawRoundRect(k.x, k.y, k.w, k.h, 3, border);
  tft.setTextColor(textCol, face);
  tft.setTextSize(1);
  int lw = strlen(k.label) * 6;
  tft.setCursor(k.x + (k.w - lw) / 2, k.y + (k.h - 8) / 2);
  tft.print(k.label);
}

void drawAllKeys() {
  for (int i = 0; i < numKeys; i++) drawKey(keys[i]);
}

// ---- Hit tests ----
int hitTestKeys(int tx, int ty) {
  for (int i = 0; i < numKeys; i++)
    if (tx >= keys[i].x && tx < keys[i].x + keys[i].w &&
        ty >= keys[i].y && ty < keys[i].y + keys[i].h)
      return i;
  return -1;
}

int hitTestTabBar(int tx, int ty) {
  if (ty < 31 || ty > 52) return -1;
  if (tx >=   2 && tx <  42) return K_SCR_BLE;
  if (tx >=  46 && tx <  86) return K_SCR_PRD;
  if (tx >=  90 && tx < 130) return K_SCR_CLK;
  if (tx >= 138 && tx < 172) return K_COLOR;
  if (tx >= 176 && tx < 214) return K_MODE;
  return -1;
}

// ---- Full BLE screen ----
void drawBLEScreen() {
  tft.fillScreen(C_BG);
  drawInfoBar();
  drawTabBar();
  drawAllKeys();
}

// ================= CLOCK SCREEN =================
void draw7Seg(int x, int y, int val, uint16_t color, int scale = 4) {
  const uint8_t seg[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
  uint8_t s = seg[val % 10];
  int w=10*scale, h=18*scale, t=2*scale;
  if(s&0x01) tft.fillRect(x+t,   y,         w-2*t, t,     color);
  if(s&0x02) tft.fillRect(x+w-t, y+t,       t,     h/2-t, color);
  if(s&0x04) tft.fillRect(x+w-t, y+h/2+t,   t,     h/2-t, color);
  if(s&0x08) tft.fillRect(x+t,   y+h-t,     w-2*t, t,     color);
  if(s&0x10) tft.fillRect(x,     y+h/2+t,   t,     h/2-t, color);
  if(s&0x20) tft.fillRect(x,     y+t,       t,     h/2-t, color);
  if(s&0x40) tft.fillRect(x+t,   y+h/2-t/2, w-2*t, t,     color);
}

// Clear a single 7-seg digit region before redrawing
void clear7Seg(int x, int y, int scale = 4) {
  tft.fillRect(x, y, 10*scale + 1, 18*scale + 1, C_BG);
}

// Track previous values so we only redraw changed digits
static int prevHH = -1, prevMM = -1, prevSS = -1;
static bool prevColon = false;
static bool clockFullDraw = true;

void drawClockScreen() {
  if (clockFullDraw) {
    tft.fillScreen(C_BG);
    drawTabBar();
    tft.setTextSize(2);
    clockFullDraw = false;
    prevHH = prevMM = prevSS = -1;
  }

  struct tm ti;
  bool useNTP = getLocalTime(&ti);
  int hh = useNTP ? ti.tm_hour : (millis()/3600000)%24;
  int mm = useNTP ? ti.tm_min  : (millis()/60000)%60;
  int ss = useNTP ? ti.tm_sec  : (millis()/1000)%60;

  // NTP label - only on full draw (already cleared above)
  static bool prevNTP = !useNTP; // force first draw
  if (useNTP != prevNTP) {
    prevNTP = useNTP;
    tft.fillRect(0, 58, 160, 18, C_BG);
    tft.setTextSize(2);
    tft.setTextColor(useNTP ? C_GREEN : C_RED, C_BG);
    tft.setCursor(8, 58);
    tft.print(useNTP ? "NTP SYNC" : "UPTIME  ");
  }

  // 7-seg clock - landscape centred vertically
  // Each digit: scale=4 ? 40px wide, 72px tall. Two digits + colon + two digits
  // Total width: 4*40 + 20(colon) + 4(gaps) = 184px - centred in 320px ? x=68
  const int DX = 40, DY = 72, CX = 68, CY = 80;

  if (hh/10 != prevHH/10 || prevHH < 0) { clear7Seg(CX,       CY); draw7Seg(CX,       CY, hh/10, C_CYAN); }
  if (hh%10 != prevHH%10 || prevHH < 0) { clear7Seg(CX+DX+2,  CY); draw7Seg(CX+DX+2,  CY, hh%10, C_CYAN); }
  if (mm/10 != prevMM/10 || prevMM < 0) { clear7Seg(CX+DX*2+24,CY); draw7Seg(CX+DX*2+24,CY, mm/10, C_CYAN); }
  if (mm%10 != prevMM%10 || prevMM < 0) { clear7Seg(CX+DX*3+26,CY); draw7Seg(CX+DX*3+26,CY, mm%10, C_CYAN); }

  prevHH = hh; prevMM = mm;

  // Blinking colon dots
  bool colonOn = (millis()/500)%2;
  if (colonOn != prevColon) {
    prevColon = colonOn;
    int cx = CX + DX*2 + 12;
    tft.fillCircle(cx, CY + DY/3,   4, colonOn ? C_WHITE : C_BG);
    tft.fillCircle(cx, CY + DY*2/3, 4, colonOn ? C_WHITE : C_BG);
  }

  // Seconds bar - update only changed region
  if (ss != prevSS) {
    prevSS = ss;
    int barX = 10, barY = 163, barW = SCR_W - 20, barH = 6;
    int filled = (barW * ss) / 59;
    tft.fillRect(barX, barY, filled, barH, C_CYAN);
    tft.fillRect(barX + filled, barY, barW - filled, barH, C_DKGREY);
  }

  // Date - only update when minute changes
  if (useNTP && (mm != prevMM || prevMM < 0)) {
    char datebuf[20];
    sprintf(datebuf, "%04d-%02d-%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
    tft.fillRect(0, 176, SCR_W, 18, C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_GREY, C_BG);
    tft.setCursor((SCR_W - strlen(datebuf)*12)/2, 176);
    tft.print(datebuf);
  }

  tft.setTextSize(1);
  tft.setTextColor(C_GREY, C_BG);
  tft.setCursor(60, 232);
  tft.print("tap BLE/PRD to switch");
}

// ================= PREDATOR SCREEN =================
// Flicker fix: redraw only segments that changed; never fillScreen in the update loop.

#define NUM_GLYPHS 5
static uint8_t prevGlyphStates[NUM_GLYPHS] = {0xFF,0xFF,0xFF,0xFF,0xFF};
static bool predFullDraw = true;

// Draw/erase one segment line of a predator glyph
void drawPredSeg(int cx, int cy, int x1,int y1,int x2,int y2, bool on) {
  uint16_t col   = on ? C_RED     : C_BG;
  uint16_t glow  = on ? C_DARKRED : C_BG;
  tft.drawLine(cx+x1-1, cy+y1, cx+x2-1, cy+y2, glow);
  tft.drawLine(cx+x1,   cy+y1, cx+x2,   cy+y2, col);
  tft.drawLine(cx+x1+1, cy+y1, cx+x2+1, cy+y2, glow);
}

// Draw all 8 segments of one glyph only where bits differ from prev
void updateGlyph(int cx, int cy, uint8_t newState, uint8_t oldState) {
  // Segment definitions: x1,y1,x2,y2
  const int8_t segs[8][4] = {
    {-10,-45,-10,-28},  // 0
    { 10,-45, 10,-28},  // 1
    {-10,-28,-22,-10},  // 2
    { 10,-28, 22,-10},  // 3
    {-22, 10,-10, 28},  // 4
    { 22, 10, 10, 28},  // 5
    {-10, 28,-10, 45},  // 6
    { 10, 28, 10, 45},  // 7
  };
  for (int s = 0; s < 8; s++) {
    bool nowOn  = bitRead(newState, s);
    bool wasOn  = bitRead(oldState, s);
    if (nowOn != wasOn || predFullDraw) {
      drawPredSeg(cx, cy,
                  segs[s][0], segs[s][1],
                  segs[s][2], segs[s][3],
                  nowOn);
    }
  }
  // Horizontal scanner line - random, erase previous then draw new
  // We always refresh these (they're cheap) only when state changes
  if (newState != oldState || predFullDraw) {
    // Erase old cross-bar area
    tft.drawFastHLine(cx-22, cy, 44, C_BG);
    if (random(0,5) > 2) tft.drawFastHLine(cx-18, cy, 36, C_RED);
  }
}

void drawPredatorScreen() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  if (predFullDraw) {
    tft.fillScreen(C_BG);
    drawTabBar();
    // Title
    tft.setTextSize(2);
    tft.setTextColor(C_RED, C_BG);
    int titleX = (SCR_W - 15*12)/2;
    tft.setCursor(titleX, 196);
    tft.print("FINAL COUNTDOWN");
    tft.setTextSize(1);
    tft.setTextColor(C_DARKRED, C_BG);
    tft.setCursor(55, 220);
    tft.print("tap BLE/CLK to switch");
    predFullDraw = false;
    memset(prevGlyphStates, 0xFF, sizeof(prevGlyphStates)); // force full redraw
  }

  if (now - lastUpdate < 350) return;  // 350ms between glyph updates - steady, not frantic
  lastUpdate = now;

  // Glyph centres spread across landscape width
  // 5 glyphs, width 320px ? spacing ~56px, first at x=32
  for (int g = 0; g < NUM_GLYPHS; g++) {
    int cx = 32 + g * 56;
    int cy = 145;
    uint8_t newState = random(0, 256);
    updateGlyph(cx, cy, newState, prevGlyphStates[g]);
    prevGlyphStates[g] = newState;
  }
}

// ================= SEND FLOW =================
void doSend() {
  statusMsg = "CONNECTING..."; drawInfoBar();
  resetBLE();
  if (connectBLE()) {
    statusMsg = "SENDING..."; drawInfoBar();
    statusMsg = sendTextNow() ? "SUCCESS" : "SEND FAIL";
    bleClient->disconnect();
  } else {
    statusMsg = "BLE ERROR";
  }
  resetBLE();
  drawInfoBar();
  delay(1500);
  statusMsg = "READY"; drawInfoBar();
}

// ================= WIFI + NTP =================
void startNTP() {
  if (cfgSSID.length() == 0) {
    Serial.println("No SSID in config - skipping NTP");
    return;
  }
  Serial.printf("WiFi connecting to %s ...\n", cfgSSID.c_str());
  WiFi.begin(cfgSSID.c_str(), cfgPASS.c_str());
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
    configTzTime(cfgTZ.c_str(), cfgNTP.c_str());
    Serial.println("NTP configured");
  } else {
    Serial.println("WiFi failed - clock will show uptime");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);

  tft.init();
  tft.setRotation(1);   // LANDSCAPE
  tft.fillScreen(C_BG);

  touchSPI.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSPI);
  touch.setRotation(1); // match display rotation

  respQueue = xQueueCreate(10, sizeof(Resp));
  NimBLEDevice::init("");

  // Load SD config first (provides MAC, SSID, TZ, NTP)
  loadConfig();

  // Connect WiFi & sync NTP using config credentials
  startNTP();

  buildKeys();
  drawBLEScreen();
}

// ================= LOOP =================
void loop() {
  static unsigned long lastScroll  = 0;
  static unsigned long lastClock   = 0;
  static unsigned long lastPred    = 0;
  static unsigned long lastTouch   = 0;
  static bool wasPressed = false;

  unsigned long now = millis();

  // Clock: update every second (dirty-rect only, no fillScreen)
  if (currentMode == MODE_CLOCK && now - lastClock >= 1000) {
    lastClock = now;
    drawClockScreen();
  }

  // Predator: update on its own internal timer (inside drawPredatorScreen)
  if (currentMode == MODE_PREDATOR) {
    drawPredatorScreen();
  }

  // Scroll preview in BLE mode
  if (currentMode == MODE_BLE && now - lastScroll > 150 && inputText.length() > 0) {
    lastScroll = now;
    previewOffset = (previewOffset + 1) % (inputText.length() + 3);
    drawPreviewStrip();
  }

  // Touch debounce
  TouchPoint p = getTouch();
  if (p.valid && !wasPressed && now - lastTouch > 180) {
    wasPressed = true;
    lastTouch  = now;

    int tabHit = hitTestTabBar(p.x, p.y);
    if (tabHit >= 0) {
      if (tabHit == K_SCR_BLE && currentMode != MODE_BLE) {
        currentMode = MODE_BLE; clockFullDraw = true; predFullDraw = true;
        drawBLEScreen(); return;
      }
      if (tabHit == K_SCR_PRD && currentMode != MODE_PREDATOR) {
        currentMode = MODE_PREDATOR; predFullDraw = true; clockFullDraw = true;
        drawPredatorScreen(); return;
      }
      if (tabHit == K_SCR_CLK && currentMode != MODE_CLOCK) {
        currentMode = MODE_CLOCK; clockFullDraw = true; predFullDraw = true;
        drawClockScreen(); return;
      }
      if (tabHit == K_COLOR) {
        selectedColorIndex = (selectedColorIndex + 1) % 5;
        drawTabBar();
        if (currentMode == MODE_BLE) drawAllKeys();
        return;
      }
      if (tabHit == K_MODE) {
        selectedModeIndex = (selectedModeIndex + 1) % 3;
        drawTabBar(); return;
      }
    }

    if (currentMode == MODE_BLE) {
      int ki = hitTestKeys(p.x, p.y);
      if (ki >= 0) {
        Key& k = keys[ki];
        drawKey(k, true); delay(60); drawKey(k, false);
        if      (k.special == K_BACKSPACE) {
          if (inputText.length() > 0) inputText.remove(inputText.length() - 1);
          drawInfoBar();
        } else if (k.special == K_CLEAR) {
          inputText = ""; previewOffset = 0; drawInfoBar();
        } else if (k.special == K_SEND) {
          doSend();
        } else if (k.special == K_SPACE || k.value == ' ') {
          if (inputText.length() < 50) { inputText += ' '; drawInfoBar(); }
        } else if (k.value != 0) {
          if (inputText.length() < 50) { inputText += k.value; drawInfoBar(); }
        }
      }
    }
  }

  if (!p.valid) wasPressed = false;
}
