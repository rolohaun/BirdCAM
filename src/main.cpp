#include "Arduino.h"
#include "ArduinoJson.h"
#include "HTTPClient.h"
#include "Preferences.h"
#include "Update.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_wifi.h"
#include "img_converters.h"
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
static const char *FIRMWARE_VERSION = "0.1.0";
static const char *OTA_MANIFEST_URL = "https://raw.githubusercontent.com/rolohaun/BirdCAM/main/firmware/manifest.json";

// Solar-friendly defaults. The page dropdowns can still raise quality when needed.
static const framesize_t STREAM_FRAME_SIZE = FRAMESIZE_VGA;
static const int STREAM_JPEG_QUALITY = 25;  // 10 is sharper/slower, 30 is smaller/faster.
static const int CPU_FREQ_MHZ = 80;
static const int CAMERA_XCLK_HZ = 10000000;
static const unsigned long IP_PRINT_INTERVAL_MS = 300000;
static const size_t OTA_BUFFER_SIZE = 4096;

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
static httpd_handle_t stream_httpd = nullptr;
static IPAddress camera_ip;
static String wifi_ssid;
static String wifi_password;
static unsigned long last_ip_print_ms = 0;
static framesize_t current_frame_size = STREAM_FRAME_SIZE;
static int current_jpeg_quality = STREAM_JPEG_QUALITY;

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
    .viewer { display: grid; place-items: center; padding: 12px; }
    img { width: min(100%, 1100px); max-height: calc(100vh - 92px); object-fit: contain; background: #050607; border: 1px solid #2b3138; }
    .status { font-size: 13px; color: #bac4cf; }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>BirdCAM</h1>
        <div class="status" id="status">Live stream</div>
      </div>
      <div class="controls">
        <label>
          Resolution
          <select id="framesize">
            <option value="uxga">UXGA 1600x1200</option>
            <option value="sxga">SXGA 1280x1024</option>
            <option value="xga">XGA 1024x768</option>
            <option value="svga">SVGA 800x600</option>
            <option value="vga" selected>VGA 640x480</option>
            <option value="qvga">QVGA 320x240</option>
            <option value="qqvga">QQVGA 160x120</option>
          </select>
        </label>
        <label>
          JPEG
          <select id="quality">
            <option value="10">Best</option>
            <option value="14">High</option>
            <option value="18">Balanced</option>
            <option value="25" selected>Fast</option>
            <option value="32">Fastest</option>
          </select>
        </label>
        <a class="button" href="/capture" target="_blank">Snapshot</a>
        <button id="update-check" type="button">Check Update</button>
        <button id="update-install" type="button" hidden>Install Update</button>
        <button id="reload" type="button">Reload</button>
      </div>
    </header>
    <section class="viewer">
      <img id="stream" alt="BirdCAM live stream">
    </section>
  </main>
  <script>
    const framesize = document.getElementById('framesize');
    const quality = document.getElementById('quality');
    const updateCheck = document.getElementById('update-check');
    const updateInstall = document.getElementById('update-install');
    const reload = document.getElementById('reload');
    const stream = document.getElementById('stream');
    const status = document.getElementById('status');

    function streamUrl() {
      return location.protocol + '//' + location.hostname + ':81/stream?t=' + Date.now();
    }

    stream.src = streamUrl();

    async function loadSettings() {
      const response = await fetch('/settings');
      if (!response.ok) return;
      const settings = await response.json();
      framesize.value = settings.framesize;
      quality.value = String(settings.quality);
    }

    async function applySettings() {
      status.textContent = 'Applying settings...';
      const params = new URLSearchParams({
        framesize: framesize.value,
        quality: quality.value
      });
      const response = await fetch('/settings?' + params.toString());
      if (response.ok) {
        stream.src = streamUrl();
        status.textContent = 'Live stream';
      } else {
        status.textContent = 'Settings failed';
      }
    }

    framesize.addEventListener('change', applySettings);
    quality.addEventListener('change', applySettings);

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
      status.textContent = 'Installing update...';
      stream.removeAttribute('src');
      try {
        const response = await fetch('/ota/update');
        const info = await response.json();
        if (!response.ok) throw new Error(info.error || 'Update failed');
        status.textContent = 'Update installed. Rebooting...';
      } catch (err) {
        status.textContent = err.message;
        stream.src = streamUrl();
      }
    });

    reload.addEventListener('click', () => {
      status.textContent = 'Reconnecting...';
      stream.src = streamUrl();
      setTimeout(() => status.textContent = 'Live stream', 700);
    });

    loadSettings();
  </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static const char *frame_size_value(framesize_t frame_size) {
  for (size_t i = 0; i < sizeof(FRAME_SIZE_OPTIONS) / sizeof(FRAME_SIZE_OPTIONS[0]); i++) {
    if (FRAME_SIZE_OPTIONS[i].frame_size == frame_size) {
      return FRAME_SIZE_OPTIONS[i].value;
    }
  }

  return "uxga";
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

  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, OTA_MANIFEST_URL)) {
    error = "Unable to open OTA manifest URL";
    return false;
  }

  int code = http.GET();
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

  return true;
}

static bool perform_ota_update(const OtaManifest &manifest, String &error) {
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, manifest.url)) {
    error = "Unable to open firmware URL";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = "Firmware request failed: HTTP " + String(code);
    http.end();
    return false;
  }

  int content_length = http.getSize();
  size_t update_size = content_length > 0 ? static_cast<size_t>(content_length) : UPDATE_SIZE_UNKNOWN;

  if (!Update.begin(update_size)) {
    error = "OTA begin failed: " + String(Update.errorString());
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[OTA_BUFFER_SIZE];
  uint8_t hash[32];
  mbedtls_sha256_context sha_context;
  mbedtls_sha256_init(&sha_context);
  mbedtls_sha256_starts_ret(&sha_context, 0);

  size_t written = 0;
  int remaining = content_length;
  unsigned long last_data_ms = millis();

  while (http.connected() && (remaining > 0 || remaining == -1)) {
    size_t available = stream->available();

    if (available > 0) {
      size_t read_size = available > OTA_BUFFER_SIZE ? OTA_BUFFER_SIZE : available;
      int bytes_read = stream->readBytes(buffer, read_size);

      if (bytes_read <= 0) {
        continue;
      }

      if (Update.write(buffer, bytes_read) != static_cast<size_t>(bytes_read)) {
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
      last_data_ms = millis();
    } else {
      if (millis() - last_data_ms > 20000) {
        error = "Firmware download timed out";
        Update.abort();
        http.end();
        mbedtls_sha256_free(&sha_context);
        return false;
      }
      delay(1);
    }
  }

  mbedtls_sha256_finish_ret(&sha_context, hash);
  mbedtls_sha256_free(&sha_context);
  http.end();

  String actual_sha256 = sha256_to_hex(hash);
  if (!actual_sha256.equalsIgnoreCase(manifest.sha256)) {
    error = "SHA-256 mismatch";
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    error = "OTA finalize failed: " + String(Update.errorString());
    return false;
  }

  Serial.printf("OTA update written: %u bytes, version %s\n", static_cast<unsigned int>(written), manifest.version.c_str());
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

static esp_err_t ota_update_handler(httpd_req_t *req) {
  OtaManifest manifest;
  String error;

  if (!fetch_ota_manifest(manifest, error)) {
    return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error);
  }

  if (compare_versions(manifest.version, FIRMWARE_VERSION) <= 0) {
    return send_json(req, "{\"updated\":false,\"message\":\"Already current\",\"current\":\"" + json_escape(FIRMWARE_VERSION) + "\"}");
  }

  Serial.printf("Starting OTA update from %s to %s\n", FIRMWARE_VERSION, manifest.version.c_str());
  if (!perform_ota_update(manifest, error)) {
    return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error);
  }

  esp_err_t result = send_json(req, "{\"updated\":true,\"version\":\"" + json_escape(manifest.version) + "\"}");
  delay(1000);
  ESP.restart();
  return result;
}

static esp_err_t send_settings_json(httpd_req_t *req) {
  char response[96];
  snprintf(response, sizeof(response), "{\"framesize\":\"%s\",\"quality\":%d}", frame_size_value(current_frame_size), current_jpeg_quality);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, response);
}

static esp_err_t settings_handler(httpd_req_t *req) {
  char query[96] = {0};
  char frame_size_value_buffer[16] = {0};
  char quality_value_buffer[8] = {0};
  bool changed = false;

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return send_settings_json(req);
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
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

  if (httpd_query_key_value(query, "quality", quality_value_buffer, sizeof(quality_value_buffer)) == ESP_OK) {
    int requested_quality = atoi(quality_value_buffer);
    if (requested_quality < 4 || requested_quality > 63) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid quality");
      return ESP_FAIL;
    }

    if (sensor->set_quality(sensor, requested_quality) == 0) {
      current_jpeg_quality = requested_quality;
      changed = true;
    }
  }

  if (changed) {
    Serial.printf("Camera settings updated: framesize=%s quality=%d\n", frame_size_value(current_frame_size), current_jpeg_quality);
  }

  return send_settings_json(req);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=birdcam.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t result = httpd_resp_send(req, reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  return result;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=birdcam";
  static const char *STREAM_BOUNDARY = "\r\n--birdcam\r\n";
  static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lld.%06lld\r\n\r\n";

  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return ESP_FAIL;
    }

    uint8_t *jpg_buf = fb->buf;
    size_t jpg_len = fb->len;
    bool converted = false;

    if (fb->format != PIXFORMAT_JPEG) {
      converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
      if (!converted) {
        esp_camera_fb_return(fb);
        Serial.println("JPEG compression failed");
        return ESP_FAIL;
      }
    }

    char part_buf[96];
    int64_t timestamp = esp_timer_get_time();
    size_t header_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpg_len, timestamp / 1000000, timestamp % 1000000);

    esp_err_t result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(req, part_buf, header_len);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(jpg_buf), jpg_len);
    }

    if (converted) {
      free(jpg_buf);
    }
    esp_camera_fb_return(fb);

    if (result != ESP_OK) {
      break;
    }
  }

  return ESP_OK;
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
    current_jpeg_quality = STREAM_JPEG_QUALITY;
  }

  return true;
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
  Serial.printf("Stream URL:  http://%s:81/stream\n", ip.c_str());

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

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &settings_uri);
    httpd_register_uri_handler(camera_httpd, &ota_check_uri);
    httpd_register_uri_handler(camera_httpd, &ota_update_uri);
  }

  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;

  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream";
  stream_uri.method = HTTP_GET;
  stream_uri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
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
