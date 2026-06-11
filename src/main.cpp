#include "Arduino.h"
#include "ArduinoJson.h"
#include "HTTPClient.h"
#include "Preferences.h"
#include "Update.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_bt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "ctype.h"
#include "string.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
static const char *DEFAULT_WIFI_SSID = "";
static const char *DEFAULT_WIFI_PASSWORD = "";
#endif

static const char *AP_SSID = "BirdCAM";
static const char *AP_PASSWORD = "birdcam123";
static const char *FIRMWARE_VERSION = "0.2.11";
static const char *OTA_MANIFEST_URL = "https://raw.githubusercontent.com/rolohaun/BirdCAM/main/firmware/manifest.json";

// Highest OV3660 snapshot defaults. QXGA is demanding, but snapshots give it
// more breathing room than continuous streaming.
static const framesize_t STREAM_FRAME_SIZE = FRAMESIZE_QXGA;
static const int STREAM_JPEG_QUALITY = 10;  // 10 is sharper/slower, 30 is smaller/faster.
static const int CPU_FREQ_MHZ = 80;
static const int CAMERA_XCLK_HZ = 10000000;
static const unsigned long IP_PRINT_INTERVAL_MS = 300000;
static const unsigned long SNAPSHOT_INTERVAL_MS = 3000;
static const int SNAPSHOT_HISTORY_COUNT = 5;
static const size_t OTA_BUFFER_SIZE = 1024;
static const int OTA_TASK_STACK_SIZE = 16384;

// AI Thinker ESP32-CAM pin map. Used by most ESP32-CAM-MB CH340G kits.
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

static httpd_handle_t camera_httpd = nullptr;
static IPAddress camera_ip;
static String wifi_ssid;
static String wifi_password;
static unsigned long last_ip_print_ms = 0;
static framesize_t current_frame_size = STREAM_FRAME_SIZE;
static SemaphoreHandle_t ota_mutex = nullptr;
static bool camera_running = false;

struct OtaProgress {
  bool running;
  bool success;
  bool rebooting;
  int progress;
  char phase[20];
  char message[112];
  char version[24];
};

static OtaProgress ota_progress = {
    false,
    false,
    false,
    0,
    "idle",
    "Idle",
    "",
};

struct OtaManifest {
  String version;
  String url;
  String sha256;
};

struct FrameSizeOption {
  const char *value;
  framesize_t frame_size;
};

static const FrameSizeOption FRAME_SIZE_OPTIONS[] = {
    {"qqvga", FRAMESIZE_QQVGA},
    {"qvga", FRAMESIZE_QVGA},
    {"vga", FRAMESIZE_VGA},
    {"svga", FRAMESIZE_SVGA},
    {"xga", FRAMESIZE_XGA},
    {"sxga", FRAMESIZE_SXGA},
    {"uxga", FRAMESIZE_UXGA},
    {"qxga", FRAMESIZE_QXGA},
};

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BirdCAM</title>
  <style>
    :root { color-scheme: dark; font-family: Arial, sans-serif; }
    body { margin: 0; background: #101214; color: #f5f7f9; }
    main { min-height: 100vh; display: grid; grid-template-rows: auto 1fr; }
    header { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 14px 16px; background: #191d21; border-bottom: 1px solid #2b3138; }
    h1 { margin: 0; font-size: 20px; font-weight: 700; }
    .controls { display: flex; gap: 8px; flex-wrap: wrap; justify-content: flex-end; align-items: end; }
    label { display: grid; gap: 4px; font-size: 12px; color: #bac4cf; }
    select, button, a.button { border: 1px solid #3c4650; background: #242b31; color: #f5f7f9; border-radius: 6px; padding: 9px 12px; font-size: 14px; text-decoration: none; cursor: pointer; }
    select:hover, button:hover, a.button:hover { background: #303941; }
    [hidden] { display: none !important; }
    .viewer { display: grid; grid-template-rows: auto auto; align-content: start; gap: 8px; padding: 6px 12px 10px; min-height: 0; }
    .stage { display: grid; place-items: start center; min-height: 0; }
    #current { width: min(100%, 1100px); max-height: calc(100vh - 150px); object-fit: contain; background: #050607; border: 1px solid #2b3138; }
    .history { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px; width: min(100%, 1100px); margin: 0 auto; }
    .history img { width: 100%; aspect-ratio: 4 / 3; object-fit: cover; background: #050607; border: 1px solid #2b3138; opacity: 0.72; cursor: pointer; }
    .history img.active { opacity: 1; border-color: #94a3b8; }
    .status { font-size: 13px; color: #bac4cf; }
    .progress { position: fixed; left: 12px; right: 12px; bottom: 12px; display: grid; gap: 6px; padding: 10px; background: #191d21; border: 1px solid #2b3138; border-radius: 6px; }
    .progress-bar { height: 10px; overflow: hidden; background: #0b0d0f; border: 1px solid #3c4650; border-radius: 999px; }
    .progress-fill { height: 100%; width: 0%; background: #9ccfd8; transition: width 180ms ease; }
    .progress-text { font-size: 13px; color: #f5f7f9; }
    @media (max-width: 720px) {
      header { padding: 10px 12px; align-items: flex-start; }
      .controls { gap: 6px; }
      select, button, a.button { padding: 8px 10px; }
      .viewer { padding-top: 4px; }
      #current { max-height: calc(100vh - 132px); }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>BirdCAM</h1>
        <div class="status" id="status">Idle</div>
        <div class="status">Firmware <span id="firmware-version">...</span></div>
      </div>
      <div class="controls">
        <label>
          Resolution
          <select id="framesize">
            <option value="qxga" selected>QXGA 2048x1536</option>
            <option value="uxga">UXGA 1600x1200</option>
            <option value="sxga">SXGA 1280x1024</option>
            <option value="xga">XGA 1024x768</option>
            <option value="svga">SVGA 800x600</option>
            <option value="vga">VGA 640x480</option>
            <option value="qvga">QVGA 320x240</option>
            <option value="qqvga">QQVGA 160x120</option>
          </select>
        </label>
        <button id="snapshot" type="button">Snapshot</button>
        <button id="update-check" type="button">Check Update</button>
        <button id="update-install" type="button" hidden>Install Update</button>
      </div>
    </header>
    <section class="viewer">
      <div class="stage">
        <img id="current" alt="BirdCAM current snapshot">
      </div>
      <div class="history" id="history"></div>
    </section>
  </main>
  <div class="progress" id="ota-progress" hidden>
    <div class="progress-bar"><div class="progress-fill" id="ota-progress-fill"></div></div>
    <div class="progress-text" id="ota-progress-text">Preparing update...</div>
  </div>
  <script>
    const SNAPSHOT_INTERVAL_MS = 3000;
    const SNAPSHOT_HISTORY_COUNT = 5;
    const framesize = document.getElementById('framesize');
    const snapshot = document.getElementById('snapshot');
    const updateCheck = document.getElementById('update-check');
    const updateInstall = document.getElementById('update-install');
    const current = document.getElementById('current');
    const history = document.getElementById('history');
    const status = document.getElementById('status');
    const firmwareVersion = document.getElementById('firmware-version');
    const otaProgress = document.getElementById('ota-progress');
    const otaProgressFill = document.getElementById('ota-progress-fill');
    const otaProgressText = document.getElementById('ota-progress-text');
    const snapshots = [];
    let snapshotTimer = null;
    let otaPollTimer = null;
    let loadingSnapshot = false;
    let captureSequenceRunning = false;

    function captureUrl() {
      return '/capture?t=' + Date.now();
    }

    function setActiveSnapshot(index) {
      current.src = snapshots[index].url;
      [...history.children].forEach((img, childIndex) => {
        img.classList.toggle('active', childIndex === index);
      });
    }

    function renderHistory() {
      history.textContent = '';
      snapshots.forEach((snapshot, index) => {
        const img = document.createElement('img');
        img.src = snapshot.url;
        img.alt = 'BirdCAM snapshot';
        img.addEventListener('click', () => setActiveSnapshot(index));
        if (index === 0) img.classList.add('active');
        history.appendChild(img);
      });
    }

    async function captureSnapshot(sequenceIndex = null) {
      if (loadingSnapshot || document.hidden) return false;
      loadingSnapshot = true;
      status.textContent = sequenceIndex ? 'Capturing ' + sequenceIndex + ' of ' + SNAPSHOT_HISTORY_COUNT + '...' : 'Capturing...';

      try {
        const response = await fetch(captureUrl(), { cache: 'no-store' });
        if (!response.ok) throw new Error('Capture failed');
        const blob = await response.blob();
        const url = URL.createObjectURL(blob);
        snapshots.unshift({ url });

        while (snapshots.length > SNAPSHOT_HISTORY_COUNT) {
          const old = snapshots.pop();
          URL.revokeObjectURL(old.url);
        }

        renderHistory();
        setActiveSnapshot(0);
        status.textContent = 'Last capture ' + new Date().toLocaleTimeString();
        return true;
      } catch (err) {
        status.textContent = err.message;
        return false;
      } finally {
        loadingSnapshot = false;
      }
    }

    function startSnapshots() {
      if (snapshotTimer) clearTimeout(snapshotTimer);
      snapshotTimer = null;

      if (captureSequenceRunning) return;
      captureSequenceRunning = true;
      let captured = 0;
      snapshot.disabled = true;

      const runNext = async () => {
        if (document.hidden) {
          captureSequenceRunning = false;
          snapshot.disabled = false;
          snapshotTimer = null;
          status.textContent = 'Snapshot sequence paused';
          return;
        }

        const nextIndex = captured + 1;
        const ok = await captureSnapshot(nextIndex);
        if (!ok) {
          captureSequenceRunning = false;
          snapshot.disabled = false;
          snapshotTimer = null;
          return;
        }

        captured = nextIndex;

        if (captured >= SNAPSHOT_HISTORY_COUNT || document.hidden) {
          captureSequenceRunning = false;
          snapshot.disabled = false;
          snapshotTimer = null;
          status.textContent = captured >= SNAPSHOT_HISTORY_COUNT ? 'Captured 5 snapshots' : 'Snapshot sequence paused';
          return;
        }

        status.textContent = 'Waiting 3 seconds for snapshot ' + (captured + 1) + ' of ' + SNAPSHOT_HISTORY_COUNT + '...';
        snapshotTimer = setTimeout(runNext, SNAPSHOT_INTERVAL_MS);
      };

      runNext();
    }

    function setOtaProgress(info) {
      otaProgress.hidden = false;
      const progress = Math.max(0, Math.min(100, Number(info.progress || 0)));
      otaProgressFill.style.width = progress + '%';
      otaProgressText.textContent = progress + '% - ' + (info.message || info.phase || 'Updating...');
      status.textContent = info.message || 'Updating...';
    }

    async function pollOtaStatus() {
      try {
        const response = await fetch('/ota/status?t=' + Date.now(), { cache: 'no-store' });
        const info = await response.json();
        if (!response.ok) throw new Error(info.error || 'Unable to read update status');
        setOtaProgress(info);

        if (info.rebooting) {
          clearInterval(otaPollTimer);
          otaPollTimer = null;
          otaProgressText.textContent = '100% - Update installed. Rebooting...';
          return;
        }

        if (!info.running) {
          clearInterval(otaPollTimer);
          otaPollTimer = null;
          if (info.success) {
            otaProgressText.textContent = '100% - Update installed. Rebooting...';
          } else {
            otaProgressText.textContent = 'Update failed - ' + (info.message || 'Unknown error');
            status.textContent = otaProgressText.textContent;
            captureSequenceRunning = false;
            snapshot.disabled = false;
          }
        }
      } catch (err) {
        clearInterval(otaPollTimer);
        otaPollTimer = null;
        otaProgress.hidden = false;
        otaProgressText.textContent = 'Update status lost - ' + err.message;
        status.textContent = otaProgressText.textContent;
      }
    }

    function startOtaPolling() {
      if (otaPollTimer) clearInterval(otaPollTimer);
      pollOtaStatus();
      otaPollTimer = setInterval(pollOtaStatus, 1000);
    }

    async function loadInfo() {
      try {
        const response = await fetch('/info?t=' + Date.now(), { cache: 'no-store' });
        const info = await response.json();
        if (response.ok) {
          firmwareVersion.textContent = 'v' + info.version;
        }
      } catch (err) {
        firmwareVersion.textContent = 'unknown';
      }
    }

    async function loadSettings() {
      const response = await fetch('/settings');
      if (!response.ok) return;
      const settings = await response.json();
      framesize.value = settings.framesize;
    }

    async function applySettings() {
      status.textContent = 'Applying settings...';
      const params = new URLSearchParams({
        framesize: framesize.value
      });
      const response = await fetch('/settings?' + params.toString());
      if (response.ok) {
        status.textContent = 'Settings applied';
      } else {
        status.textContent = 'Settings failed';
      }
    }

    framesize.addEventListener('change', applySettings);

    updateCheck.addEventListener('click', async () => {
      updateInstall.hidden = true;
      status.textContent = 'Checking for update...';
      try {
        const response = await fetch('/ota/check');
        const info = await response.json();
        if (!response.ok) throw new Error(info.error || 'Update check failed');
        if (info.available) {
          status.textContent = 'Update ' + info.latest + ' available';
          updateInstall.hidden = false;
        } else {
          status.textContent = 'Firmware is current: ' + info.current;
        }
      } catch (err) {
        status.textContent = err.message;
      }
    });

    updateInstall.addEventListener('click', async () => {
      updateInstall.hidden = true;
      status.textContent = 'Starting update...';
      otaProgress.hidden = false;
      otaProgressFill.style.width = '0%';
      otaProgressText.textContent = '0% - Starting update...';
      if (snapshotTimer) clearTimeout(snapshotTimer);
      try {
        const response = await fetch('/ota/start');
        const info = await response.json();
        if (!response.ok) throw new Error(info.error || 'Update failed to start');
        setOtaProgress(info);
        startOtaPolling();
      } catch (err) {
        status.textContent = err.message;
        otaProgressText.textContent = 'Update failed - ' + err.message;
        captureSequenceRunning = false;
        snapshot.disabled = false;
      }
    });

    snapshot.addEventListener('click', () => {
      startSnapshots();
    });

    loadInfo();
    loadSettings();
  </script>
</body>
</html>
)rawliteral";

static String json_escape(const String &value);
static esp_err_t send_json(httpd_req_t *req, const String &json);
static void stop_camera_for_ota();
static void restart_camera_after_ota_failure();

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t info_handler(httpd_req_t *req) {
  String json = "{\"version\":\"" + json_escape(FIRMWARE_VERSION) + "\"}";
  return send_json(req, json);
}

static const char *frame_size_value(framesize_t frame_size) {
  for (size_t i = 0; i < sizeof(FRAME_SIZE_OPTIONS) / sizeof(FRAME_SIZE_OPTIONS[0]); i++) {
    if (FRAME_SIZE_OPTIONS[i].frame_size == frame_size) {
      return FRAME_SIZE_OPTIONS[i].value;
    }
  }

  return "qxga";
}

static bool parse_frame_size(const char *value, framesize_t *frame_size) {
  for (size_t i = 0; i < sizeof(FRAME_SIZE_OPTIONS) / sizeof(FRAME_SIZE_OPTIONS[0]); i++) {
    if (strcasecmp(value, FRAME_SIZE_OPTIONS[i].value) == 0) {
      *frame_size = FRAME_SIZE_OPTIONS[i].frame_size;
      return true;
    }
  }

  return false;
}

static void apply_power_saving_settings() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
}

static void yield_to_system() {
  delay(1);
}

static String json_escape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else {
      escaped += c;
    }
  }

  return escaped;
}

static esp_err_t send_json(httpd_req_t *req, const String &json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, json.c_str());
}

static esp_err_t send_json_error(httpd_req_t *req, httpd_err_code_t status, const String &message) {
  httpd_resp_set_status(req, status == HTTPD_400_BAD_REQUEST ? "400 Bad Request" : "500 Internal Server Error");
  return send_json(req, "{\"error\":\"" + json_escape(message) + "\"}");
}

static void copy_ota_text(char *destination, size_t size, const String &value) {
  if (size == 0) {
    return;
  }

  strncpy(destination, value.c_str(), size - 1);
  destination[size - 1] = '\0';
}

static void set_ota_progress(const char *phase, int progress, const String &message, const String &version = "") {
  if (!ota_mutex) {
    ota_mutex = xSemaphoreCreateMutex();
  }

  if (ota_mutex) {
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
  }

  ota_progress.progress = progress < 0 ? 0 : (progress > 100 ? 100 : progress);
  copy_ota_text(ota_progress.phase, sizeof(ota_progress.phase), phase);
  copy_ota_text(ota_progress.message, sizeof(ota_progress.message), message);
  if (version.length() > 0) {
    copy_ota_text(ota_progress.version, sizeof(ota_progress.version), version);
  }

  if (ota_mutex) {
    xSemaphoreGive(ota_mutex);
  }
}

static void finish_ota_progress(bool success, bool rebooting, const String &message) {
  if (!ota_mutex) {
    ota_mutex = xSemaphoreCreateMutex();
  }

  if (ota_mutex) {
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
  }

  ota_progress.running = false;
  ota_progress.success = success;
  ota_progress.rebooting = rebooting;
  ota_progress.progress = success ? 100 : ota_progress.progress;
  copy_ota_text(ota_progress.phase, sizeof(ota_progress.phase), success ? "complete" : "failed");
  copy_ota_text(ota_progress.message, sizeof(ota_progress.message), message);

  if (ota_mutex) {
    xSemaphoreGive(ota_mutex);
  }
}

static String ota_status_json() {
  if (!ota_mutex) {
    ota_mutex = xSemaphoreCreateMutex();
  }

  OtaProgress snapshot;
  if (ota_mutex) {
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
  }
  snapshot = ota_progress;
  if (ota_mutex) {
    xSemaphoreGive(ota_mutex);
  }

  return "{\"running\":" + String(snapshot.running ? "true" : "false") +
         ",\"success\":" + String(snapshot.success ? "true" : "false") +
         ",\"rebooting\":" + String(snapshot.rebooting ? "true" : "false") +
         ",\"progress\":" + String(snapshot.progress) +
         ",\"phase\":\"" + json_escape(snapshot.phase) +
         "\",\"message\":\"" + json_escape(snapshot.message) +
         "\",\"version\":\"" + json_escape(snapshot.version) + "\"}";
}

static int next_version_part(const char **cursor) {
  while (**cursor && !isdigit(**cursor)) {
    (*cursor)++;
  }

  int value = 0;
  while (**cursor && isdigit(**cursor)) {
    value = (value * 10) + (**cursor - '0');
    (*cursor)++;
  }

  if (**cursor == '.') {
    (*cursor)++;
  }

  return value;
}

static int compare_versions(const String &available, const String &current) {
  const char *left = available.c_str();
  const char *right = current.c_str();

  for (int i = 0; i < 3; i++) {
    int left_part = next_version_part(&left);
    int right_part = next_version_part(&right);

    if (left_part > right_part) {
      return 1;
    }
    if (left_part < right_part) {
      return -1;
    }
  }

  return 0;
}

static String sha256_to_hex(const uint8_t hash[32]) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  String result;
  result.reserve(64);

  for (size_t i = 0; i < 32; i++) {
    result += HEX_DIGITS[(hash[i] >> 4) & 0x0F];
    result += HEX_DIGITS[hash[i] & 0x0F];
  }

  return result;
}

static bool fetch_ota_manifest(OtaManifest &manifest, String &error) {
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();
  set_ota_progress("manifest", 5, "Fetching update manifest...");

  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String manifest_url = String(OTA_MANIFEST_URL) + "?t=" + String(millis());

  if (!http.begin(client, manifest_url)) {
    error = "Unable to open OTA manifest URL";
    return false;
  }

  int code = http.GET();
  delay(1);
  if (code != HTTP_CODE_OK) {
    error = "Manifest request failed: HTTP " + String(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError json_error = deserializeJson(doc, payload);
  if (json_error) {
    error = "Manifest JSON parse failed";
    return false;
  }

  manifest.version = String(doc["version"] | "");
  manifest.url = String(doc["url"] | "");
  manifest.sha256 = String(doc["sha256"] | "");
  manifest.sha256.toLowerCase();

  if (manifest.version.length() == 0 || manifest.url.length() == 0 || manifest.sha256.length() != 64) {
    error = "Manifest must include version, url, and 64-character sha256";
    return false;
  }

  set_ota_progress("manifest", 12, "Manifest loaded: " + manifest.version, manifest.version);
  return true;
}

static bool perform_ota_update(const OtaManifest &manifest, String &error) {
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();
  set_ota_progress("download", 15, "Opening firmware download...", manifest.version);

  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, manifest.url)) {
    error = "Unable to open firmware URL";
    return false;
  }

  int code = http.GET();
  delay(1);
  if (code != HTTP_CODE_OK) {
    error = "Firmware request failed: HTTP " + String(code);
    http.end();
    return false;
  }

  int content_length = http.getSize();
  size_t update_size = content_length > 0 ? static_cast<size_t>(content_length) : UPDATE_SIZE_UNKNOWN;
  set_ota_progress("download", 18, content_length > 0 ? "Downloading " + String(content_length) + " bytes..." : "Downloading firmware...", manifest.version);
  Serial.printf("OTA size: %d bytes\n", content_length);
  yield_to_system();

  Serial.println("OTA begin...");
  if (!Update.begin(update_size)) {
    error = "OTA begin failed: " + String(Update.errorString());
    http.end();
    return false;
  }
  Serial.println("OTA begin OK");
  yield_to_system();

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[OTA_BUFFER_SIZE];
  uint8_t hash[32];
  mbedtls_sha256_context sha_context;
  mbedtls_sha256_init(&sha_context);
  mbedtls_sha256_starts_ret(&sha_context, 0);

  size_t written = 0;
  int remaining = content_length;
  unsigned long last_data_ms = millis();
  int last_reported_progress = 18;

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    size_t available = stream->available();

    if (available > 0) {
      size_t read_size = available > OTA_BUFFER_SIZE ? OTA_BUFFER_SIZE : available;
      int bytes_read = stream->read(buffer, read_size);

      if (bytes_read <= 0) {
        delay(1);
        continue;
      }

      yield_to_system();
      size_t update_written = Update.write(buffer, bytes_read);
      yield_to_system();
      if (update_written != static_cast<size_t>(bytes_read)) {
        error = "OTA write failed: " + String(Update.errorString());
        Update.abort();
        http.end();
        mbedtls_sha256_free(&sha_context);
        return false;
      }

      mbedtls_sha256_update_ret(&sha_context, buffer, bytes_read);
      written += bytes_read;
      if (remaining > 0) {
        remaining -= bytes_read;
      }
      if (content_length > 0) {
        int progress = 20 + static_cast<int>((static_cast<uint64_t>(written) * 65ULL) / static_cast<uint64_t>(content_length));
        if (progress > last_reported_progress) {
          last_reported_progress = progress;
          set_ota_progress("download", progress, "Downloaded " + String(written) + " of " + String(content_length) + " bytes", manifest.version);
          Serial.printf("OTA progress: %d%% (%u/%d bytes)\n", progress, static_cast<unsigned int>(written), content_length);
        }
      } else if (written % 32768 < OTA_BUFFER_SIZE) {
        set_ota_progress("download", 50, "Downloaded " + String(written) + " bytes", manifest.version);
      }
      last_data_ms = millis();
      delay(1);
    } else {
      if (millis() - last_data_ms > 20000) {
        error = "Firmware download timed out";
        Update.abort();
        http.end();
        mbedtls_sha256_free(&sha_context);
        return false;
      }
      delay(10);
    }
  }

  set_ota_progress("verify", 88, "Verifying firmware hash...", manifest.version);
  delay(1);
  mbedtls_sha256_finish_ret(&sha_context, hash);
  mbedtls_sha256_free(&sha_context);
  http.end();

  String actual_sha256 = sha256_to_hex(hash);
  if (!actual_sha256.equalsIgnoreCase(manifest.sha256)) {
    error = "SHA-256 mismatch";
    Update.abort();
    return false;
  }

  set_ota_progress("finalize", 96, "Finalizing firmware update...", manifest.version);
  delay(1);
  if (!Update.end(true)) {
    error = "OTA finalize failed: " + String(Update.errorString());
    return false;
  }

  Serial.printf("OTA update written: %u bytes, version %s\n", static_cast<unsigned int>(written), manifest.version.c_str());
  set_ota_progress("complete", 100, "Update installed. Rebooting...", manifest.version);
  return true;
}

static esp_err_t ota_check_handler(httpd_req_t *req) {
  OtaManifest manifest;
  String error;

  if (!fetch_ota_manifest(manifest, error)) {
    return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error);
  }

  bool available = compare_versions(manifest.version, FIRMWARE_VERSION) > 0;
  String json = "{\"current\":\"" + json_escape(FIRMWARE_VERSION) +
                "\",\"latest\":\"" + json_escape(manifest.version) +
                "\",\"available\":" + String(available ? "true" : "false") + "}";
  return send_json(req, json);
}

static void ota_update_task(void *parameter) {
  OtaManifest manifest;
  String error;

  setCpuFrequencyMhz(240);
  esp_wifi_set_ps(WIFI_PS_NONE);
  set_ota_progress("starting", 1, "Starting update...");
  yield_to_system();

  if (!fetch_ota_manifest(manifest, error)) {
    finish_ota_progress(false, false, error);
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelete(nullptr);
    return;
  }

  if (compare_versions(manifest.version, FIRMWARE_VERSION) <= 0) {
    finish_ota_progress(false, false, "Already current: " + String(FIRMWARE_VERSION));
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("Starting OTA update from %s to %s\n", FIRMWARE_VERSION, manifest.version.c_str());
  stop_camera_for_ota();
  if (!perform_ota_update(manifest, error)) {
    finish_ota_progress(false, false, error);
    restart_camera_after_ota_failure();
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    vTaskDelete(nullptr);
    return;
  }

  finish_ota_progress(true, true, "Update installed. Rebooting...");
  delay(2000);
  ESP.restart();
}

static esp_err_t ota_status_handler(httpd_req_t *req) {
  return send_json(req, ota_status_json());
}

static esp_err_t ota_start_handler(httpd_req_t *req) {
  if (!ota_mutex) {
    ota_mutex = xSemaphoreCreateMutex();
  }

  bool running = false;
  if (ota_mutex) {
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    running = ota_progress.running;
    if (!running) {
      ota_progress.running = true;
      ota_progress.success = false;
      ota_progress.rebooting = false;
      ota_progress.progress = 0;
      copy_ota_text(ota_progress.phase, sizeof(ota_progress.phase), "queued");
      copy_ota_text(ota_progress.message, sizeof(ota_progress.message), "Update queued...");
      copy_ota_text(ota_progress.version, sizeof(ota_progress.version), "");
    }
    xSemaphoreGive(ota_mutex);
  }

  if (running) {
    return send_json(req, ota_status_json());
  }

  BaseType_t created = xTaskCreate(ota_update_task, "ota_update", OTA_TASK_STACK_SIZE, nullptr, 1, nullptr);
  if (created != pdPASS) {
    finish_ota_progress(false, false, "Unable to start update task");
    return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to start update task");
  }

  return send_json(req, ota_status_json());
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  return ota_start_handler(req);
}

static esp_err_t send_settings_json(httpd_req_t *req) {
  char response[96];
  snprintf(response, sizeof(response), "{\"framesize\":\"%s\"}", frame_size_value(current_frame_size));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, response);
}

static esp_err_t settings_handler(httpd_req_t *req) {
  char query[96] = {0};
  char frame_size_value_buffer[16] = {0};
  bool changed = false;

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return send_settings_json(req);
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!camera_running || !sensor) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  if (httpd_query_key_value(query, "framesize", frame_size_value_buffer, sizeof(frame_size_value_buffer)) == ESP_OK) {
    framesize_t requested_frame_size;
    if (!parse_frame_size(frame_size_value_buffer, &requested_frame_size)) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid framesize");
      return ESP_FAIL;
    }

    if (sensor->set_framesize(sensor, requested_frame_size) == 0) {
      current_frame_size = requested_frame_size;
      changed = true;
    }
  }

  if (changed) {
    Serial.printf("Camera settings updated: framesize=%s quality=%d\n", frame_size_value(current_frame_size), STREAM_JPEG_QUALITY);
  }

  return send_settings_json(req);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  if (!camera_running) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "Camera unavailable during update");
    return ESP_FAIL;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=birdcam.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  esp_err_t result = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static bool start_camera() {
  camera_config_t config = {};
  bool has_psram = psramFound();

  Serial.printf("PSRAM:       %s\n", has_psram ? "detected" : "not detected");
  Serial.printf("Free heap:   %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM:  %u bytes\n", ESP.getFreePsram());

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = CAMERA_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = STREAM_JPEG_QUALITY;
  config.fb_count = 1;
  config.frame_size = STREAM_FRAME_SIZE;

  if (has_psram) {
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    Serial.printf("Detected camera PID: 0x%04x\n", sensor->id.PID);
  }

  if (sensor && sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
  }

  if (sensor) {
    sensor->set_framesize(sensor, STREAM_FRAME_SIZE);
    sensor->set_quality(sensor, STREAM_JPEG_QUALITY);
    current_frame_size = STREAM_FRAME_SIZE;
  }

  camera_running = true;
  return true;
}

static void stop_camera_for_ota() {
  if (!camera_running) {
    return;
  }

  Serial.println("Stopping camera before OTA flash write...");
  set_ota_progress("camera", 14, "Stopping camera for update...");
  esp_camera_deinit();
  camera_running = false;
  delay(100);
  yield_to_system();
}

static void restart_camera_after_ota_failure() {
  if (camera_running) {
    return;
  }

  Serial.println("Restarting camera after OTA failure...");
  start_camera();
}

static void load_wifi_credentials() {
  Preferences preferences;
  preferences.begin("birdcam", false);

  wifi_ssid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  wifi_password = preferences.isKey("password") ? preferences.getString("password", "") : "";

  if (wifi_ssid.length() == 0 && strlen(DEFAULT_WIFI_SSID) > 0) {
    wifi_ssid = DEFAULT_WIFI_SSID;
    wifi_password = DEFAULT_WIFI_PASSWORD;
    preferences.putString("ssid", wifi_ssid);
    preferences.putString("password", wifi_password);
    Serial.println("Stored Wi-Fi credentials in NVS for future OTA firmware.");
  } else if (wifi_ssid.length() > 0) {
    Serial.println("Loaded Wi-Fi credentials from NVS.");
  } else {
    Serial.println("No Wi-Fi credentials found. Starting access point fallback.");
  }

  preferences.end();
}

static IPAddress start_wifi() {
  load_wifi_credentials();

  if (wifi_ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.printf("Connecting to Wi-Fi network %s", wifi_ssid.c_str());

    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Connected. Signal: %d dBm\n", WiFi.RSSI());
      return WiFi.localIP();
    }

    Serial.println("Wi-Fi connection failed. Starting access point instead.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(true);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  return WiFi.softAPIP();
}

static void print_camera_urls() {
  String ip = camera_ip.toString();

  Serial.println();
  Serial.println("================================");
  Serial.println("BirdCAM ready");
  Serial.printf("CPU clock:   %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("IP address:  %s\n", ip.c_str());
  Serial.printf("Web page:    http://%s/\n", ip.c_str());
  Serial.printf("Snapshot:    http://%s/capture\n", ip.c_str());

  if (WiFi.getMode() == WIFI_AP) {
    Serial.printf("Wi-Fi AP:    %s\n", AP_SSID);
    Serial.printf("AP password: %s\n", AP_PASSWORD);
  } else {
    Serial.printf("Wi-Fi SSID:  %s\n", WiFi.SSID().c_str());
    Serial.printf("Signal:      %d dBm\n", WiFi.RSSI());
  }

  Serial.println("================================");
}

static void start_web_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {};
  index_uri.uri = "/";
  index_uri.method = HTTP_GET;
  index_uri.handler = index_handler;

  httpd_uri_t info_uri = {};
  info_uri.uri = "/info";
  info_uri.method = HTTP_GET;
  info_uri.handler = info_handler;

  httpd_uri_t capture_uri = {};
  capture_uri.uri = "/capture";
  capture_uri.method = HTTP_GET;
  capture_uri.handler = capture_handler;

  httpd_uri_t settings_uri = {};
  settings_uri.uri = "/settings";
  settings_uri.method = HTTP_GET;
  settings_uri.handler = settings_handler;

  httpd_uri_t ota_check_uri = {};
  ota_check_uri.uri = "/ota/check";
  ota_check_uri.method = HTTP_GET;
  ota_check_uri.handler = ota_check_handler;

  httpd_uri_t ota_update_uri = {};
  ota_update_uri.uri = "/ota/update";
  ota_update_uri.method = HTTP_GET;
  ota_update_uri.handler = ota_update_handler;

  httpd_uri_t ota_start_uri = {};
  ota_start_uri.uri = "/ota/start";
  ota_start_uri.method = HTTP_GET;
  ota_start_uri.handler = ota_start_handler;

  httpd_uri_t ota_status_uri = {};
  ota_status_uri.uri = "/ota/status";
  ota_status_uri.method = HTTP_GET;
  ota_status_uri.handler = ota_status_handler;

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &info_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
    httpd_register_uri_handler(camera_httpd, &ota_check_uri);
    httpd_register_uri_handler(camera_httpd, &ota_update_uri);
    httpd_register_uri_handler(camera_httpd, &ota_start_uri);
    httpd_register_uri_handler(camera_httpd, &ota_status_uri);
  }

}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  delay(1000);
  Serial.println();
  Serial.println("Booting BirdCAM...");
  apply_power_saving_settings();
  Serial.println("Power saving: Bluetooth off, CPU clock lowered, Wi-Fi modem sleep enabled");

  Serial.println("Starting camera...");
  if (!start_camera()) {
    delay(5000);
    ESP.restart();
  }

  Serial.println("Starting Wi-Fi...");
  camera_ip = start_wifi();
  Serial.println("Starting web server...");
  start_web_server();
  print_camera_urls();
}

void loop() {
  if (millis() - last_ip_print_ms > IP_PRINT_INTERVAL_MS) {
    last_ip_print_ms = millis();
    print_camera_urls();
  }

  delay(1000);
}
