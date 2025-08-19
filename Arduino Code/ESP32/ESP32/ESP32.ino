#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "Free_Fonts.h"
#include <SD.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <XPT2046_Touchscreen.h>
#include <JPEGDecoder.h>
#include "esp_task_wdt.h"
#include "esp_system.h"

// Touch Configuration
#define TOUCH_CS 5
#define TOUCH_IRQ 27
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// Display
TFT_eSPI tft = TFT_eSPI();

#define GRBL_RESET_PIN 22

// UI constants
#define BUTTON1_W 100
#define BUTTON1_H 50
#define BUTTONARROW_W 40
#define BUTTONARROW_H 40
#define BUTTONBACK_W 80
#define BUTTONBACK_H 40
#define BUTTONEXEC_W 100
#define BUTTONEXEC_H 40

// Button structure
struct Button {
  int x, y, w, h;
  uint16_t color;
  uint16_t textColor;
  char label[20];
  bool visible;
  int id;
};

bool ledRunning = false;

// Button IDs
enum ButtonID {
  BTN_NONE = 0,
  BTN_ART = 1,
  BTN_LED = 2,
  BTN_LEFT = 3,
  BTN_RIGHT = 4,
  BTN_EXECUTE = 5,
  BTN_BACK = 6,
  BTN_STOP = 7,
  BTN_PAUSE = 8
};

// Global variables for true synchronous WebSocket communication
struct PendingWebSocketCommand {
  String command;
  uint8_t clientNum;
  unsigned long timestamp;
  bool isActive;
};

PendingWebSocketCommand pendingWSCommand;
bool wsWaitingForGRBLOk = false;
unsigned long wsCommandStartTime = 0;
const unsigned long WS_COMMAND_TIMEOUT = 70000;  // 15 second timeout

// Button definitions
Button buttons[8];
int buttonCount = 0;

// States
uint8_t artIndex = 1;
int ledIndex = 0;
bool inArtPage = false;
bool inLEDPage = false;
bool isRunningGcode = false;
bool isPausedGcode = false;  // New pause state
uint8_t currentPage = 1;     // 1=menu, 2=art, 3=led
bool hasDrawn = false;

// Global variables for synchronous G-code sending
bool waitingForOk = false;
unsigned long lastCommandTime = 0;
const unsigned long commandTimeout = 30000;  // 5 second timeout per command

// *** NEW: Stop button tracking variables ***
bool stopButtonPressed = false;  // Track if stop button was pressed
bool needsHoming = false;        // Track if homing is required before next execution
bool isCurrentlyHoming = false;  // Track if homing is in progress

//image
int screenWidth = 480;
int screenHeight = 320;
int imgWidth = 200;
int imgHeight = 200;

// SD Module
#define SD_CS 21
File root;
int imageIndex = 0;

// GRBL Communication
#define GRBL_SERIAL Serial2
#define GRBL_BAUDRATE 115200
#define GRBL_TX 17
#define GRBL_RX 16
char grblBuffer[128];
int grblBufferIndex = 0;

// G-code file handling
File currentGcodeFile;
bool gcodeFileOpen = false;
char currentGcodeFilename[30];

// LED Strip
#define LED_PIN 25
#define NUM_LEDS 182
#define BRIGHTNESS 100

int currentLEDPattern = 0;  // 0 = off, 1-8 = patterns
unsigned long lastLEDUpdate = 0;
const unsigned long patternUpdateInterval = 50;
uint16_t pixelCount = NUM_LEDS;
uint8_t knightIndex = 0;
int knightDir = 1;

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// WIFI communication
const char *ssid = "";
const char *password = "";

IPAddress local_IP();
IPAddress gateway();
IPAddress subnet();
IPAddress primaryDNS();
IPAddress secondaryDNS();

WebSocketsServer webSocket = WebSocketsServer(81);

// Initialize HSPI for SD card
SPIClass hspi(HSPI);

// Single file approach - put everything in your .ino file

#define QUEUE_SIZE 60

class Queue {
private:
  int arr[QUEUE_SIZE];
  int front;
  int rear;
  int currentSize;

public:
  Queue()
    : front(0), rear(0), currentSize(0) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
      arr[i] = 0;
    }
  }

  bool isFull() const {
    return currentSize == QUEUE_SIZE;
  }

  bool enqueue(int value) {
    if (isFull()) {
      // Overwrite the oldest value
      front = (front + 1) % QUEUE_SIZE;
      currentSize--;  // avoid overflow
    }

    arr[rear] = value;
    rear = (rear + 1) % QUEUE_SIZE;
    currentSize++;

    return true;
  }


  bool hasRequiredItems() const {
    return currentSize == 60;
  }

  void clear() {
    front = rear = 0;
    currentSize = 0;
  }

  bool isValid() const {
    const int MIN_VAL = 5;
    const int MAX_VAL = 45;
    const int RANGE = MAX_VAL - MIN_VAL + 1;

    int count[RANGE] = { 0 };

    int index = front;
    for (int i = 0; i < currentSize; i++) {
      int value = arr[index];
      if (value < MIN_VAL || value > MAX_VAL) {
        Serial.println("Value out of range: " + String(value));
        index = (index + 1) % QUEUE_SIZE;
        continue;
      }
      count[value - MIN_VAL]++;
      index = (index + 1) % QUEUE_SIZE;
    }

    // Check if any 3 consecutive values together appear 56 times or more
    for (int i = 0; i < RANGE - 2; i++) {
      int total = count[i] + count[i + 1] + count[i + 2];
      if (total >= 56) {
        Serial.println("Too many of consecutive values: " + String(i + MIN_VAL) + ", " + String(i + MIN_VAL + 1) + ", " + String(i + MIN_VAL + 2));
        return false;
      }
    }

    return true;
  }
};

// Ultrasonic
#define TRIG1 32
#define ECHO1 34
#define TRIG2 33
#define ECHO2 26

// Detection range in cm
const int rangeMin = 5;
const int rangeMax = 45;

// Detection time thresholds in ms
const unsigned long confirmTime = 30000UL;
const unsigned long tempLeaveMax = 300000UL;

bool preDetect = false;

// Detection states
enum HumanState {
  NO_CUSTOMER = 0,
  CUSTOMER_PRESENT_NEW = 1,
  CUSTOMER_TEMP_LEFT = 2,
  CUSTOMER_PERM_LEFT = 3
};

Queue dis1_30;
Queue dis2_30;

int head = 0;

void enqueu(int dis) {
}

// Internal FSM states
enum InternalState {
  IDLE,
  COUNTING_IN,
  PRESENT,
  OUT_WAIT
};

InternalState currentState = IDLE;
unsigned long inRangeTimer = 0;
unsigned long outRangeTimer = 0;

// Function declarations
void menuPage();
void artPage();
void ledPage();
void updateLEDPatterns();
void waitForGRBLStartup();
void checkGRBLResponse();
void sendToGRBL(const char *command);
void sendGcodeFile(int patternNumber);
void pauseGcode();
void resumeGcode();
void stopGcode();
void setupHardwareReset();
void hardwareResetGRBL();
bool testGRBLResponse();
void executeLEDPattern(int patternNumber);
void processTouch();
void drawButton(Button &btn, bool pressed = false);
void clearButtons();
void addButton(int x, int y, int w, int h, uint16_t color, uint16_t textColor, const char *label, int id);
ButtonID getTouchedButton(int x, int y);
HumanState detectHuman();
long readUltrasonic(int trigPin, int echoPin);
float distanceCM(long duration);
bool isInRange(float d);

// *** NEW: Homing function declaration ***
bool performHoming();

void initWebSocketSync();
void checkWebSocketTimeout();

// LED Pattern functions
void setAllWhite();
void rainbowCycle(int wait);
void fireEffect();
void fireEmber();
void knightRider();
void slowRainbow();
void blueGradientWave();
void sparkleEffect();
void cometEffect();
void policeLights();
void deepDarkRedPurpleBlend();
void smoothColorSweep(uint8_t wait);
void theaterChase(uint32_t color, int wait);
void waveFade(uint32_t color1, uint32_t color2);
void rainbowTheaterChase();

void setup() {
  Serial.begin(115200);
  delay(1000);

  setupHardwareReset();

  // Initialize GRBL serial communication
  GRBL_SERIAL.begin(GRBL_BAUDRATE, SERIAL_8N1, GRBL_RX, GRBL_TX);
  Serial.println("GRBL Serial initialized.");

  // Initialize GRBL
  waitForGRBLStartup();

  sendToGRBL("$X\n");
  delay(500);
  sendToGRBL("$H\n");
  delay(1000);
  sendToGRBL("F5000\n");  // Set Feed Rate
  delay(1000);

  // *** NEW: Initialize stop button tracking ***
  stopButtonPressed = false;
  needsHoming = false;
  isCurrentlyHoming = false;
  Serial.println("Stop button tracking initialized");

  Serial.println("Starting TFT initialization...");

  // Initialize TFT
  tft.begin();
  tft.setRotation(3);

  // Set font
  tft.setFreeFont(FF18);

  Serial.println("TFT initialized successfully");

  // Initialize Touch
  ts.begin();
  ts.setRotation(3);
  Serial.println("Touch initialized");

  // Load menu
  menuPage();

  //LED
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.show();

  // Initialize SD card
  hspi.begin(14, 12, 13, SD_CS);
  if (!SD.begin(SD_CS, hspi)) {
    Serial.println("SD Card initialization failed!");
  } else {
    Serial.println("SD Card initialized.");

    // Create gcode directory if it doesn't exist
    if (!SD.exists("/gcode")) {
      if (SD.mkdir("/gcode")) {
        Serial.println("Created /gcode directory.");
      } else {
        Serial.println("Failed to create /gcode directory.");
      }
    }
  }

  // Initialize WiFi
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Failed to configure Static IP!");
  }

  WiFi.begin(ssid, password);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    Serial.print("ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed, continuing without WiFi");
  }
  if (WiFi.status() == WL_CONNECTED) {
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    Serial.println("WebSocket server started on port 81");
  }

  initWebSocketSync();

  // Initialize ultrasonic pins
  pinMode(TRIG1, OUTPUT);
  pinMode(ECHO1, INPUT);
  pinMode(TRIG2, OUTPUT);
  pinMode(ECHO2, INPUT);
  Serial.println("Ultrasonic sensors initialized.");

  const esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 30000,                              // 30 seconds
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // All cores
    .trigger_panic = true
  };

  esp_task_wdt_add(NULL);
  esp_task_wdt_init(&twdt_config);

  Serial.println("Setup complete!");
}

uint32_t lastLEDTime = 0;
uint32_t lastUltraTime = 0;

void loop() {
  static uint32_t lastSensorTime = 0;
  static uint32_t lastYieldTime = 0;
  static uint32_t lastPrintTime = 0;
  static uint32_t lastWatchdogReset = 0;

  // *** NEW: Reset watchdog timer more frequently ***
  uint32_t currentTime = millis();
  if (currentTime - lastWatchdogReset >= 1000) {  // Every 1 second
    esp_task_wdt_reset();
    lastWatchdogReset = currentTime;
  }

  processTouch();

  if (millis() - lastLEDTime >= patternUpdateInterval && currentLEDPattern != 0) {
    lastLEDTime = millis();
    updateLEDPatterns();
  }

  // *** IMPROVED: More frequent yielding ***
  if (millis() - lastYieldTime >= 50) {  // Reduced from 100ms to 50ms
    lastYieldTime = millis();
    yield();
  }

  webSocket.loop();
  checkGRBLResponse();

  // *** NEW: Check WebSocket command timeout ***
  checkWebSocketTimeout();

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') {
      // Debug touch coordinates
      Serial.println("Touch debug mode");
    }
  }
  HumanState currentDetection;
  if (millis() - lastUltraTime >= 500) {
    lastUltraTime = millis();
    currentDetection = detectHuman();
  }

  if ((currentDetection == CUSTOMER_PRESENT_NEW) && (preDetect == false) && !hasDrawn) {
    preDetect = true;
    Serial.println("G-code Running Starting");
    executeLEDPattern(8);
    esp_task_wdt_reset();
    delay(3000);
    esp_task_wdt_reset();
    executeLEDPattern(1);
    sendGcodeFile(100);
    Serial.println("G-code Running");
  } else if (currentDetection == CUSTOMER_PERM_LEFT) {
    preDetect = false;
  }
}

bool testGRBLResponse() {
  Serial.println("Testing GRBL response...");

  // Clear any existing data
  while (GRBL_SERIAL.available()) {
    GRBL_SERIAL.read();
  }

  // Send status query
  sendToGRBL("?\n");
  delay(1000);

  // Check for response
  String response = "";
  int timeout = 0;
  while (timeout < 20) {  // 2 second timeout
    while (GRBL_SERIAL.available()) {
      char c = GRBL_SERIAL.read();
      response += c;
      if (c == '\n') {
        Serial.print("GRBL responded: ");
        Serial.println(response);
        return true;
      }
    }
    delay(100);
    timeout++;
  }

  Serial.println("No response from GRBL");
  return false;
}

void hardwareResetGRBL() {
  Serial.println("Performing hardware reset of Arduino/GRBL...");

  // Pull reset line LOW (resets Arduino)
  digitalWrite(GRBL_RESET_PIN, LOW);
  delay(200);  // Hold reset for 200ms

  // Release reset line (goes high-Z, pulled up by 10kΩ)
  digitalWrite(GRBL_RESET_PIN, HIGH);
  delay(3000);  // Wait for Arduino to fully boot and GRBL to initialize

  Serial.println("Hardware reset completed");
}

void setupHardwareReset() {
  // Configure GPIO22 as open-drain output
  pinMode(GRBL_RESET_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(GRBL_RESET_PIN, HIGH);  // Start in high-Z state (Arduino runs)
  Serial.println("Hardware reset pin configured on GPIO22");
}

void initWebSocketSync() {
  pendingWSCommand.isActive = false;
  wsWaitingForGRBLOk = false;
  Serial.println("WebSocket synchronous communication initialized");
}

void waitForGRBLStartup() {
  Serial.println("=== Enhanced GRBL Startup ===");

  // Clear any existing data in the serial buffer
  while (GRBL_SERIAL.available()) {
    GRBL_SERIAL.read();
  }

  // Try software initialization first
  Serial.println("Attempting software GRBL initialization...");

  // Send a soft reset to ensure clean state
  GRBL_SERIAL.write(0x18);  // Ctrl+X soft reset
  delay(2000);

  // Clear buffer again after reset
  while (GRBL_SERIAL.available()) {
    GRBL_SERIAL.read();
  }

  // Test if GRBL responds to software reset
  if (testGRBLResponse()) {
    Serial.println("✓ GRBL responding to software reset");
    return;
  }

  // Software reset failed, try hardware reset
  Serial.println("✗ Software reset failed, trying hardware reset...");
  hardwareResetGRBL();

  // Test again after hardware reset
  if (testGRBLResponse()) {
    Serial.println("✓ GRBL responding after hardware reset");
  } else {
    Serial.println("✗ WARNING: GRBL still not responding after hardware reset!");
    Serial.println("Check wiring and connections");
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("Client #%u connected\n", num);
      break;

    case WStype_DISCONNECTED:
      Serial.printf("Client #%u disconnected\n", num);
      // Clear any pending command for this client
      if (pendingWSCommand.isActive && pendingWSCommand.clientNum == num) {
        pendingWSCommand.isActive = false;
        wsWaitingForGRBLOk = false;
        Serial.println("Cleared pending command for disconnected client");
      }
      break;

    case WStype_TEXT:
      {
        String gcodeLine = String((char *)payload);
        gcodeLine.trim();

        // ✅ Ignore keep-alive ping messages
        if (gcodeLine == ";ping") {
          return;  // Don't forward to GRBL or send "ok"
        }

        Serial.print("G-code WS received: ");
        Serial.println(gcodeLine);

        // *** CHECK: If we're already waiting for GRBL response, reject new command ***
        if (wsWaitingForGRBLOk || pendingWSCommand.isActive) {
          Serial.println("WARNING: WebSocket command rejected - still waiting for previous GRBL response");
          String busyResponse = "busy";
          webSocket.sendTXT(num, busyResponse);
          return;
        }

        // *** NEW: Auto-perform homing if needed before executing web G-code ***
        if (needsHoming && !isCurrentlyHoming && gcodeLine.length() > 0 && gcodeLine[0] != ';' && gcodeLine[0] != '(' && gcodeLine != "?") {

          Serial.println("Homing required before web G-code execution - performing auto-homing");

          // Send homing status to web client
          String homingResponse = "homing_started";
          webSocket.sendTXT(num, homingResponse);

          // Perform homing
          if (performHoming()) {
            // Reset the flags after successful homing
            needsHoming = false;
            stopButtonPressed = false;

            Serial.println("Auto-homing completed successfully, proceeding with G-code command");

            // Send homing completion notification
            String homingCompleteResponse = "homing_complete";
            webSocket.sendTXT(num, homingCompleteResponse);

            // Small delay to ensure homing is fully complete
            delay(1000);

            // Now proceed with the original G-code command
            // Fall through to the normal command processing below

          } else {
            Serial.println("Auto-homing failed, rejecting G-code command");

            String homingFailedResponse = "homing_failed";
            webSocket.sendTXT(num, homingFailedResponse);
            return;  // Don't execute the G-code command
          }
        }

        // *** PROCESS: Send command to GRBL and wait for response ***
        if (gcodeLine.length() > 0) {
          // Store the pending command details
          pendingWSCommand.command = gcodeLine;
          pendingWSCommand.clientNum = num;
          pendingWSCommand.timestamp = millis();
          pendingWSCommand.isActive = true;

          // Set waiting flag
          wsWaitingForGRBLOk = true;
          wsCommandStartTime = millis();

          // Send to GRBL
          Serial.print("Sending to GRBL: ");
          Serial.println(gcodeLine);
          sendToGRBL(gcodeLine.c_str());
          sendToGRBL("\n");

          // *** IMPORTANT: DO NOT send "ok" here - wait for GRBL response ***
          Serial.println("Command sent to GRBL, waiting for response...");

        } else {
          // Empty command - send immediate ok
          String response = "ok";
          webSocket.sendTXT(num, response);
        }
      }
      break;

    default:
      break;
  }
}

void checkWebSocketTimeout() {
  if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
    if (millis() - wsCommandStartTime > WS_COMMAND_TIMEOUT) {
      Serial.println("ERROR: WebSocket command timeout - GRBL did not respond");

      // Send timeout error to web client
      String timeoutResponse = "timeout";
      webSocket.sendTXT(pendingWSCommand.clientNum, timeoutResponse);

      // Reset state
      pendingWSCommand.isActive = false;
      wsWaitingForGRBLOk = false;

      Serial.println("WebSocket command timeout handled");
    }
  }

  // Reset watchdog during timeout checking
  static unsigned long lastWatchdogReset = 0;
  if (millis() - lastWatchdogReset >= 1000) {
    esp_task_wdt_reset();
    lastWatchdogReset = millis();
  }
}

void processTouch() {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();

    // Map touch coordinates to screen coordinates
    int x = map(p.x, 200, 3700, tft.width(), 0);
    int y = map(p.y, 200, 3700, tft.height(), 0);

    // Boundary check
    if (x < 0) x = 0;
    if (x >= tft.width()) x = tft.width() - 1;
    if (y < 0) y = 0;
    if (y >= tft.height()) y = tft.height() - 1;

    // Serial.printf("Touch: x=%d, y=%d\n", x, y);

    ButtonID touchedBtn = getTouchedButton(x, y);

    if (touchedBtn != BTN_NONE) {
      Serial.printf("Button %d pressed\n", touchedBtn);

      // Handle button press
      switch (touchedBtn) {
        case BTN_ART:
          if (currentPage == 1) {
            artPage();
          }
          break;
        case BTN_LED:
          if (currentPage == 1) {
            ledPage();
          }
          break;
        case BTN_LEFT:
          if (currentPage == 2) {
            artIndex--;
            if (artIndex < 1) artIndex = 10;
            artPage();
          } else if (currentPage == 3) {
            ledIndex--;
            if (ledIndex < 0) ledIndex = 17;
            ledPage();
          }
          break;
        case BTN_RIGHT:
          if (currentPage == 2) {
            artIndex++;
            if (artIndex > 10) artIndex = 1;
            artPage();
          } else if (currentPage == 3) {
            ledIndex++;
            if (ledIndex > 17) ledIndex = 0;
            ledPage();
          }
          break;
        case BTN_EXECUTE:
          if (currentPage == 2) {
            hasDrawn=true;
            executeLEDPattern(5);
            sendGcodeFile(artIndex);
          } else if (currentPage == 3) {
            executeLEDPattern(ledIndex);
          }
          break;
        case BTN_PAUSE:
          if (currentPage == 2) {
            if (isPausedGcode) {
              resumeGcode();
            } else {
              pauseGcode();
            }
          }
          break;
        case BTN_BACK:
          if (currentPage == 2 || currentPage == 3) {
            menuPage();
          }
          break;
        case BTN_STOP:
          if (currentPage == 2) {
            stopGcode();
          }
          break;
      }

      delay(200);
    }
  }
}

void menuPage() {
  Serial.println("Loading menu page...");

  currentPage = 1;
  tft.fillScreen(TFT_BLACK);

  // Set text properties
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);

  tft.setCursor(110, 70);

  tft.print("MAIN MENU");
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Draw buttons with visible borders
  clearButtons();
  // Art Button (left)
  addButton(110, 150, 100, 50, TFT_NAVY, TFT_WHITE, "ART", BTN_ART);

  // LED Button (right)
  addButton(270, 150, 100, 50, TFT_DARKGREEN, TFT_WHITE, "LED", BTN_LED);

  // Draw all buttons
  for (int i = 0; i < buttonCount; i++) {
    drawButton(buttons[i]);
  }

  Serial.println("Menu page loaded");
}

void artPage() {
  Serial.println("Loading art page...");

  currentPage = 2;
  inArtPage = true;
  inLEDPage = false;

  // Reset watchdog before heavy operations
  esp_task_wdt_reset();

  tft.fillScreen(TFT_BLACK);
  esp_task_wdt_reset();  // Critical: after fillScreen

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN);
  tft.setCursor(150, 40);
  tft.print("Art Page");
  esp_task_wdt_reset();  // After text operations

  tft.fillRect((screenWidth - imgWidth) / 2, (screenHeight - imgHeight) / 2, imgWidth, imgHeight, TFT_WHITE);
  esp_task_wdt_reset();  // After fillRect

  // Draw current art index
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(210, 150);
  tft.print("Art " + String(artIndex));
  esp_task_wdt_reset();  // After text

  //draw images - THIS IS LIKELY THE SLOWEST OPERATION
  String filename = "/image/img" + String(artIndex) + ".jpg";
  esp_task_wdt_reset();  // Before JPEG operation
  drawJpeg(filename.c_str(), (screenWidth - imgWidth) / 2, (screenHeight - imgHeight) / 2);
  esp_task_wdt_reset();  // Critical: after JPEG drawing

  // *** NEW: Show homing status if needed ***
  // if (needsHoming) {
  //   tft.setTextColor(TFT_ORANGE);
  //   tft.setTextSize(1);
  //   tft.setCursor(120, 180);
  //   tft.print("Homing required before next draw");
  //   esp_task_wdt_reset();
  // }

  clearButtons();
  esp_task_wdt_reset();  // After clearButtons

  // Left Arrow
  uint16_t x = 20;
  uint16_t y = tft.height() / 2 - BUTTONARROW_H / 2;
  addButton(x, y, BUTTONARROW_W, BUTTONARROW_H, TFT_DARKGREY, TFT_WHITE, "<", BTN_LEFT);

  // Right Arrow
  x = tft.width() - (20 + BUTTONARROW_W);
  addButton(x, y, BUTTONARROW_W, BUTTONARROW_H, TFT_DARKGREY, TFT_WHITE, ">", BTN_RIGHT);

  // Show different buttons based on G-code state
  x = (tft.width() - BUTTONEXEC_W - 100);
  y = tft.height() - 40;

  if (isRunningGcode && !isPausedGcode) {
    // Show Pause button when G-code is running
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_ORANGE, TFT_BLACK, "Pause", BTN_PAUSE);
  } else if (isRunningGcode && isPausedGcode) {
    // Show Resume button when G-code is paused
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_BLUE, TFT_WHITE, "Resume", BTN_PAUSE);
  } else {
    // Show Execute button when not running
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_GREEN, TFT_BLACK, "Execute", BTN_EXECUTE);
  }

  // Stop G-code Button - FIXED: Now shows when gcode is running
  if (isRunningGcode) {
    x = (tft.width() - BUTTONEXEC_W) / 2;
    y = tft.height() / 2 + 70;
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_RED, TFT_WHITE, "Stop", BTN_STOP);
  }

  // Back Button
  x = 100;
  y = tft.height() - 40;
  addButton(x, y, BUTTONBACK_W, BUTTONBACK_H, TFT_MAROON, TFT_WHITE, "Back", BTN_BACK);

  esp_task_wdt_reset();  // After all button setup

  // Draw all buttons - this loop could also be slow if you have many buttons
  for (int i = 0; i < buttonCount; i++) {
    drawButton(buttons[i]);
    // Reset watchdog every few buttons to be safe
    if (i % 3 == 0) {  // Every 3 buttons
      esp_task_wdt_reset();
    }
  }

  // Final reset before finishing
  esp_task_wdt_reset();
  yield();  // Also yield to other tasks

  Serial.println("Art page loaded");
}

void ledPage() {
  Serial.println("Loading LED page...");

  currentPage = 3;
  inLEDPage = true;
  inArtPage = false;

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN);
  tft.setCursor(150, 80);
  tft.print("LED Page");

  // Show description
  tft.setTextSize(1);
  tft.setCursor(150, 130);
  tft.setTextColor(TFT_YELLOW);
  switch (ledIndex) {
    case 0:
      tft.setCursor(180, 130);
      tft.print("Turn off LED");
      break;
    case 1:
      tft.setCursor(231, 130);
      tft.print("White");
      break;
    case 2:
      tft.setCursor(180, 130);
      tft.print("Rainbow Cycle");
      break;
    case 3:
      tft.setCursor(200, 130);
      tft.print("Fire Effect");
      break;
    case 4:
      tft.setCursor(180, 130);
      tft.print("Dark Red Purple");
      break;
    case 5:
      tft.setCursor(200, 130);
      tft.print("Fire Ember");
      break;
    case 6:
      tft.setCursor(180, 130);
      tft.print("Knight Rider");
      break;
    case 7:
      tft.setCursor(180, 130);
      tft.print("Slow Rainbow");
      break;
    case 8:
      tft.setCursor(150, 130);
      tft.print("Blue Gradient Wave");
      break;
    case 9:
      tft.setCursor(200, 130);
      tft.print("Theater Chase");
      break;
    case 10:
      tft.setCursor(150, 130);
      tft.print("Rainbow Theater Chase");
      break;
    case 11:
      tft.setCursor(200, 130);
      tft.print("Police Lights");
      break;
    case 12:
      tft.setCursor(200, 130);
      tft.print("Color Wipe Red");
      break;
    case 13:
      tft.setCursor(180, 130);
      tft.print("Color Wipe Green");
      break;
    case 14:
      tft.setCursor(200, 130);
      tft.print("Color Wipe Blue");
      break;
    case 15:
      tft.setCursor(200, 130);
      tft.print("Sparkle Effect");
      break;
    case 16:
      tft.setCursor(200, 130);
      tft.print("Comet Effect");
      break;
    case 17:
      tft.setCursor(150, 130);
      tft.print("Wave Fade Red-Blue");
      break;
  }

  clearButtons();

  // Left Arrow
  uint16_t x = 50;
  uint16_t y = tft.height() / 2 - BUTTONARROW_H / 2;
  addButton(x, y, BUTTONARROW_W, BUTTONARROW_H, TFT_DARKGREY, TFT_WHITE, "<", BTN_LEFT);

  // Right Arrow
  x = tft.width() - (50 + BUTTONARROW_W);
  addButton(x, y, BUTTONARROW_W, BUTTONARROW_H, TFT_DARKGREY, TFT_WHITE, ">", BTN_RIGHT);

  // Execute Button
  x = (tft.width() - BUTTONEXEC_W) / 2;
  y = tft.height() / 2 + 40;

  if ((ledIndex == 0 && !ledRunning) || (ledIndex == 0 && currentLEDPattern == 0) || (ledRunning && currentLEDPattern == ledIndex)) {
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_RED, TFT_WHITE, "Running", BTN_EXECUTE);
  } else {
    addButton(x, y, BUTTONEXEC_W, BUTTONEXEC_H, TFT_GREEN, TFT_BLACK, "Execute", BTN_EXECUTE);
  }

  // Back Button
  x = (tft.width() - BUTTONBACK_W) / 2;
  y = tft.height() - 50;
  addButton(x, y, BUTTONBACK_W, BUTTONBACK_H, TFT_MAROON, TFT_WHITE, "Back", BTN_BACK);

  // Draw all buttons
  for (int i = 0; i < buttonCount; i++) {
    drawButton(buttons[i]);
  }

  Serial.println("LED page loaded");
}

void clearButtons() {
  buttonCount = 0;
  for (int i = 0; i < 8; i++) {
    buttons[i].visible = false;
  }
}

void addButton(int x, int y, int w, int h, uint16_t color, uint16_t textColor, const char *label, int id) {
  if (buttonCount < 8) {
    buttons[buttonCount].x = x;
    buttons[buttonCount].y = y;
    buttons[buttonCount].w = w;
    buttons[buttonCount].h = h;
    buttons[buttonCount].color = color;
    buttons[buttonCount].textColor = textColor;
    strcpy(buttons[buttonCount].label, label);
    buttons[buttonCount].id = id;
    buttons[buttonCount].visible = true;
    buttonCount++;
  }
}

void drawButton(Button &btn, bool pressed) {
  if (!btn.visible) return;

  // Button colors
  uint16_t fillColor = pressed ? TFT_DARKGREY : btn.color;
  uint16_t borderColor = TFT_WHITE;
  uint16_t textColor = btn.textColor;

  // Draw the button shape
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, 5, fillColor);
  tft.drawRoundRect(btn.x, btn.y, btn.w, btn.h, 5, borderColor);

  // Text setup
  tft.setTextSize(1);
  tft.setTextColor(textColor, fillColor);  // Background erase
  int textW = tft.textWidth(btn.label);
  int textH = 8;  // Default font height for size 1 is 8px

  // Centered position
  int textX = btn.x + (btn.w - textW) / 2;
  int textY = btn.y + (btn.h - textH) / 2 + 10;  // <-- +2 gives visual centering

  tft.setCursor(textX, textY);
  tft.print(btn.label);
}

// FIXED: Button touch detection function
ButtonID getTouchedButton(int x, int y) {
  for (int i = 0; i < buttonCount; i++) {
    if (buttons[i].visible && x >= buttons[i].x && x <= buttons[i].x + buttons[i].w && y >= buttons[i].y && y <= buttons[i].y + buttons[i].h) {
      return (ButtonID)buttons[i].id;  // Return the button's ID, not the array index
    }
  }
  return BTN_NONE;
}

// New function specifically for synchronous G-code sending
void checkGRBLResponseSync() {
  while (GRBL_SERIAL.available()) {
    char c = GRBL_SERIAL.read();

    if (c == '\n') {
      grblBuffer[grblBufferIndex] = '\0';

      // Clean the buffer by removing any trailing whitespace
      int len = strlen(grblBuffer);
      while (len > 0 && (grblBuffer[len - 1] == '\r' || grblBuffer[len - 1] == ' ' || grblBuffer[len - 1] == '\t')) {
        grblBuffer[len - 1] = '\0';
        len--;
      }

      Serial.print("GRBL: ");
      Serial.println(grblBuffer);
      grblBufferIndex = 0;

      // Handle different GRBL responses
      if (strcmp(grblBuffer, "ok") == 0) {
        Serial.println("DEBUG: GRBL OK received");
        waitingForOk = false;  // *** KEY: Allow next command to be sent ***

      } else if (strstr(grblBuffer, "ALARM") != NULL) {
        Serial.print("DEBUG: GRBL ALARM during execution: ");
        Serial.println(grblBuffer);

        // Stop execution on alarm
        isRunningGcode = false;
        isPausedGcode = false;
        waitingForOk = false;

        if (currentPage == 2) {
          tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
          tft.setTextColor(TFT_RED);
          tft.setCursor(130, 200);
          tft.print("GRBL ALARM!");
          delay(2000);
          artPage();
        }

      } else if (strstr(grblBuffer, "error") != NULL) {
        Serial.print("DEBUG: GRBL ERROR during execution: ");
        Serial.println(grblBuffer);

        // Continue on minor errors, stop on serious ones
        if (strstr(grblBuffer, "error:9") != NULL ||   // G-code locked out
            strstr(grblBuffer, "error:20") != NULL ||  // Unsupported command
            strstr(grblBuffer, "error:21") != NULL) {  // Soft limit error

          Serial.println("DEBUG: Serious error, stopping execution");
          isRunningGcode = false;
          isPausedGcode = false;
          waitingForOk = false;

          if (currentPage == 2) {
            tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
            tft.setTextColor(TFT_RED);
            tft.setCursor(130, 200);
            tft.print("GRBL Error!");
            delay(2000);
            artPage();
          }
        } else {
          Serial.println("DEBUG: Minor error, continuing");
          waitingForOk = false;  // Continue to next command
        }

      } else if (len > 0 && grblBuffer[0] == '<' && grblBuffer[len - 1] == '>') {
        // Status report - just log it, don't change waitingForOk
        Serial.print("DEBUG: GRBL status: ");
        Serial.println(grblBuffer);

      } else if (len == 0) {
        // Empty line, ignore

      } else {
        // Other responses
        Serial.print("DEBUG: Other GRBL response: ");
        Serial.println(grblBuffer);

        // Handle "ok" with hidden characters
        if (strstr(grblBuffer, "ok") != NULL && len <= 4) {
          Serial.println("DEBUG: Detected 'ok' with extra characters");
          waitingForOk = false;
        }
      }
    } else if (grblBufferIndex < sizeof(grblBuffer) - 1) {
      grblBuffer[grblBufferIndex++] = c;
    }
  }
}

// Updated original checkGRBLResponse for non-synchronous operations (homing, etc.)
// Updated checkGRBLResponse function with TRUE WebSocket synchronization
void checkGRBLResponse() {
  static unsigned long lastWatchdogReset = 0;

  while (GRBL_SERIAL.available()) {
    // *** Reset watchdog during long response processing ***
    if (millis() - lastWatchdogReset >= 1000) {
      esp_task_wdt_reset();
      lastWatchdogReset = millis();
    }

    char c = GRBL_SERIAL.read();

    if (c == '\n') {
      grblBuffer[grblBufferIndex] = '\0';

      // Clean the buffer by removing any trailing whitespace
      int len = strlen(grblBuffer);
      while (len > 0 && (grblBuffer[len - 1] == '\r' || grblBuffer[len - 1] == ' ' || grblBuffer[len - 1] == '\t')) {
        grblBuffer[len - 1] = '\0';
        len--;
      }

      Serial.print("GRBL: ");
      Serial.println(grblBuffer);
      grblBufferIndex = 0;

      // *** HANDLE GRBL "OK" RESPONSE ***
      if (strcmp(grblBuffer, "ok") == 0) {
        Serial.println("DEBUG: GRBL confirmed command processed");

        // *** NEW: Send "ok" to WebSocket client ONLY when GRBL sends "ok" ***
        if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
          Serial.println("DEBUG: Sending 'ok' to WebSocket client after GRBL confirmation");

          String response = "ok";
          webSocket.sendTXT(pendingWSCommand.clientNum, response);

          // Reset WebSocket state
          pendingWSCommand.isActive = false;
          wsWaitingForGRBLOk = false;

          Serial.print("DEBUG: WebSocket command completed: ");
          Serial.println(pendingWSCommand.command);
        }

        if (isCurrentlyHoming) {
          Serial.println("DEBUG: OK received during homing, querying status...");
          esp_task_wdt_reset();  // *** Reset before delay ***
          delay(500);
          esp_task_wdt_reset();  // *** Reset after delay ***
          sendToGRBL("?\n");
        }

      } else if (strstr(grblBuffer, "ALARM") != NULL) {
        Serial.print("DEBUG: GRBL ALARM detected: ");
        Serial.println(grblBuffer);

        // *** NEW: Send alarm response to WebSocket client ***
        if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
          Serial.println("DEBUG: Sending ALARM response to WebSocket client");

          String alarmResponse = "alarm:";
          alarmResponse += grblBuffer;
          webSocket.sendTXT(pendingWSCommand.clientNum, alarmResponse);

          // Reset WebSocket state
          pendingWSCommand.isActive = false;
          wsWaitingForGRBLOk = false;

          Serial.println("DEBUG: WebSocket command failed due to GRBL alarm");
        }

        if (isCurrentlyHoming) {
          if (strstr(grblBuffer, "ALARM:3") != NULL || strstr(grblBuffer, "ALARM:8") != NULL) {
            Serial.println("DEBUG: Homing alarm detected (normal)");
          } else {
            Serial.println("DEBUG: Unexpected alarm during homing");
            isCurrentlyHoming = false;
          }
        } else if (isRunningGcode && !stopButtonPressed) {
          Serial.println("DEBUG: Unexpected alarm during execution, stopping");
          isRunningGcode = false;
          isPausedGcode = false;

          if (gcodeFileOpen) {
            currentGcodeFile.close();
            gcodeFileOpen = false;
          }

          delay(500);
          sendToGRBL("$X\n");

          if (currentPage == 2) {
            tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
            tft.setTextColor(TFT_RED);
            tft.setCursor(130, 200);
            tft.print("GRBL ALARM!");
            delay(2000);
            artPage();
          }
        } else {
          Serial.println("DEBUG: Alarm state detected (expected after stop or homing)");
        }

      } else if (strstr(grblBuffer, "error") != NULL) {
        Serial.println("GRBL Error detected!");
        Serial.print("DEBUG: Error message: ");
        Serial.println(grblBuffer);

        // *** NEW: Send error response to WebSocket client ***
        if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
          Serial.println("DEBUG: Sending ERROR response to WebSocket client");

          String errorResponse = "error:";
          errorResponse += grblBuffer;
          webSocket.sendTXT(pendingWSCommand.clientNum, errorResponse);

          // Reset WebSocket state
          pendingWSCommand.isActive = false;
          wsWaitingForGRBLOk = false;

          Serial.println("DEBUG: WebSocket command failed due to GRBL error");
        }

        if (isCurrentlyHoming) {
          Serial.println("DEBUG: Error during homing, stopping homing process");
          isCurrentlyHoming = false;
        }

      } else if (strstr(grblBuffer, "Idle") != NULL) {
        Serial.println("DEBUG: GRBL is idle");

        if (isCurrentlyHoming) {
          isCurrentlyHoming = false;
          Serial.println("DEBUG: Homing completed successfully (idle state)");
        }

      } else if (strstr(grblBuffer, "Run") != NULL) {
        Serial.println("DEBUG: GRBL is running");

      } else if (strstr(grblBuffer, "Hold") != NULL) {
        Serial.println("DEBUG: GRBL is in hold state");

      } else if (strstr(grblBuffer, "Home") != NULL) {
        Serial.println("DEBUG: GRBL is homing");

      } else if (len > 0 && grblBuffer[0] == '<' && grblBuffer[len - 1] == '>') {
        Serial.print("DEBUG: GRBL status report: ");
        Serial.println(grblBuffer);

        // *** OPTIONAL: Forward status reports to WebSocket client ***
        if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
          // Only forward if the web client wants status updates
          // You can add a flag to enable/disable this
          String statusResponse = "status:";
          statusResponse += grblBuffer;
          webSocket.sendTXT(pendingWSCommand.clientNum, statusResponse);
        }

        if (isCurrentlyHoming && strstr(grblBuffer, "Idle") != NULL) {
          isCurrentlyHoming = false;
          Serial.println("DEBUG: Homing completed successfully (status report)");
        }

      } else if (len == 0) {
        Serial.println("DEBUG: Empty GRBL response");

      } else {
        Serial.print("DEBUG: Other GRBL response: ");
        Serial.println(grblBuffer);

        // *** Handle "ok" with hidden characters ***
        if (strstr(grblBuffer, "ok") != NULL && len <= 4) {
          Serial.println("DEBUG: Detected 'ok' with extra characters, treating as command processed");

          // *** NEW: Send "ok" to WebSocket client for hidden "ok" ***
          if (wsWaitingForGRBLOk && pendingWSCommand.isActive) {
            Serial.println("DEBUG: Sending 'ok' to WebSocket client (hidden ok detected)");

            String response = "ok";
            webSocket.sendTXT(pendingWSCommand.clientNum, response);

            // Reset WebSocket state
            pendingWSCommand.isActive = false;
            wsWaitingForGRBLOk = false;

            Serial.print("DEBUG: WebSocket command completed (hidden ok): ");
            Serial.println(pendingWSCommand.command);
          }

          if (isCurrentlyHoming) {
            Serial.println("DEBUG: OK (with extra chars) received during homing, querying status...");
            esp_task_wdt_reset();  // *** Reset before delay ***
            delay(500);
            esp_task_wdt_reset();  // *** Reset after delay ***
            sendToGRBL("?\n");
          }
        }
      }
    } else if (grblBufferIndex < sizeof(grblBuffer) - 1) {
      grblBuffer[grblBufferIndex++] = c;
    }

    // *** Yield periodically during response processing ***
    yield();
  }

  // *** Final watchdog reset after processing all responses ***
  esp_task_wdt_reset();
}

void sendToGRBL(const char *command) {
  Serial.print("Sending to GRBL: ");
  Serial.print(command);
  GRBL_SERIAL.print(command);
}

// Add this function to ensure GRBL is ready before starting G-code
bool ensureGRBLReady() {
  Serial.println("Checking GRBL status...");

  // Send status query
  sendToGRBL("?\n");
  delay(500);
  checkGRBLResponse();

  // Try to unlock if in alarm state
  sendToGRBL("$X\n");
  delay(500);
  checkGRBLResponse();

  // Set basic parameters
  sendToGRBL("G21\n");  // Millimeters
  delay(200);
  sendToGRBL("G90\n");  // Absolute positioning
  delay(200);
  sendToGRBL("G94\n");  // Feed rate mode
  delay(200);

  checkGRBLResponse();

  Serial.println("GRBL should be ready for commands");
  return true;
}

bool performHoming() {
  Serial.println("Starting homing sequence...");

  // Show homing status on screen
  // if (currentPage == 2) {
  //   tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
  //   tft.setTextColor(TFT_YELLOW);
  //   tft.setCursor(150, 200);
  //   tft.print("Homing in progress...");
  // }

  isCurrentlyHoming = true;

  // *** CRITICAL: Reset watchdog before starting ***
  esp_task_wdt_reset();

  // Send unlock command first
  sendToGRBL("$X\n");
  delay(1000);
  checkGRBLResponse();

  // *** Reset watchdog after each major operation ***
  esp_task_wdt_reset();

  // Send homing command
  sendToGRBL("$H\n");
  delay(1000);  // Give GRBL time to start homing

  // *** Reset watchdog after homing command ***
  esp_task_wdt_reset();

  // Wait for homing to complete (timeout after 30 seconds)
  unsigned long homingStartTime = millis();
  const unsigned long homingTimeout = 30000;  // 30 seconds
  bool homingSuccess = false;
  unsigned long lastWatchdogReset = millis();  // *** NEW: Track watchdog resets ***

  Serial.println("DEBUG: Waiting for homing to complete...");

  while (isCurrentlyHoming && (millis() - homingStartTime) < homingTimeout) {
    // *** CRITICAL: Reset watchdog every 500ms during homing wait ***
    if (millis() - lastWatchdogReset >= 500) {
      esp_task_wdt_reset();
      lastWatchdogReset = millis();
      Serial.println("DEBUG: Watchdog reset during homing wait");
    }

    checkGRBLResponse();
    yield();  // *** Allow other tasks to run ***

    // Check if homing completed
    if (!isCurrentlyHoming) {
      homingSuccess = true;
      Serial.println("DEBUG: Homing flag cleared, checking final status...");
      break;
    }

    delay(100);  // Reduced delay for more responsive watchdog handling
  }

  // *** Reset watchdog after homing loop ***
  esp_task_wdt_reset();

  // *** FIXED: Better timeout and final status check ***
  if (isCurrentlyHoming) {
    // Homing timed out
    isCurrentlyHoming = false;
    homingSuccess = false;
    Serial.println("ERROR: Homing timed out!");
  } else if (homingSuccess) {
    // Final confirmation - query GRBL status
    Serial.println("DEBUG: Performing final homing status check...");

    // *** Reset watchdog before delays ***
    esp_task_wdt_reset();
    delay(1000);  // Give GRBL time to settle after homing

    esp_task_wdt_reset();  // *** Reset again after delay ***
    sendToGRBL("?\n");     // Query status

    esp_task_wdt_reset();
    delay(1000);

    esp_task_wdt_reset();  // *** Reset before checkGRBLResponse ***
    checkGRBLResponse();

    // Double-check that we're not still homing
    if (!isCurrentlyHoming) {
      homingSuccess = true;
      Serial.println("DEBUG: Final homing confirmation successful");
    } else {
      homingSuccess = false;
      Serial.println("DEBUG: Final homing confirmation failed");
    }
  }

  // *** Final watchdog reset ***
  esp_task_wdt_reset();

  if (!homingSuccess) {
    // if (currentPage == 2) {
    //   tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    //   tft.setTextColor(TFT_RED);
    //   tft.setCursor(150, 200);
    //   tft.print("Homing failed!");
    //   delay(2000);
    //   tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    // }
    return false;
  }

  // Set feed rate after homing
  sendToGRBL("F5000\n");

  // *** CRITICAL: Reset watchdog before delay and response check ***
  esp_task_wdt_reset();
  delay(500);
  esp_task_wdt_reset();  // *** Reset after delay ***
  checkGRBLResponse();
  esp_task_wdt_reset();  // *** Reset after response check ***

  Serial.println("Homing completed successfully");

  // if (currentPage == 2) {
  //   tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
  //   tft.setTextColor(TFT_GREEN);
  //   tft.setCursor(150, 200);
  //   tft.print("Homing completed!");
  //   delay(2000);  // Show success message for 2 seconds
  //   tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
  // }

  return true;
}

void sendGcodeFile(int patternNumber) {
  Serial.println("DEBUG: Resetting all flags for new pattern");
  isRunningGcode = false;
  isPausedGcode = false;
  waitingForOk = false;
  sprintf(currentGcodeFilename, "/gcode/pattern%d.gcode", patternNumber);

  // === Homing check ===
  if (needsHoming) {
    Serial.println("Homing required before G-code execution");

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(120, 200);
    tft.print("Homing required...");

    if (!performHoming()) {
      Serial.println("Homing failed, aborting G-code execution");
      return;
    }

    needsHoming = false;
    stopButtonPressed = false;
    delay(1000);
    artPage();
  }

  currentGcodeFile = SD.open(currentGcodeFilename);
  if (!currentGcodeFile) {
    Serial.print("Failed to open G-code file: ");
    Serial.println(currentGcodeFilename);

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setCursor(100, 200);
    tft.print("File not found!");
    delay(2000);
    return;
  }

  size_t fileSize = currentGcodeFile.size();
  Serial.print("G-code file size: ");
  Serial.print(fileSize);
  Serial.println(" bytes");

  if (fileSize == 0) {
    Serial.println("WARNING: G-code file is empty!");
    currentGcodeFile.close();

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setCursor(150, 200);
    tft.print("File is empty!");
    delay(2000);
    return;
  }

  Serial.print("Sending G-code file: ");
  Serial.println(currentGcodeFilename);

  if (!ensureGRBLReady()) {
    Serial.println("GRBL not ready, aborting");
    currentGcodeFile.close();
    return;
  }

  tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setCursor(150, 200);
  tft.print("Sending G-code...");

  isRunningGcode = true;
  isPausedGcode = false;
  gcodeFileOpen = true;
  waitingForOk = false;

  artPage();

  Serial.println("DEBUG: Starting STREAMING G-code transmission...");

  char lineBuf[128];
  int lineIndex = 0;
  int linesProcessed = 0;
  int linesSent = 0;
  unsigned long startTime = millis();
  unsigned long lastYieldTime = millis();
  unsigned long lastProgressUpdate = millis();

  // === Start streaming the file ===
  while (currentGcodeFile.available() && isRunningGcode) {
    char c = currentGcodeFile.read();

    if (c == '\n' || c == '\r') {
      if (lineIndex > 0) {
        lineBuf[lineIndex] = '\0';

        if (lineBuf[0] != '\0' && lineBuf[0] != ';' && lineBuf[0] != '(') {
          // Wait if paused
          while (isPausedGcode && isRunningGcode) {

            if (millis() - lastLEDTime >= patternUpdateInterval && currentLEDPattern != 0) {
              lastLEDTime = millis();
              processTouch();
              updateLEDPatterns();
            }
            yield();
            esp_task_wdt_reset();
            delay(50);
          }

          if (!isRunningGcode) break;

          Serial.print("Sending line ");
          Serial.print(linesSent + 1);
          Serial.print(": ");
          Serial.println(lineBuf);

          sendToGRBL(lineBuf);
          sendToGRBL("\n");

          waitingForOk = true;
          lastCommandTime = millis();
          linesSent++;

          // Wait for GRBL "ok"
          while (waitingForOk && (millis() - lastCommandTime < commandTimeout)) {
            checkGRBLResponseSync();
            yield();
            esp_task_wdt_reset();
            processTouch();

            if (millis() - lastLEDTime >= patternUpdateInterval && currentLEDPattern != 0) {
              lastLEDTime = millis();
              updateLEDPatterns();
            }
            delay(2);
          }

          if (waitingForOk) {
            Serial.println("WARNING: Command timeout, continuing");
            waitingForOk = false;
          }

          linesProcessed++;
        }

        lineIndex = 0;
      }
    } else if (lineIndex < sizeof(lineBuf) - 1) {
      lineBuf[lineIndex++] = c;
    }

    // Periodic yield to avoid watchdog timeout
    unsigned long now = millis();
    if (now - lastYieldTime >= 100) {
      yield();
      esp_task_wdt_reset();
      webSocket.loop();
      lastYieldTime = now;
    }

    // Update progress on screen every second
    if (now - lastProgressUpdate >= 2000) {
      lastProgressUpdate = now;

      int percent = (int)((currentGcodeFile.position() * 100UL) / fileSize);
      tft.fillRect(tft.width() - 80, tft.height() - 40, 80, 40, TFT_BLACK);
      tft.setTextColor(TFT_GREEN);
      tft.setCursor(tft.width() - 75, tft.height() - 10);
      tft.print(" (");
      tft.print(percent);
      tft.print("%)");
    }

    processTouch();

    if (millis() - lastLEDTime >= patternUpdateInterval && currentLEDPattern != 0) {
      lastLEDTime = millis();
      updateLEDPatterns();
    }
  }

  currentGcodeFile.close();
  gcodeFileOpen = false;

  Serial.print("DEBUG: G-code transmission finished. Lines sent: ");
  Serial.print(linesSent);
  Serial.print(", Lines completed: ");
  Serial.println(linesProcessed);
  Serial.print("DEBUG: Execution time: ");
  Serial.print(millis() - startTime);
  Serial.println(" ms");

  waitingForOk = false;

  if (isRunningGcode && !isPausedGcode && linesProcessed > 0) {
    Serial.println("DEBUG: G-code completed successfully");

    isRunningGcode = false;
    isPausedGcode = false;

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(150, 200);
    tft.print("G-code completed!");
    delay(2000);
    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);

    artPage();
  } else {
    Serial.println("DEBUG: G-code did not complete normally");
    Serial.print("DEBUG: isRunningGcode=");
    Serial.print(isRunningGcode);
    Serial.print(", isPausedGcode=");
    Serial.print(isPausedGcode);
    Serial.print(", linesProcessed=");
    Serial.println(linesProcessed);

    isRunningGcode = false;
    isPausedGcode = false;
  }
}


// FIXED: Pause function with proper UI update
void pauseGcode() {
  if (isRunningGcode && !isPausedGcode) {
    isPausedGcode = true;

    // Send pause command to GRBL
    sendToGRBL("!\n");  // Feed hold command

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_ORANGE);
    tft.setCursor(150, 200);
    tft.print("G-code paused");

    // Update the art page to show resume button
    artPage();

    Serial.println("G-code paused");
  }
}

// FIXED: Resume function with proper UI update
void resumeGcode() {
  if (isRunningGcode && isPausedGcode) {
    isPausedGcode = false;

    // Send resume command to GRBL
    sendToGRBL("~\n");  // Cycle start/resume command

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setCursor(150, 200);
    tft.print("G-code resumed");

    // Update the art page to show pause button
    artPage();

    Serial.println("G-code resumed");
  }
}

// *** MODIFIED: Stop function with stop button tracking ***
void stopGcode() {
  if (isRunningGcode) {
    Serial.println("Stopping G-code execution...");

    // *** NEW: Set stop button tracking flags ***
    stopButtonPressed = true;
    needsHoming = true;

    // Set flags to stop execution
    isRunningGcode = false;
    isPausedGcode = false;

    // Close file if open
    if (gcodeFileOpen) {
      currentGcodeFile.close();
      gcodeFileOpen = false;
    }

    // Send emergency stop commands to GRBL
    sendToGRBL("!\n");  // Feed hold (immediate pause)
    delay(200);
    sendToGRBL("\x18");  // Soft reset (Ctrl+X) - this will trigger ALARM:3
    delay(1000);         // Wait for reset to complete

    // Clear the alarm state
    sendToGRBL("$X\n");  // Unlock/clear alarm state
    delay(500);

    // Send additional reset commands to ensure clean state
    sendToGRBL("G21\n");  // Set units to millimeters
    delay(200);
    sendToGRBL("G90\n");  // Absolute positioning
    delay(200);
    sendToGRBL("G94\n");  // Feed rate mode
    delay(200);

    // Clear any remaining GRBL responses
    checkGRBLResponse();
    delay(500);
    checkGRBLResponse();

    Serial.println("GRBL reset and alarm cleared");
    Serial.println("DEBUG: Stop button pressed - homing will be required for next execution");

    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.setCursor(150, 200);
    tft.print("G-code stopped!");
    delay(2000);
    tft.fillRect(0, 180, tft.width(), 40, TFT_BLACK);

    // Update the art page to show execute button again and homing warning
    artPage();

    Serial.println("G-code execution stopped and GRBL ready");
  }
}

void executeLEDPattern(int patternNumber) {
  Serial.print("Executing LED pattern: ");
  Serial.println(patternNumber);

  if (ledRunning && patternNumber == currentLEDPattern) {
    // Turn off LEDs
    ledRunning = false;
    currentLEDPattern = 0;
    strip.clear();
    strip.show();

    // Redraw LED page with updated button state
    ledPage();
  } else {
    // Start LED pattern
    ledRunning = true;
    currentLEDPattern = patternNumber;

    // Redraw LED page with updated button state
    ledPage();

    // Run the selected pattern
    switch (patternNumber) {
      case 1: setAllWhite(); break;
      case 2: rainbowCycle(10); break;
      case 3: fireEffect(); break;
      case 4: deepDarkRedPurpleBlend(); break;
      case 5: fireEmber(); break;
      case 6: knightRider(); break;
      case 7: slowRainbow(); break;
      case 8: blueGradientWave(); break;
      case 9: theaterChase(strip.Color(127, 127, 127), 50); break;  // Theater chase white
      case 10: rainbowTheaterChase(); break;                        // Theater chase rainbow
      case 11: policeLights(); break;
      case 12: colorWipe(strip.Color(255, 0, 0), 50); break;                     // Red wipe
      case 13: colorWipe(strip.Color(0, 255, 0), 50); break;                     // Green wipe
      case 14: colorWipe(strip.Color(0, 0, 255), 50); break;                     // Blue wipe
      case 15: sparkleEffect(); break;                                           // Sparkle effect
      case 16: cometEffect(); break;                                             // Comet effect
      case 17: waveFade(strip.Color(255, 0, 0), strip.Color(0, 0, 255)); break;  // Red-Blue fade wave
      default:
        strip.clear();
        strip.show();
        break;
    }
  }
}

void updateLEDPatterns() {
  if (currentLEDPattern == 0) return;  // LEDs off

  switch (currentLEDPattern) {
    case 2: rainbowCycle(0); break;  // Fast rainbow
    case 3: fireEffect(); break;
    case 4: deepDarkRedPurpleBlend(); break;
    case 5: fireEmber(); break;
    case 6: knightRider();
    case 7: slowRainbow(); break;
    case 8: blueGradientWave(); break;
    case 9: theaterChase(strip.Color(127, 127, 127), 50); break;  // Theater chase white
    case 10: rainbowTheaterChase(); break;                        // Theater chase rainbow
    case 11: policeLights(); break;
    case 12: colorWipe(strip.Color(255, 0, 0), 50); break;                     // Red wipe
    case 13: colorWipe(strip.Color(0, 255, 0), 50); break;                     // Green wipe
    case 14: colorWipe(strip.Color(0, 0, 255), 50); break;                     // Blue wipe
    case 15: sparkleEffect(); break;                                           // Sparkle effect
    case 16: cometEffect(); break;                                             // Comet effect
    case 17: waveFade(strip.Color(255, 0, 0), strip.Color(0, 0, 255)); break;  // Red-Blue fade wave
  }
}

long readUltrasonic(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  return pulseIn(echoPin, HIGH, 30000);  // timeout 30ms
}

float distanceCM(long duration) {
  float dis = (duration / 2.0) * 0.0343;
  // Serial.print("Distance = ");
  // Serial.println(dis, 2);
  return dis;
}

bool isInRange(float d) {
  return (d >= rangeMin && d <= rangeMax);
}

HumanState detectHuman() {
  // Read both ultrasonic sensors
  long duration1 = readUltrasonic(TRIG1, ECHO1);
  long duration2 = readUltrasonic(TRIG2, ECHO2);

  float distance1 = distanceCM(duration1);
  Serial.print("Distance 1: ");
  Serial.println(distance1);
  float distance2 = distanceCM(duration2);
  Serial.print("Distance 2: ");
  Serial.println(distance2);

  // Check if any sensor detects someone in range
  bool detected1 = isInRange(distance1);

  bool detected2 = isInRange(distance2);

  bool detected = detected1 || detected2;

  unsigned long currentTime = millis();

  switch (currentState) {
    case IDLE:
      if (detected) {
        currentState = COUNTING_IN;
        inRangeTimer = currentTime;
        dis1_30.clear();
        dis2_30.clear();
      }
      return NO_CUSTOMER;

    case COUNTING_IN:
      if (detected) {
        if (detected1) {
          dis1_30.enqueue(distance1);
        }
        if (detected2) {
          dis2_30.enqueue(distance2);
        }
        if (currentTime - inRangeTimer >= confirmTime) {
          if (dis1_30.hasRequiredItems()) {
            if (dis1_30.isValid()) {
              currentState = PRESENT;
              return CUSTOMER_PRESENT_NEW;
            }
          }
          if (dis2_30.hasRequiredItems()) {
            if (dis2_30.isValid()) {
              currentState = PRESENT;
              return CUSTOMER_PRESENT_NEW;
            }
          }
        }
      } else {
        currentState = IDLE;
      }
      return NO_CUSTOMER;

    case PRESENT:
      if (!detected) {
        currentState = OUT_WAIT;
        outRangeTimer = currentTime;
        return CUSTOMER_TEMP_LEFT;
      }
      return NO_CUSTOMER;

    case OUT_WAIT:
      if (detected) {
        currentState = PRESENT;
      } else if (currentTime - outRangeTimer >= tempLeaveMax) {
        currentState = IDLE;
        executeLEDPattern(0);
        return CUSTOMER_PERM_LEFT;
      }
      return CUSTOMER_TEMP_LEFT;
  }

  return NO_CUSTOMER;
}

// LED Pattern Functions

void solidColor(uint32_t color) {
  for (uint16_t i = 0; i < pixelCount; i++) strip.setPixelColor(i, color);
  strip.show();
}

void rainbowTheaterChase() {
  static int j = 0, q = 0;
  for (int i = 0; i < pixelCount; i++) {
    if ((i + q) % 3 == 0) strip.setPixelColor(i, Wheel((i + j) % 255));
    else strip.setPixelColor(i, 0);
  }
  strip.show();
  q = (q + 1) % 3;
  if (q == 0) j++;
  delay(50);
}

void deepDarkRedPurpleBlend() {
  static uint8_t step = 0;
  uint8_t r = 20 + (sin(step * 0.1) + 1) * 15;  // 20–50 red
  uint8_t b = 10 + (cos(step * 0.1) + 1) * 10;  // 10–30 blue
  for (uint16_t i = 0; i < pixelCount; i++) strip.setPixelColor(i, r, 0, b);
  strip.show();
  step++;
  delay(40);
}

void policeLights() {
  static bool state = false;
  for (int i = 0; i < pixelCount / 2; i++)
    strip.setPixelColor(i, state ? strip.Color(255, 0, 0) : 0);
  for (int i = pixelCount / 2; i < pixelCount; i++)
    strip.setPixelColor(i, state ? 0 : strip.Color(0, 0, 255));
  strip.show();
  state = !state;
  delay(100);
}

void waveFade(uint32_t color1, uint32_t color2) {
  static uint8_t offset = 0;
  for (int i = 0; i < pixelCount; i++) {
    float ratio = (sin((i + offset) * 0.3) + 1) / 2;
    uint8_t r = ((color1 >> 16) & 0xFF) * ratio + ((color2 >> 16) & 0xFF) * (1 - ratio);
    uint8_t g = ((color1 >> 8) & 0xFF) * ratio + ((color2 >> 8) & 0xFF) * (1 - ratio);
    uint8_t b = (color1 & 0xFF) * ratio + (color2 & 0xFF) * (1 - ratio);
    strip.setPixelColor(i, r, g, b);
  }
  strip.show();
  offset++;
  delay(30);
}

void cometEffect() {
  static int head = 0;
  const int tailDecay = 5;  // Smaller = longer tail (original was 50)
  const int maxFade = 255;
  const int tailLength = maxFade / tailDecay;  // How many LEDs in the tail

  for (int i = 0; i < pixelCount; i++) {
    int distance = abs(i - head);

    // Wrap tail around ends of the strip
    if (distance > pixelCount / 2) {
      distance = pixelCount - distance;
    }

    int fade = max(0, maxFade - (distance * tailDecay));
    strip.setPixelColor(i, strip.Color(fade, 0, fade));  // Purple comet
  }

  strip.show();
  head = (head + 1) % pixelCount;
  delay(30);
}


void sparkleEffect() {
  strip.setPixelColor(random(pixelCount), strip.Color(255, 255, 255));
  strip.show();
  delay(10);
}

void theaterChase(uint32_t color, int wait) {
  static int q = 0;
  for (int i = 0; i < pixelCount; i++) {
    if ((i + q) % 3 == 0) strip.setPixelColor(i, color);
    else strip.setPixelColor(i, 0);
  }
  strip.show();
  q = (q + 1) % 3;
  delay(wait);
}

void knightRider() {
  for (uint16_t i = 0; i < pixelCount; i++) {
    strip.setPixelColor(i, 0);  // Clear
  }
  strip.setPixelColor(knightIndex, strip.Color(255, 0, 0));
  strip.show();

  knightIndex += knightDir;
  if (knightIndex == pixelCount - 1 || knightIndex == 0) knightDir = -knightDir;
  delay(30);
}

void fireEmber() {
  for (uint16_t i = 0; i < pixelCount; i++) {
    byte r = random(100, 255);
    byte g = random(30, 80);
    byte b = random(0, 30);
    strip.setPixelColor(i, r, g, b);
  }
  strip.show();
  delay(50);
}

void setAllWhite() {
  for (int i = 0; i < strip.numPixels(); i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 255));
  }
  strip.show();
}

void slowRainbow() {
  static uint16_t j = 0;
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV((i * 65536 / NUM_LEDS) + j)));
  }
  strip.show();
  j += 5;  // adjust for speed
}

void brightComet(uint32_t color, uint8_t tailLength, uint8_t fadeAmount, uint8_t wait) {
  static int head = 0;

  // Fade all LEDs
  for (int i = 0; i < strip.numPixels(); i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    r = (r > fadeAmount) ? r - fadeAmount : 0;
    g = (g > fadeAmount) ? g - fadeAmount : 0;
    b = (b > fadeAmount) ? b - fadeAmount : 0;