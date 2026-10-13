#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <time.h> // Required for NTP syncing

// --- Configuration ---
const char* ssid = "SSID";
const char* password = "PASSWORD";
const char* apiUrl = "API_URL";

// --- Custom Sprites ---
const unsigned char rocket_bmp[] PROGMEM = {
  0x00, 0x1C, // ........ ...***..  (Top Fin)
  0x00, 0x3E, // ........ ..*****.  (Top Fin)
  0x1F, 0xFF, // ...***** ********  (Body)
  0x7F, 0xFF, // .******* ********  (Nose + Body)
  0xFF, 0xFF, // ******** ********  (Nose + Body)
  0x7F, 0xFF, // .******* ********  (Nose + Body)
  0x1F, 0xFF, // ...***** ********  (Body)
  0x00, 0x3E  // ........ ..*****.  (Bottom Fin)
};

// --- Hardware Pins ---
#define LED_PIN 15 // GP15 (Physical Pin 20)

// --- OLED Setup ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- LED Matrix Setup (4 Units: 128x8) ---
const int MATRIX_WIDTH = 128;
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(MATRIX_WIDTH, 8, LED_PIN,
  NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG, 
  NEO_GRB            + NEO_KHZ800);       

// --- Global Variables ---
String mainText = "Loading...";
unsigned long launchTimestamp = 0; 
long timeUntilLaunch = 0;
bool isCountingDown = false;
bool postLaunchFetchTriggered = false;
unsigned long lastTickTime = 0;
unsigned long zeroHitTime = 0;
int cursorX = MATRIX_WIDTH; 

String consoleLines[5] = {"", "", "", "", ""};

// --- OLED Scrolling Variables ---
String globalLaunchName = "";
bool displayScrollingName = false;
bool needsOledScroll = false;
int oledCursorX = 0;
unsigned long lastOledUpdateTime = 0;
unsigned long dataSyncedTime = 0;
bool pendingNameScroll = false;

// --- Function Prototypes ---
void printConsole(String msg);
void updateOLED();
void connectWiFi();
void fetchLaunchData();
String formatCountdown(long seconds);
void updateDisplay();

void setup() {
  delay(2000); 
  Serial.begin(115200);
  
  Wire.setSDA(4);
  Wire.setSCL(5);
  Wire.begin();
  
  // Speed up I2C to 400kHz so the OLED animation doesn't lag the LED matrix
  Wire.setClock(400000); 
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); 
  }
  
  display.setTextWrap(false);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printConsole("System Booting...");
  
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(40); // 0-255
  
  connectWiFi();
  fetchLaunchData();
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Check if it's time to swap "Data Synced" to the scrolling name
  if (pendingNameScroll && (currentMillis - dataSyncedTime >= 2000)) {
    pendingNameScroll = false;
    displayScrollingName = true;
    // Check if the name is wider than the 128px screen (6 pixels per char)
    needsOledScroll = (globalLaunchName.length() * 6) > 128;
    oledCursorX = needsOledScroll ? 128 : 0; // Start at right edge if scrolling, else lock to left
    updateOLED(); // Force an immediate draw
  }

  // 2. Handle OLED Name Scrolling (Updates every 50ms if scrolling is needed)
  if (displayScrollingName && needsOledScroll && (currentMillis - lastOledUpdateTime >= 50)) {
    lastOledUpdateTime = currentMillis;
    oledCursorX--;
    int nameWidth = globalLaunchName.length() * 6;
    if (oledCursorX < -nameWidth) {
      oledCursorX = 128; 
    }
    updateOLED();
  }

  // 3. Handle Internal Countdown via Real-World Time (1 Hz Tick)
  if (isCountingDown && (currentMillis - lastTickTime >= 1000)) {
    lastTickTime = currentMillis;
    
    time_t now = time(nullptr);
    
    if (launchTimestamp > now) {
      timeUntilLaunch = launchTimestamp - now;
    } else {
      timeUntilLaunch = 0;
    }

    if (timeUntilLaunch == 0 && !postLaunchFetchTriggered) {
      zeroHitTime = currentMillis;
      postLaunchFetchTriggered = true;
      printConsole("LIFTOFF! Awaiting API.");
    }
    
    // Only trigger the 1-second OLED refresh here if it isn't ALREADY 
    // being refreshed every 50ms by the scrolling animation above.
    if (!(displayScrollingName && needsOledScroll)) {
       updateOLED(); 
    }
  }

  // 4. Fetch next launch 60 seconds after zero
  if (postLaunchFetchTriggered && (currentMillis - zeroHitTime >= 60000)) {
    printConsole("Fetching next launch...");
    fetchLaunchData();
    postLaunchFetchTriggered = false; 
  }

  // 5. Render Main LED Matrix
  updateDisplay();
  
  delay(40); 
}

// --- Helper Functions ---

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  display.setTextSize(1);
  
  // 1. Draw the top 4 console lines (0 through 3)
  for(int i = 0; i < 4; i++) {
    display.setCursor(0, i * 8); 
    display.println(consoleLines[i]);
  }
  
  // 2. Draw either the 5th console line OR the scrolling JSON name
  if (displayScrollingName) {
    display.setCursor(oledCursorX, 32); 
    display.print(globalLaunchName);
  } else {
    display.setCursor(0, 32);
    display.println(consoleLines[4]);
  }
  
  // 3. Draw separator and Countdown Timer
  // display.drawLine(0, 44, 128, 44, SSD1306_WHITE);
  
  display.setCursor(4, 49); 
  display.setTextSize(2); 
  display.print(formatCountdown(timeUntilLaunch));
  
  display.setTextSize(1); 
  display.display();
}

void printConsole(String msg) {
  Serial.println(msg); 
  
  // If a new console message arrives, turn off the scrolling name
  // so the user can see what the console is trying to tell them.
  displayScrollingName = false;
  pendingNameScroll = false;
  
  for(int i = 0; i < 4; i++) {
    consoleLines[i] = consoleLines[i+1];
  }
  consoleLines[4] = msg;
  
  updateOLED();
}

void connectWiFi() {
  printConsole("Starting WiFi...");

  WiFi.disconnect(true);
  delay(1000); 
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  matrix.fillScreen(0);
  matrix.setCursor(0, 0);
  matrix.setTextColor(matrix.Color(0, 0, 255));
  matrix.print("WiFi...");
  matrix.show();

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    attempts++;
    if(attempts % 4 == 0) printConsole("Still connecting...");
    
    if (attempts > 20) {
      printConsole("Radio stuck. Retrying...");
      WiFi.disconnect(true);
      delay(1000);
      WiFi.begin(ssid, password);
      attempts = 0; 
    }
  }
  
  printConsole("WiFi Connected!");
  printConsole("IP: " + WiFi.localIP().toString());
  
  // -- NTP Time Sync --
  printConsole("Syncing NTP Time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); 
  
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
    client.setInsecure(); 
    
    HTTPClient http;
    http.begin(client, apiUrl);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String payload = http.getString();
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        // Save the launch name to our global variable for the OLED scroller
        globalLaunchName = doc["name"].as<String>();
        
        String launchLoc = doc["location"].as<String>();
        String launchDateStr = doc["date"].as<String>();
        
        launchDateStr.replace("T", " ");
        launchDateStr.replace(":00Z", " UTC"); 

        mainText = globalLaunchName + " | " + launchLoc + " | " + launchDateStr + " | ";
        
        launchTimestamp = doc["timestamp"].as<unsigned long>();
        isCountingDown = true;
        
        // Print to console, then trigger the 2-second delay before scrolling starts
        printConsole("Data Synced.");
        dataSyncedTime = millis();
        pendingNameScroll = true;
        
      } else {
        printConsole("JSON Error!");
      }
    } else {
      printConsole("HTTP Error: " + String(httpResponseCode));
    }
    http.end();
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

  // 1. Draw Scrolling Blue Text
  matrix.setCursor(cursorX, 0);
  matrix.setTextColor(matrix.Color(0, 0, 255)); 
  matrix.print(mainText);

  int textWidth = mainText.length() * 6;
  
  // 2. Draw the Rocket Sprite in the gap!
  // Places the rocket 30 pixels after the end of the text
  int rocketX = cursorX + textWidth + 30; 
  
  // Draw the solid white rocket body (16px wide, 8px high)
  matrix.drawBitmap(rocketX, 0, rocket_bmp, 16, 8, matrix.Color(255, 255, 255));
  
  // Draw a flickering orange/red flame shooting out the back (to the right of the rocket)
  // random(3, 8) makes the flame physically flutter in length every frame
  matrix.fillRect(rocketX + 16, 2, random(3, 8), 4, matrix.Color(255, random(50, 150), 0));

  // 3. MASKING TRICK: Black box over the right side
  int countdownWidth = 64; 
  int countdownX = MATRIX_WIDTH - countdownWidth; 
  
  // Draws solid black to obliterate the blue text and rocket moving behind it
  matrix.fillRect(countdownX, 0, countdownWidth, 8, matrix.Color(0, 0, 0));

  // 4. Draw Static Countdown
  if (timeUntilLaunch > 0) {
    matrix.setTextColor(matrix.Color(0, 255, 0), matrix.Color(0, 0, 0)); 
  } else {
    matrix.setTextColor(matrix.Color(255, 0, 0), matrix.Color(0, 0, 0)); 
  }
  matrix.setCursor(countdownX + 2, 0); 
  
  String countdownString = formatCountdown(timeUntilLaunch);
  matrix.print(countdownString);

  matrix.show();

  // 5. Handle scrolling math
  cursorX--;
  
  // Update the reset boundary so it waits for the text, the gap, the rocket, AND the flame to pass
  int totalScrollWidth = textWidth + 30 + 16 + 10; 
  
  if (cursorX < -totalScrollWidth) {
    cursorX = MATRIX_WIDTH; 
  }
}
