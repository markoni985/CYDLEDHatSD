# CYD LED Hat Controller

A touchscreen BLE controller for LED badge hats, running on the **ESP32-2432S028R** (Cheap Yellow Display). Type a message on the on-screen keyboard, pick a color and scroll mode, and blast it over BLE to your LED hat — all from a 320x240 landscape touchscreen.

![ESP32-2432S028R CYD board](https://raw.githubusercontent.com/witnessmenow/ESP32-Cheap-Yellow-Display/main/images/CYD.jpg)

---

## Features

- **BLE keyboard screen** — full QWERTY touchscreen keyboard with backspace, clear, and send
- **5 text colors** — Red, Green, Blue, Yellow, White (tap the color swatch to cycle)
- **3 scroll modes** — Fixed, Scroll Right, Scroll Left (tap the mode button to cycle)
- **Live preview strip** — scrolling ticker of your typed text in the tab bar
- **Clock screen** — large 7-segment NTP-synced clock with date and seconds progress bar; falls back to uptime if WiFi is unavailable
- **Predator screen** — animated alien-glyph display with red segment flicker
- **SD card config** — WiFi credentials, MAC address, and timezone loaded from `config.txt` at boot; no recompile needed to change settings
- **AES-128 encrypted BLE** — matches the badge protocol (ECB mode, hardcoded key)

---

## Hardware

| Component | Value |
|-----------|-------|
| Board | ESP32-2432S028R (CYD — Cheap Yellow Display) |
| Display | ILI9341 2.8" 320x240 TFT |
| Touch | XPT2046 resistive touchscreen |
| Interface | Landscape (rotation 1) |
| SD card | SPI via GPIO 5 (CS) |

### Pin Reference

| Function | GPIO |
|----------|------|
| TFT MISO | 12 |
| TFT MOSI | 13 |
| TFT SCLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT BL | 21 |
| Touch CS | 33 |
| Touch IRQ | 36 |
| Touch CLK | 25 |
| Touch MISO | 39 |
| Touch MOSI | 32 |
| SD CS | 5 |

---

## Libraries Required

Install all of these via **Arduino Library Manager** or the links below:

| Library | Install |
|---------|---------|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | Library Manager |
| [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) | Library Manager |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Library Manager |
| SD | Built into ESP32 Arduino core |
| WiFi | Built into ESP32 Arduino core |
| mbedtls/aes | Built into ESP32 Arduino core |

---

## TFT_eSPI Configuration

Edit `User_Setup.h` in your TFT_eSPI library folder and set:

```cpp
#define ILI9341_DRIVER

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

#define SPI_FREQUENCY  40000000
```

---

## SD Card Setup

Create a file called `config.txt` in the **root** of your SD card with the following format:

```
MAC=00:41:20:03:16:05
TZ=CST6CDT,M3.2.0,M11.1.0
SSID=YourNetworkName
PASS=YourPassword
```

| Key | Description |
|-----|-------------|
| `MAC` | BLE MAC address of your LED hat |
| `TZ` | POSIX timezone string (used for DST-aware NTP) |
| `SSID` | WiFi network name |
| `PASS` | WiFi password |

The NTP server defaults to `pool.ntp.org`. You can override it by adding:
```
NTP=time.cloudflare.com
```

If the SD card or config file is missing, the sketch falls back to the compiled-in defaults and skips NTP sync.

---

## Touch Calibration

If taps feel off on your unit, tune these four defines near the top of the sketch:

```cpp
#define TOUCH_X_MIN   300
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN   200
#define TOUCH_Y_MAX  3700
```

**If top and bottom are swapped**, flip the Y values:
```cpp
#define TOUCH_Y_MIN  3700
#define TOUCH_Y_MAX   200
```

---

## Screens

### BLE Keyboard
The main screen. Type your message, select a color and scroll mode from the tab bar, then tap **SND** to connect and push the text to the hat over BLE.

```
[ BLE ] [ PRD ] [ CLK ]   [color]  [mode]   ...preview...
+------------------------------------------------+
|  1   2   3   4   5   6   7   8   9   0        |
|  Q   W   E   R   T   Y   U   I   O   P        |
|    A   S   D   F   G   H   J   K   L          |
|  Z   X   C   V   B   N   M      [ DEL ]       |
|  [  S P A C E  ]    [ CLR ]    [  SND  ]      |
+------------------------------------------------+
```

### Clock
Large 7-segment display showing hours and minutes. When WiFi/NTP is available the time is accurate to your configured timezone, including DST. A seconds progress bar runs across the bottom.

### Predator
Animated alien-language glyph display with red flickering segments — purely decorative, for hat ambiance.

---

## BLE Protocol

The sketch talks to the LED hat using an AES-128 ECB encrypted protocol over two GATT characteristics:

| Role | UUID |
|------|------|
| Command | `d44bc439-abfd-45a2-b575-925416129600` |
| Upload | `d44bc439-abfd-45a2-b575-92541612960a` |
| Notify | `d44bc439-abfd-45a2-b575-925416129601` |

Text is converted to 5x8 bitmap font column pairs, each column gets an RGB color triplet appended, then the payload is chunked into 98-byte upload frames with a handshake (`DATSOK` / `REOK` / `DATCPOK`) between each.

---

## Arduino IDE Setup

1. Install **ESP32 board support** via Boards Manager — search `esp32` by Espressif, version 3.x
2. Select board: **ESP32 Dev Module**
3. Set upload speed: **921600**
4. CPU frequency: **240 MHz**
5. Flash size: **4MB**
6. Partition scheme: **Default**

---

## Troubleshooting

**Touch is flipped top-to-bottom** — swap `TOUCH_Y_MIN` and `TOUCH_Y_MAX` in the defines.

**`'TouchPoint' does not name a type`** — the structs must stay above the `#include` lines. Do not move them. This is an Arduino IDE preprocessor quirk with custom types as return values.

**SD card not mounting** — confirm your SD CS pin. Some CYD variants use GPIO 5, others use GPIO 4. Check with a bare `SD.begin()` sketch if unsure.

**BLE connect fails** — verify the MAC address in `config.txt` matches your hat exactly. The sketch uses `BLE_ADDR_PUBLIC`; if your hat advertises a random address type, change that constant in `connectBLE()`.

**Clock shows UPTIME instead of NTP SYNC** — WiFi credentials in `config.txt` are wrong, the network is out of range, or the NTP server is unreachable. Check Serial monitor at 115200 baud for the connection log.

---

## License

MIT
