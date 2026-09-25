#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

// --- Configuration ---
const char* ssid = "SSID";
const char* password = "PASSWORD";
const char* apiUrl = "apiURL";

// --- Hardware Pins ---
#define LED_PIN 15 // GP15 (Physical Pin 20) for WS2812B Data

// --- OLED Setup ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- LED Matrix Setup (2 Units: 64x8) ---
// Using COLUMNS + ZIGZAG and NEO_RGB based on your hardware tests
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(64, 8, LED_PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG, 
  NEO_RGB            + NEO_KHZ800);       

// --- Global Variables ---
String mainText = "Loading...";
unsigned long launchTimestamp = 0;
long timeUntilLaunch = 0;
bool isCountingDown = false;
bool postLaunchFetchTriggered = false;
unsigned long lastTickTime = 0;
unsigned long zeroHitTime = 0;
int cursorX = 64; 

// Array to hold our rolling console messages (reduced to 5 lines for the timer)
String consoleLines[5] = {"", "", "", "", ""};

// --- Function Prototypes ---
void printConsole(String msg);
void updateOLED();
void connectWiFi();
void fetchLaunchData();
String formatCountdown(long seconds);
void updateDisplay();

void setup() {
  // Give the hardware 2 seconds to stabilize power before hitting the WiFi radio
  delay(2000); 
  Serial.begin(115200);
  
  // Initialize OLED (Default I2C pins for Pico: SDA=GP4, SCL=GP5)
  Wire.setSDA(4);
  Wire.setSCL(5);
  Wire.begin();
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printConsole("System Booting...");
  
  // Initialize LED Matrix
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(40); // Safe brightness for USB testing
  
  connectWiFi();
  fetchLaunchData();
}

void loop() {
  unsigned long currentMillis = millis();

// 1. Handle Internal Countdown (Tick every 1 second)
  if (isCountingDown && (currentMillis - lastTickTime >= 1000)) {
    lastTickTime = currentMillis;
    
    // Get the current real-world Unix time
    time_t now = time(nullptr);
    
    // Calculate the difference
    if (launchTimestamp > now) {
      timeUntilLaunch = launchTimestamp - now;
    } else {
      timeUntilLaunch = 0;
    }

    // Trigger post-launch fetch if we hit zero
    if (timeUntilLaunch == 0 && !postLaunchFetchTriggered) {
      zeroHitTime = currentMillis;
      postLaunchFetchTriggered = true;
      printConsole("LIFTOFF! Awaiting API.");
    }
    
    // Refresh the OLED screen every second to update the visual countdown
    updateOLED(); 
  }

  // 2. Fetch next launch 60 seconds after zero
  if (postLaunchFetchTriggered && (currentMillis - zeroHitTime >= 60000)) {
    printConsole("Fetching next launch...");
    fetchLaunchData();
    postLaunchFetchTriggered = false; 
  }

  // 3. Render LED Matrix and handle scrolling
  updateDisplay();
  
  // Controls scrolling speed (lower = faster)
  delay(40); 
}

// --- Helper Functions ---

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // 1. Draw the top 5 console lines (Standard size 1 text)
  display.setTextSize(1);
  for(int i = 0; i < 5; i++) {
    display.setCursor(0, i * 8); // Spaces lines 8 pixels apart vertically
    display.println(consoleLines[i]);
  }
  
  // 2. Draw a sleek separator line across the screen
  display.drawLine(0, 44, 128, 44, SSD1306_WHITE);
  
  // 3. Draw the live countdown at the bottom
  display.setCursor(4, 49); 
  display.setTextSize(2); // Double size for readability
  display.print(formatCountdown(timeUntilLaunch));
  
  // Reset text size for the next time it loops
  display.setTextSize(1); 
  
  display.display();
}

void printConsole(String msg) {
  Serial.println(msg); // Keep serial backup
  
  // Shift 5 lines up by one
  for(int i = 0; i < 4; i++) {
    consoleLines[i] = consoleLines[i+1];
  }
  // Add new message to the bottom slot
  consoleLines[4] = msg;
  
  // Trigger the screen update
  updateOLED();
}

void connectWiFi() {
  printConsole("Starting WiFi...");

  // Clear any stalled radio states and set to Client mode
  WiFi.disconnect(true);
  delay(1000); 
  WiFi.mode(WIFI_STA);

  WiFi.begin(ssid, password);
  
  // Show WiFi loading on the main LEDs
  matrix.fillScreen(0);
  matrix.setCursor(0, 0);
  matrix.setTextColor(matrix.Color(0, 0, 255));
  matrix.print("WiFi...");
  matrix.show();

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1500);
    attempts++;
    
    if(attempts % 4 == 0) {
      printConsole("Still connecting...");
    }

    // If it hangs for 10 seconds, reboot the WiFi radio
    if (attempts > 20) {
      printConsole("Radio stuck. Retrying...");
      WiFi.disconnect(true);
      delay(1000);
      WiFi.begin(ssid, password);
      attempts = 0; // Reset counter
    }
  }
  
  printConsole("WiFi Connected!");
  printConsole("IP: " + WiFi.localIP().toString());
  printConsole("Syncing NTP Time...");
  
  // Set time via NTP. (0 timezone offset, 0 daylight savings because Unix epoch is always UTC)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  // Wait for the time to actually sync
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    delay(500);
    now = time(nullptr);
  }
  printConsole("Time Synced!");
}

void fetchLaunchData() {
  if (WiFi.status() == WL_CONNECTED) {
    printConsole("Connecting to API...");
    
    WiFiClientSecure client;
    client.setInsecure(); // Safe for public APIs
    
    HTTPClient http;
    http.begin(client, apiUrl);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      printConsole("HTTP " + String(httpResponseCode) + " OK");
      String payload = http.getString();
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        String launchName = doc["name"].as<String>();
        String launchLoc = doc["location"].as<String>();
        String launchDateStr = doc["date"].as<String>();
        
        launchDateStr.replace("T", " ");
        launchDateStr.replace(":00Z", " UTC"); 

        // Construct the blue text portion
        mainText = launchName + " | " + launchLoc + " | " + launchDateStr + " | ";
        
        // Grab the absolute Unix timestamp instead of the cached seconds
        launchTimestamp = doc["timestamp"].as<unsigned long>();
        isCountingDown = true;
        
        printConsole("Parsed: " + launchName.substring(0, 10) + "...");
        printConsole("Sync complete.");
        } else {
        printConsole("JSON Error!");
      }
    } else {
      printConsole("HTTP Error: " + String(httpResponseCode));
    }
    http.end();
  } else {
    printConsole("Error: No WiFi!");
  }
}

String formatCountdown(long seconds) {
  if (seconds <= 0) return "T-00:00:00"; 
  int h = seconds / 3600;
  int m = (seconds % 3600) / 60;
  int s = seconds % 60;
  
  char buf[20];
  sprintf(buf, "T-%02d:%02d:%02d", h, m, s);
  return String(buf);
}

void updateDisplay() {
  matrix.fillScreen(0); 
  matrix.setCursor(cursorX, 0);

  // 1. Print main text in Blue
  matrix.setTextColor(matrix.Color(0, 0, 255)); 
  matrix.print(mainText);

  // 2. Print countdown in Green (or Red if 0)
  if (timeUntilLaunch > 0) {
    matrix.setTextColor(matrix.Color(0, 255, 0)); 
  } else {
    matrix.setTextColor(matrix.Color(255, 0, 0)); 
  }

  String countdownString = formatCountdown(timeUntilLaunch);
  matrix.print(countdownString);

  matrix.show();

  // 3. Handle scrolling math
  cursorX--;
  int totalTextWidth = (mainText.length() + countdownString.length()) * 6;
  
  if (cursorX < -totalTextWidth) {
    cursorX = 64; // Reset to the far right side of the 64-pixel display
  }
}
