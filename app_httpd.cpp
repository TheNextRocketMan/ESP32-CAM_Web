#include "Arduino.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "board_config.h"
#include "FS.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// =====================================================
// Recording settings
// =====================================================

static volatile bool isRecording = false;
static volatile bool recordingBusy = false;

static String currentRecordingFolder = "";
static uint32_t frameNumber = 0;

static TaskHandle_t recordingTaskHandle = NULL;
static SemaphoreHandle_t cameraMutex = NULL;

httpd_handle_t camera_httpd = NULL;

// Page-selected full-resolution setting.
static framesize_t currentFrameSize = FRAMESIZE_QVGA;
static String currentFrameSizeName = "QVGA";

// Actual sensor setting we believe is currently active.
static framesize_t activeSensorFrameSize = FRAMESIZE_QVGA;
static String activeSensorFrameSizeName = "QVGA";

// Preview returned to browser.
static const framesize_t WEB_PREVIEW_FRAME_SIZE = FRAMESIZE_QQVGA;
static const char *WEB_PREVIEW_FRAME_SIZE_NAME = "QQVGA";

// Latest preview image stored temporarily in RAM.
static uint8_t *latestPreviewBuffer = NULL;
static size_t latestPreviewLength = 0;
static uint32_t latestPreviewId = 0;

// =====================================================
// Resolution helpers
// =====================================================

static framesize_t getFrameSizeFromName(const char *name, bool *valid) {
  *valid = true;

  if (!strcmp(name, "QQVGA")) return FRAMESIZE_QQVGA;
  if (!strcmp(name, "QVGA"))  return FRAMESIZE_QVGA;
  if (!strcmp(name, "VGA"))   return FRAMESIZE_VGA;
  if (!strcmp(name, "SVGA"))  return FRAMESIZE_SVGA;
  if (!strcmp(name, "XGA"))   return FRAMESIZE_XGA;
  if (!strcmp(name, "SXGA"))  return FRAMESIZE_SXGA;
  if (!strcmp(name, "UXGA"))  return FRAMESIZE_UXGA;

  *valid = false;
  return FRAMESIZE_QVGA;
}

static const char *getFrameSizeName(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA: return "QQVGA";
    case FRAMESIZE_QVGA:  return "QVGA";
    case FRAMESIZE_VGA:   return "VGA";
    case FRAMESIZE_SVGA:  return "SVGA";
    case FRAMESIZE_XGA:   return "XGA";
    case FRAMESIZE_SXGA:  return "SXGA";
    case FRAMESIZE_UXGA:  return "UXGA";
    default:              return "UNKNOWN";
  }
}

// =====================================================
// HTTP response helpers
// =====================================================

static void setCommonHeaders(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
}

static esp_err_t sendText(httpd_req_t *req, const char *status, const String &message) {
  httpd_resp_set_type(req, "text/plain");
  setCommonHeaders(req);

  if (status != NULL) {
    httpd_resp_set_status(req, status);
  }

  return httpd_resp_sendstr(req, message.c_str());
}

static esp_err_t sendJson(httpd_req_t *req, const char *status, const String &json) {
  httpd_resp_set_type(req, "application/json");
  setCommonHeaders(req);

  if (status != NULL) {
    httpd_resp_set_status(req, status);
  }

  return httpd_resp_sendstr(req, json.c_str());
}

static String jsonEscape(const String &input) {
  String output = "";

  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);

    if (c == '\\') {
      output += "\\\\";
    } else if (c == '"') {
      output += "\\\"";
    } else if (c == '\n') {
      output += "\\n";
    } else if (c == '\r') {
      output += "\\r";
    } else if (c == '\t') {
      output += "\\t";
    } else {
      output += c;
    }
  }

  return output;
}

// =====================================================
// Camera mutex helpers
// =====================================================

static bool lockCamera(uint32_t timeoutMs) {
  if (cameraMutex == NULL) {
    return true;
  }

  return xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void unlockCamera() {
  if (cameraMutex != NULL) {
    xSemaphoreGive(cameraMutex);
  }
}

// =====================================================
// Camera frame flushing
// =====================================================

static bool discardOneFrame(uint32_t timeoutPauseMs) {
  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    if (timeoutPauseMs > 0) {
      delay(timeoutPauseMs);
    }
    return false;
  }

  esp_camera_fb_return(fb);
  return true;
}

static bool ensureSensorFrameSize(framesize_t targetSize, uint8_t framesToFlush, uint32_t settleDelayMs) {
  sensor_t *s = esp_camera_sensor_get();

  if (s == NULL) {
    return false;
  }

  if (activeSensorFrameSize != targetSize) {
    int result = s->set_framesize(s, targetSize);

    if (result != 0) {
      return false;
    }

    activeSensorFrameSize = targetSize;
    activeSensorFrameSizeName = getFrameSizeName(targetSize);

    if (settleDelayMs > 0) {
      delay(settleDelayMs);
    }

    for (uint8_t i = 0; i < framesToFlush; i++) {
      discardOneFrame(30);
      delay(30);
    }
  }

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  return true;
}

// =====================================================
// Simple web page
// =====================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-CAM SD Recorder</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">

  <style>
    body {
      background: #111;
      color: #f2f2f2;
      font-family: Arial, sans-serif;
      text-align: center;
      padding: 24px;
    }

    h1 {
      font-size: 28px;
      margin-bottom: 24px;
    }

    label {
      display: block;
      font-size: 18px;
      margin-bottom: 8px;
    }

    select {
      display: block;
      width: 90%;
      max-width: 320px;
      margin: 10px auto 22px auto;
      padding: 12px;
      font-size: 18px;
      border-radius: 8px;
    }

    button {
      display: block;
      width: 90%;
      max-width: 320px;
      margin: 14px auto;
      padding: 16px;
      font-size: 20px;
      border: none;
      border-radius: 10px;
      color: white;
      cursor: pointer;
    }

    button:disabled {
      opacity: 0.45;
      cursor: not-allowed;
    }

    #startButton {
      background: #168a36;
    }

    #stopButton {
      background: #b71c1c;
    }

    #captureButton {
      background: #1565c0;
    }

    #pingButton {
      background: #6a1b9a;
    }

    #status {
      margin-top: 22px;
      font-size: 18px;
      min-height: 28px;
      line-height: 1.4;
    }

    #pingStatus {
      margin-top: 8px;
      font-size: 16px;
      color: #cccccc;
      min-height: 22px;
    }

    #capturedImage {
      display: none;
      margin-top: 24px;
      max-width: 95%;
      border: 2px solid #f2f2f2;
      border-radius: 8px;
    }

    #logBox {
      width: 90%;
      max-width: 620px;
      margin: 24px auto 0 auto;
      padding: 12px;
      background: #1b1b1b;
      border: 1px solid #444;
      border-radius: 8px;
      text-align: left;
      font-family: Consolas, monospace;
      font-size: 13px;
      white-space: pre-wrap;
      min-height: 120px;
      max-height: 300px;
      overflow-y: auto;
    }
  </style>
</head>

<body>
  <h1>ESP32-CAM SD Recorder</h1>

  <label for="resolutionSelect">Image Resolution</label>

  <select id="resolutionSelect" onchange="setResolution()">
    <option value="QQVGA">QQVGA - 160 x 120</option>
    <option value="QVGA" selected>QVGA - 320 x 240</option>
    <option value="VGA">VGA - 640 x 480</option>
    <option value="SVGA">SVGA - 800 x 600</option>
    <option value="XGA">XGA - 1024 x 768</option>
    <option value="SXGA">SXGA - 1280 x 1024</option>
    <option value="UXGA">UXGA - 1600 x 1200</option>
  </select>

  <button id="startButton" onclick="startRecording()">Start Recording</button>
  <button id="stopButton" onclick="stopRecording()">Stop Recording</button>
  <button id="captureButton" onclick="captureImage()">Capture Still</button>
  <button id="pingButton" onclick="pingBoard()">Ping Board</button>

  <div id="status">Status: idle</div>
  <div id="pingStatus">Ping: not tested yet</div>

  <img id="capturedImage">

  <div id="logBox">Event log:</div>

  <script>
    function setStatus(message) {
      document.getElementById("status").innerText = "Status: " + message;
    }

    function setPingStatus(message) {
      document.getElementById("pingStatus").innerText = "Ping: " + message;
    }

    function addLog(message) {
      const box = document.getElementById("logBox");
      const time = new Date().toLocaleTimeString();
      box.textContent += "\n[" + time + "] " + message;
      box.scrollTop = box.scrollHeight;
    }

    function setBusy(isBusy) {
      document.getElementById("startButton").disabled = isBusy;
      document.getElementById("stopButton").disabled = isBusy;
      document.getElementById("captureButton").disabled = isBusy;
      document.getElementById("pingButton").disabled = isBusy;
      document.getElementById("resolutionSelect").disabled = isBusy;
    }

    function addCacheBuster(url) {
      const separator = url.includes("?") ? "&" : "?";
      return url + separator + "t=" + Date.now();
    }

    async function fetchWithTimeout(url, timeoutMs) {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), timeoutMs);

      try {
        return await fetch(addCacheBuster(url), {
          method: "GET",
          cache: "no-store",
          signal: controller.signal
        });
      } finally {
        clearTimeout(timer);
      }
    }

    function formatFetchError(error) {
      if (error.name === "AbortError") {
        return "Timed out waiting for the board";
      }

      return error.name + ": " + error.message;
    }

    async function textCommand(commandName, url, timeoutMs = 20000) {
      setBusy(true);
      setStatus(commandName + " command sent. Waiting for board confirmation...");
      addLog(commandName + " command sent.");

      const startTime = performance.now();

      try {
        const response = await fetchWithTimeout(url, timeoutMs);
        const text = await response.text();
        const elapsed = Math.round(performance.now() - startTime);

        if (response.ok) {
          setStatus("Board confirmed: " + text + " (" + elapsed + " ms)");
          addLog("Board confirmed " + commandName + ": " + text + " | " + elapsed + " ms");
        } else {
          setStatus("Board rejected " + commandName + ": " + text + " (" + elapsed + " ms)");
          addLog("Board rejected " + commandName + ": HTTP " + response.status + " | " + text);
        }
      } catch (error) {
        const message = formatFetchError(error);
        setStatus(commandName + " failed. " + message + ".");
        addLog(commandName + " failed: " + message);
      }

      setBusy(false);
    }

    async function setResolution() {
      const selectedResolution = document.getElementById("resolutionSelect").value;

      await textCommand(
        "SET RESOLUTION " + selectedResolution,
        "/resolution?size=" + encodeURIComponent(selectedResolution),
        30000
      );
    }

    async function startRecording() {
      await textCommand("START RECORDING", "/start", 30000);
    }

    async function stopRecording() {
      await textCommand("STOP RECORDING", "/stop", 30000);
    }

    async function pingBoard() {
      setBusy(true);
      setStatus("Ping sent. Waiting for board...");
      addLog("Ping sent.");

      const startTime = performance.now();

      try {
        const response = await fetchWithTimeout("/ping", 25000);
        const text = await response.text();
        const elapsed = Math.round(performance.now() - startTime);

        if (response.ok) {
          setPingStatus(elapsed + " ms round trip");
          setStatus("Board responded to ping: " + text + " (" + elapsed + " ms)");
          addLog("Ping response: " + text + " | browser round trip: " + elapsed + " ms");
        } else {
          setPingStatus("failed");
          setStatus("Ping failed: HTTP " + response.status);
          addLog("Ping failed: HTTP " + response.status + " | " + text);
        }
      } catch (error) {
        const message = formatFetchError(error);
        setPingStatus("failed");
        setStatus("Ping failed. " + message + ".");
        addLog("Ping failed: " + message);
      }

      setBusy(false);
    }

    async function captureImage() {
      setBusy(true);
      setStatus("CAPTURE STILL command sent. Saving full-resolution image and preparing preview...");
      addLog("CAPTURE STILL command sent.");

      const startTime = performance.now();

      try {
        const response = await fetchWithTimeout("/capture", 90000);
        const elapsed = Math.round(performance.now() - startTime);
        const text = await response.text();

        let data = null;

        try {
          data = JSON.parse(text);
        } catch (parseError) {
          setStatus("Capture response was not valid JSON.");
          addLog("Capture response parse failed: " + text);
          setBusy(false);
          return;
        }

        if (!response.ok || !data.ok) {
          const errorMessage = data.message || text || "unknown capture error";
          setStatus("Board rejected capture: " + errorMessage + " (" + elapsed + " ms)");
          addLog("Board rejected capture: HTTP " + response.status + " | " + errorMessage);
          setBusy(false);
          return;
        }

        setStatus(
          "Board confirmed capture. Saved: " + data.path +
          " | full: " + data.fullResolution +
          " | preview: " + data.previewResolution +
          " (" + elapsed + " ms)"
        );

        addLog(
          "Board confirmed capture. Saved full-res image: " + data.path +
          " | full: " + data.fullResolution + " / " + data.fullBytes + " bytes" +
          " | preview: " + data.previewResolution + " / " + data.previewBytes + " bytes" +
          " | command time: " + data.captureMs + " ms" +
          " | full capture: " + data.fullCaptureMs + " ms" +
          " | SD write: " + data.sdWriteMs + " ms" +
          " | preview capture: " + data.previewCaptureMs + " ms" +
          " | browser round trip: " + elapsed + " ms"
        );

        if (data.previewAvailable && data.previewUrl) {
          const img = document.getElementById("capturedImage");

          img.onload = function() {
            addLog("Preview image loaded successfully.");
          };

          img.onerror = function() {
            addLog("Preview image failed to load, but full-resolution image was already saved to SD.");
          };

          img.src = addCacheBuster(data.previewUrl);
          img.style.display = "block";
        } else {
          addLog("No preview image was available, but full-resolution image was saved to SD.");
        }
      } catch (error) {
        const message = formatFetchError(error);
        setStatus("Capture failed. " + message + ".");
        addLog("Capture failed: " + message);
      }

      setBusy(false);
    }
  </script>
</body>
</html>
)rawliteral";

// =====================================================
// SD helpers
// =====================================================

static bool sdCardReady() {
  return SD_MMC.cardType() != CARD_NONE;
}

static String makeUniqueRecordingFolder() {
  char folderName[24];

  for (uint32_t i = 1; i < 10000; i++) {
    snprintf(folderName, sizeof(folderName), "/REC_%04lu", (unsigned long)i);

    if (!SD_MMC.exists(folderName)) {
      if (SD_MMC.mkdir(folderName)) {
        return String(folderName);
      } else {
        return "";
      }
    }
  }

  return "";
}

static String makeUniqueCapturePath() {
  char fileName[32];

  for (uint32_t i = 1; i < 10000; i++) {
    snprintf(fileName, sizeof(fileName), "/CAPTURE_%04lu.jpg", (unsigned long)i);

    if (!SD_MMC.exists(fileName)) {
      return String(fileName);
    }
  }

  return "";
}

static bool saveBufferToSD(const String &path, const uint8_t *buffer, size_t length) {
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);

  if (!file) {
    log_e("Failed to open file for writing: %s", path.c_str());
    return false;
  }

  size_t written = file.write(buffer, length);
  file.close();

  if (written != length) {
    log_e("File write incomplete: %s", path.c_str());
    return false;
  }

  return true;
}

static bool storeLatestPreview(const uint8_t *buffer, size_t length) {
  if (buffer == NULL || length == 0) {
    return false;
  }

  uint8_t *newBuffer = (uint8_t *)realloc(latestPreviewBuffer, length);

  if (newBuffer == NULL) {
    return false;
  }

  latestPreviewBuffer = newBuffer;
  memcpy(latestPreviewBuffer, buffer, length);
  latestPreviewLength = length;
  latestPreviewId++;

  return true;
}

static bool saveOneRecordingFrame() {
  if (!lockCamera(3000)) {
    log_e("Recording skipped/stopped because camera mutex could not be locked");
    return false;
  }

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    unlockCamera();
    log_e("Camera capture failed while recording");
    return false;
  }

  frameNumber++;

  char fileName[40];
  snprintf(fileName, sizeof(fileName), "/frame_%06lu.jpg", (unsigned long)frameNumber);

  String path = currentRecordingFolder + String(fileName);

  bool ok = saveBufferToSD(path, fb->buf, fb->len);

  esp_camera_fb_return(fb);
  unlockCamera();

  return ok;
}

// =====================================================
// Recording task
// =====================================================

static void recordingTask(void *parameter) {
  while (true) {
    while (isRecording) {
      recordingBusy = true;

      bool ok = saveOneRecordingFrame();

      recordingBusy = false;

      if (!ok) {
        log_e("Recording stopped because a frame could not be saved");
        isRecording = false;
        break;
      }

      vTaskDelay(1);
    }

    recordingBusy = false;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// =====================================================
// JSON builders
// =====================================================

static String makeCaptureSuccessJson(
  const String &path,
  size_t fullBytes,
  bool previewAvailable,
  size_t previewBytes,
  uint32_t captureMs,
  uint32_t fullSetupMs,
  uint32_t fullCaptureMs,
  uint32_t sdWriteMs,
  uint32_t previewSetupMs,
  uint32_t previewCaptureMs
) {
  String json = "{";
  json += "\"ok\":true,";
  json += "\"message\":\"CAPTURE command received. Full-resolution image saved. Small preview prepared.\",";
  json += "\"path\":\"";
  json += jsonEscape(path);
  json += "\",";
  json += "\"fullResolution\":\"";
  json += currentFrameSizeName;
  json += "\",";
  json += "\"activeSensorResolution\":\"";
  json += activeSensorFrameSizeName;
  json += "\",";
  json += "\"previewResolution\":\"";
  json += WEB_PREVIEW_FRAME_SIZE_NAME;
  json += "\",";
  json += "\"fullBytes\":";
  json += String((unsigned int)fullBytes);
  json += ",";
  json += "\"previewAvailable\":";
  json += previewAvailable ? "true" : "false";
  json += ",";
  json += "\"previewBytes\":";
  json += String((unsigned int)previewBytes);
  json += ",";
  json += "\"previewUrl\":\"/preview\",";
  json += "\"previewId\":";
  json += String(latestPreviewId);
  json += ",";
  json += "\"captureMs\":";
  json += String(captureMs);
  json += ",";
  json += "\"fullSetupMs\":";
  json += String(fullSetupMs);
  json += ",";
  json += "\"fullCaptureMs\":";
  json += String(fullCaptureMs);
  json += ",";
  json += "\"sdWriteMs\":";
  json += String(sdWriteMs);
  json += ",";
  json += "\"previewSetupMs\":";
  json += String(previewSetupMs);
  json += ",";
  json += "\"previewCaptureMs\":";
  json += String(previewCaptureMs);
  json += ",";
  json += "\"rssi\":";
  json += String(WiFi.RSSI());
  json += ",";
  json += "\"freeHeap\":";
  json += String(ESP.getFreeHeap());
  json += "}";

  return json;
}

static String makeErrorJson(const String &message) {
  String json = "{";
  json += "\"ok\":false,";
  json += "\"message\":\"";
  json += jsonEscape(message);
  json += "\",";
  json += "\"rssi\":";
  json += String(WiFi.RSSI());
  json += ",";
  json += "\"freeHeap\":";
  json += String(ESP.getFreeHeap());
  json += "}";

  return json;
}

// =====================================================
// HTTP handlers
// =====================================================

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  setCommonHeaders(req);
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ping_handler(httpd_req_t *req) {
  String message = "pong, board millis = ";
  message += String(millis());
  message += ", recording = ";
  message += isRecording ? "yes" : "no";
  message += ", recordingBusy = ";
  message += recordingBusy ? "yes" : "no";
  message += ", frames = ";
  message += String(frameNumber);
  message += ", selected resolution = ";
  message += currentFrameSizeName;
  message += ", active sensor resolution = ";
  message += activeSensorFrameSizeName;
  message += ", preview bytes = ";
  message += String((unsigned int)latestPreviewLength);
  message += ", RSSI = ";
  message += String(WiFi.RSSI());
  message += " dBm";
  message += ", free heap = ";
  message += String(ESP.getFreeHeap());

  return sendText(req, NULL, message);
}

static esp_err_t start_handler(httpd_req_t *req) {
  if (!sdCardReady()) {
    return sendText(req, "503 Service Unavailable", "SD card not ready");
  }

  if (isRecording) {
    return sendText(req, NULL, "START command received, but board is already recording");
  }

  if (!lockCamera(10000)) {
    return sendText(req, "503 Service Unavailable", "START command received, but camera is busy");
  }

  bool ready = ensureSensorFrameSize(currentFrameSize, 2, 120);
  unlockCamera();

  if (!ready) {
    return sendText(req, "500 Internal Server Error", "START command received, but camera failed to switch to the selected recording resolution");
  }

  currentRecordingFolder = makeUniqueRecordingFolder();

  if (currentRecordingFolder.length() == 0) {
    return sendText(req, "500 Internal Server Error", "START command received, but failed to create recording folder on SD card");
  }

  frameNumber = 0;
  isRecording = true;

  String message = "START command received. Recording started: ";
  message += currentRecordingFolder;
  message += ". Resolution: ";
  message += currentFrameSizeName;
  message += ". RSSI: ";
  message += String(WiFi.RSSI());
  message += " dBm";

  return sendText(req, NULL, message);
}

static esp_err_t stop_handler(httpd_req_t *req) {
  if (!isRecording && !recordingBusy) {
    return sendText(req, NULL, "STOP command received, but board was not recording");
  }

  isRecording = false;

  uint32_t waitStart = millis();

  while (recordingBusy && (millis() - waitStart < 3000)) {
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }

  String message = "STOP command received. Recording stopped: ";
  message += currentRecordingFolder;
  message += ", frames saved: ";
  message += String(frameNumber);

  if (recordingBusy) {
    message += ". Warning: final frame was still finishing when this response was sent.";
  }

  return sendText(req, NULL, message);
}

static esp_err_t preview_handler(httpd_req_t *req) {
  if (latestPreviewBuffer == NULL || latestPreviewLength == 0) {
    return sendText(req, "404 Not Found", "No preview image is available yet");
  }

  httpd_resp_set_type(req, "image/jpeg");
  setCommonHeaders(req);
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=preview.jpg");

  return httpd_resp_send(req, (const char *)latestPreviewBuffer, latestPreviewLength);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  uint32_t captureStartMs = millis();

  uint32_t fullSetupMs = 0;
  uint32_t fullCaptureMs = 0;
  uint32_t sdWriteMs = 0;
  uint32_t previewSetupMs = 0;
  uint32_t previewCaptureMs = 0;

  if (isRecording || recordingBusy) {
    return sendJson(
      req,
      "409 Conflict",
      makeErrorJson("CAPTURE command received, but stop recording before capturing a still image")
    );
  }

  if (!sdCardReady()) {
    return sendJson(
      req,
      "503 Service Unavailable",
      makeErrorJson("CAPTURE command received, but SD card is not ready")
    );
  }

  if (!lockCamera(15000)) {
    return sendJson(
      req,
      "503 Service Unavailable",
      makeErrorJson("CAPTURE command received, but camera is busy")
    );
  }

  uint32_t t0 = millis();

  bool fullReady = ensureSensorFrameSize(currentFrameSize, 2, 120);

  fullSetupMs = millis() - t0;

  if (!fullReady) {
    unlockCamera();
    return sendJson(
      req,
      "500 Internal Server Error",
      makeErrorJson("CAPTURE command received, but camera failed to switch to selected full resolution")
    );
  }

  t0 = millis();

  camera_fb_t *fullFb = esp_camera_fb_get();

  fullCaptureMs = millis() - t0;

  if (!fullFb) {
    unlockCamera();
    return sendJson(
      req,
      "500 Internal Server Error",
      makeErrorJson("CAPTURE command received, but full-resolution camera capture failed")
    );
  }

  String capturePath = makeUniqueCapturePath();

  if (capturePath.length() == 0) {
    esp_camera_fb_return(fullFb);
    unlockCamera();
    return sendJson(
      req,
      "500 Internal Server Error",
      makeErrorJson("CAPTURE command received, but failed to create capture file path")
    );
  }

  size_t fullBytes = fullFb->len;

  t0 = millis();

  bool saved = saveBufferToSD(capturePath, fullFb->buf, fullFb->len);

  sdWriteMs = millis() - t0;

  esp_camera_fb_return(fullFb);
  fullFb = NULL;

  if (!saved) {
    unlockCamera();
    return sendJson(
      req,
      "500 Internal Server Error",
      makeErrorJson("CAPTURE command received, but failed to save full-resolution image to SD card")
    );
  }

  bool previewAvailable = false;
  size_t previewBytes = 0;

  t0 = millis();

  bool previewReady = ensureSensorFrameSize(WEB_PREVIEW_FRAME_SIZE, 3, 160);

  previewSetupMs = millis() - t0;

  if (previewReady) {
    t0 = millis();

    camera_fb_t *previewFb = esp_camera_fb_get();

    previewCaptureMs = millis() - t0;

    if (previewFb) {
      previewBytes = previewFb->len;
      previewAvailable = storeLatestPreview(previewFb->buf, previewFb->len);

      esp_camera_fb_return(previewFb);
      previewFb = NULL;
    }
  }

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  unlockCamera();

  uint32_t captureMs = millis() - captureStartMs;

  String json = makeCaptureSuccessJson(
    capturePath,
    fullBytes,
    previewAvailable,
    previewBytes,
    captureMs,
    fullSetupMs,
    fullCaptureMs,
    sdWriteMs,
    previewSetupMs,
    previewCaptureMs
  );

  return sendJson(req, NULL, json);
}

static esp_err_t resolution_handler(httpd_req_t *req) {
  if (isRecording || recordingBusy) {
    return sendText(req, "409 Conflict", "RESOLUTION command received, but stop recording before changing resolution");
  }

  char query[96];
  char sizeName[16];

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return sendText(req, "400 Bad Request", "RESOLUTION command received, but resolution query is missing");
  }

  if (httpd_query_key_value(query, "size", sizeName, sizeof(sizeName)) != ESP_OK) {
    return sendText(req, "400 Bad Request", "RESOLUTION command received, but size value is missing");
  }

  bool valid = false;
  framesize_t newFrameSize = getFrameSizeFromName(sizeName, &valid);

  if (!valid) {
    return sendText(req, "400 Bad Request", "RESOLUTION command received, but resolution value is invalid");
  }

  currentFrameSize = newFrameSize;
  currentFrameSizeName = getFrameSizeName(newFrameSize);

  if (!lockCamera(10000)) {
    return sendText(req, "503 Service Unavailable", "RESOLUTION command received, but camera is busy");
  }

  bool ready = ensureSensorFrameSize(currentFrameSize, 2, 120);

  unlockCamera();

  if (!ready) {
    return sendText(req, "500 Internal Server Error", "RESOLUTION command received, but camera failed to set resolution");
  }

  String message = "RESOLUTION command received. Resolution changed to ";
  message += currentFrameSizeName;
  message += ". RSSI: ";
  message += String(WiFi.RSSI());
  message += " dBm";

  return sendText(req, NULL, message);
}

// =====================================================
// Server startup
// =====================================================

void startCameraServer() {
  if (cameraMutex == NULL) {
    cameraMutex = xSemaphoreCreateMutex();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 10;
  config.stack_size = 8192;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 30;
  config.lru_purge_enable = true;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t ping_uri = {
    .uri = "/ping",
    .method = HTTP_GET,
    .handler = ping_handler,
    .user_ctx = NULL
  };

  httpd_uri_t start_uri = {
    .uri = "/start",
    .method = HTTP_GET,
    .handler = start_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stop_uri = {
    .uri = "/stop",
    .method = HTTP_GET,
    .handler = stop_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t preview_uri = {
    .uri = "/preview",
    .method = HTTP_GET,
    .handler = preview_handler,
    .user_ctx = NULL
  };

  httpd_uri_t resolution_uri = {
    .uri = "/resolution",
    .method = HTTP_GET,
    .handler = resolution_handler,
    .user_ctx = NULL
  };

  log_i("Starting web server on port: '%u'", config.server_port);

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &ping_uri);
    httpd_register_uri_handler(camera_httpd, &start_uri);
    httpd_register_uri_handler(camera_httpd, &stop_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &preview_uri);
    httpd_register_uri_handler(camera_httpd, &resolution_uri);
  }

  if (recordingTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      recordingTask,
      "recordingTask",
      8192,
      NULL,
      1,
      &recordingTaskHandle,
      1
    );
  }
}

// Kept so MyCameraWebServer.ino can still call setupLedFlash()
// if LED_GPIO_NUM is accidentally left defined.
// This version intentionally does nothing except force the flash pin LOW.
void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  log_i("LED flash setup skipped. Flash LED is intentionally unused.");
}