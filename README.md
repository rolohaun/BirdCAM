# BirdCAM ESP32-CAM Snapshot Viewer

This project runs on an ESP32-CAM board with an OV3660 camera and serves a tiny web page that refreshes snapshots every five seconds.

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
- `/info` firmware version info
- `/capture` one JPEG snapshot
- `/settings?framesize=qxga&quality=10` update snapshot settings
- `/ota/check` check GitHub manifest for a newer firmware release
- `/ota/start` start a background OTA install
- `/ota/status` read OTA progress
- `/ota/update` compatibility alias for `/ota/start`

## Snapshot Viewer

The web interface fetches one JPEG every five seconds and keeps the five most recent snapshots in a thumbnail strip. It pauses snapshot polling when the browser tab is hidden and resumes when the tab is visible again.

## OTA Updates

BirdCAM checks this manifest:

```text
https://raw.githubusercontent.com/rolohaun/BirdCAM/main/firmware/manifest.json
```

The manifest points to a GitHub Release binary and includes its SHA-256 hash. The web page has `Check Update` and `Install Update` buttons, with a progress bar and status/error text during installation.

To publish a new OTA release from this machine:

```powershell
.\scripts\publish-ota.ps1 -Version 0.2.0
```

The script builds firmware, computes SHA-256, uploads the binary with GitHub CLI, updates `firmware/manifest.json`, commits, and pushes.

Before mounting the board in the birdhouse, do one USB flash with your local `src/secrets.h` present so Wi-Fi credentials are stored in NVS.

## Quality and Frame Rate Tuning

The sketch defaults to `FRAMESIZE_QXGA` and JPEG quality `10` for maximum OV3660 snapshot quality. In `src/main.cpp`, change these values if you want a different tradeoff:

```cpp
static const framesize_t STREAM_FRAME_SIZE = FRAMESIZE_QXGA;
static const int STREAM_JPEG_QUALITY = 10;
```

Try `FRAMESIZE_UXGA` if QXGA is unstable, or `FRAMESIZE_VGA` / `FRAMESIZE_QVGA` with quality `25` for lower power and faster refreshes.

`FRAMESIZE_QXGA` requires an OV3660 sensor. The firmware prints the detected camera PID in Serial Monitor at boot.

## Solar Power Notes

The firmware disables Bluetooth, lowers the CPU clock, enables Wi-Fi modem sleep, lowers the camera XCLK, reduces serial chatter, and starts with lower-bandwidth snapshots.

The web page uses periodic snapshots instead of an always-open MJPEG stream, which greatly reduces Wi-Fi traffic and camera work when someone is watching.

The red power LED on most ESP32-CAM boards is wired directly to the power rail, so firmware cannot turn it off. To remove that draw, you would need to desolder the LED or its series resistor, or cut the LED trace.

## Notes

The included pin map is for the common AI Thinker-style ESP32-CAM layout, which is what most ESP32-CAM-MB/CH340G kits use. If the serial log says `Camera init failed`, reseat the ribbon cable and confirm the board is the AI Thinker ESP32-CAM pinout.
