# BirdCAM ESP32-CAM Web Viewer

This project runs on an ESP32-CAM board with an OV3660 camera and serves a tiny web page for viewing the camera.

## Configure Wi-Fi

Copy `src/secrets.example.h` to `src/secrets.h` and set:

```cpp
static const char *DEFAULT_WIFI_SSID = "";
static const char *DEFAULT_WIFI_PASSWORD = "";
```

`src/secrets.h` is ignored by git. On a USB-flashed build, BirdCAM stores those credentials in ESP32 NVS so later OTA firmware does not need to contain your Wi-Fi password.

If you leave `DEFAULT_WIFI_SSID` blank, the ESP32-CAM creates its own Wi-Fi network:

- Network: `BirdCAM`
- Password: `birdcam123`
- Camera page: `http://192.168.4.1`

If you enter your home Wi-Fi credentials, open Serial Monitor at `115200` baud after flashing. The board prints the URL.

## Flashing With Arduino IDE

1. Install the ESP32 board package in Arduino IDE.
2. Open `src/main.cpp`, or copy its contents into a new Arduino sketch.
3. Select board: `AI Thinker ESP32-CAM`.
4. Select the CH340G serial port.
5. Upload.
6. Press `RST` on the ESP32-CAM-MB board after upload.

## Flashing With PlatformIO

```powershell
pio run -t upload
pio device monitor
```

## Useful URLs

- `/` live viewing page
- `:81/stream` raw MJPEG stream
- `/capture` one JPEG snapshot
- `/settings?framesize=vga&quality=18` update stream settings
- `/ota/check` check GitHub manifest for a newer firmware release
- `/ota/update` install a newer release after SHA-256 verification

## OTA Updates

BirdCAM checks this manifest:

```text
https://raw.githubusercontent.com/rolohaun/BirdCAM/main/firmware/manifest.json
```

The manifest points to a GitHub Release binary and includes its SHA-256 hash. The web page has `Check Update` and `Install Update` buttons.

To publish a new OTA release from this machine:

```powershell
.\scripts\publish-ota.ps1 -Version 0.2.0
```

The script builds firmware, computes SHA-256, uploads the binary with GitHub CLI, updates `firmware/manifest.json`, commits, and pushes.

Before mounting the board in the birdhouse, do one USB flash with your local `src/secrets.h` present so Wi-Fi credentials are stored in NVS.

## Quality and Frame Rate Tuning

The sketch defaults to `FRAMESIZE_VGA` and JPEG quality `25` to reduce power use and Wi-Fi bandwidth. In `src/main.cpp`, change these values if you want a different tradeoff:

```cpp
static const framesize_t STREAM_FRAME_SIZE = FRAMESIZE_VGA;
static const int STREAM_JPEG_QUALITY = 25;
```

Try `FRAMESIZE_QVGA` for lower power and faster streaming, or `FRAMESIZE_UXGA` with quality `10` for the highest reliable quality on common ESP32-CAM boards.

`FRAMESIZE_QXGA` exists for OV3660 sensors in the camera library, but it is unreliable on many ESP32-CAM kits and is not supported by OV2640 modules. The firmware prints the detected camera PID in Serial Monitor at boot.

## Solar Power Notes

The firmware disables Bluetooth, lowers the CPU clock, enables Wi-Fi modem sleep, lowers the camera XCLK, reduces serial chatter, and starts with a lower-bandwidth stream.

The red power LED on most ESP32-CAM boards is wired directly to the power rail, so firmware cannot turn it off. To remove that draw, you would need to desolder the LED or its series resistor, or cut the LED trace.

## Notes

The included pin map is for the common AI Thinker-style ESP32-CAM layout, which is what most ESP32-CAM-MB/CH340G kits use. If the serial log says `Camera init failed`, reseat the ribbon cable and confirm the board is the AI Thinker ESP32-CAM pinout.
