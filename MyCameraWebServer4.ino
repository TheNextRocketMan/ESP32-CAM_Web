#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "FS.h"
#include "SD_MMC.h"

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "SETUP-09B1-2.4";
const char *password = "eager6685anyway";

void startCameraServer();
void setupLedFlash();

bool setupSDCard() {
  /*
    AI Thinker ESP32-CAM SD card note:

    SD_MMC.begin("/sdcard", true) means 1-bit SD mode.

    This is important because the AI Thinker ESP32-CAM has the flash LED
    on GPIO 4, and GPIO 4 is also used by the SD card in 4-bit mode.

    Using 1-bit mode avoids GPIO 4 for SD card data and helps prevent
    accidental flash LED activity or SD/LED pin conflicts.
  */

#if defined(LED_GPIO_NUM)
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed");
    return false;
  }

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  uint8_t cardType = SD_MMC.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected");
    return false;
  }

  Serial.print("SD card type: ");

  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD card size: %llu MB\n", cardSize);

  return true;
}

void setup() {
  Serial.begin(115200);

  /*
    Keep this false unless you are actively debugging camera-driver internals.
    Serial debug spam, especially at low baud rates, can make the web page
    feel extremely slow.
  */
  Serial.setDebugOutput(false);

  delay(300);
  Serial.println();
  Serial.println("Booting ESP32-CAM SD recorder...");

#if defined(LED_GPIO_NUM)
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  camera_config_t config;
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
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
      config.fb_count = 1;
      config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;

#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  esp_err_t err = esp_camera_init(&config);

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (s == NULL) {
    Serial.println("Camera sensor pointer is NULL");
    return;
  }

  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  /*
    The default page selection is QVGA.
    The web page can change this later.
  */
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  if (!setupSDCard()) {
    Serial.println("SD card is not ready. Web page will still load, but recording/capture saving will fail until SD is fixed.");
  }

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print("WiFi connecting");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif
}

void loop() {
#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, LOW);
#endif

  delay(10000);
}