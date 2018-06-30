#include <Wire.h>
#include <FS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Adafruit_SSD1306.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Ethernet.h>
#include <SPI.h>
#include <WiFiUdp.h>

// -------------------- CONFIGURATION --------------------

#define JSON_BUFF_SIZE 512

#define WIFI_TIMEOUT 30000

#define BADGE_TEAM_DEFAULT 1
#define BADGE_ID_DEFAULT 1
#define BADGE_TEAM_MAX 50
#define BADGE_ID_MAX 99

const char STR_WAIT      [] PROGMEM = "Please wait...";
const char STR_OTA_UPDATE[] PROGMEM = "OTA Update";
const char STR_OTA_DONE  [] PROGMEM = "Done. Rebooting...";

const char STR_FILE_WIFI   [] = "/wifi.json";
const char STR_FILE_CONF   [] = "/conf.json";
const char STR_NAME_DEFAULT[] = "Team 1-1";

const RgbColor COLOR_OFF     (  0,   0,   0);
const RgbColor COLOR_DEFAULT (  0,   0, 128);
const RgbColor COLOR_ECHO    ( 20, 150,   0);

const uint16_t  NET_PORT   = 11337;
const IPAddress NET_SERVER ( 10,  13,  37, 100);
const IPAddress NET_GROUP  (239,  13,  37,   1);

// -------------------- CONFIGURATION --------------------
const uint16_t PixelCount = 8; // make sure to set this to the number of pixels in your strip
const uint16_t PixelPin = 2;  // make sure to set this to the correct pin, ignored for Esp8266
const uint16_t AnimCount = PixelCount / 5 * 2 + 1; // we only need enough animations for the tail and one extra

const uint16_t PixelFadeDuration = 100; // half of a second
// one second divide by the number of pixels = loop once a second
const uint16_t NextPixelMoveDuration = 1000 / PixelCount; // how fast we move through the pixels

NeoGamma<NeoGammaTableMethod> colorGamma; // for any fade animations, best to correct gamma

struct MyAnimationState
{
    RgbColor StartingColor;
    RgbColor EndingColor;
    uint16_t IndexPixel;
};

NeoPixelAnimator animations(AnimCount); // NeoPixel animation management object
MyAnimationState animationState[AnimCount];
uint16_t frontPixel = 0;  // the front of the loop
RgbColor frontColor;  // the color at the front of the loop

// ---------- BADGE GLOBALS - CAN BE REMOVED ----------

enum sigval {
    SIG_LOW  = -85,
    SIG_MED  = -65,
    SIG_HIGH = -55
};

typedef struct textscroll {
    int16_t length;
    int16_t offset;
} textscroll_t;

const int COLORS_COUNT = 3;
typedef struct badge {
    uint8_t      team;
    uint8_t      id;
    RgbColor     color;
    // RgbColor     colors[COLORS_COUNT];
    char         name[256];
    textscroll_t scroll;
} badge_t;

typedef struct event {
    ulong interval;
    void  (*callback)(void);
    ulong prev;
} event_t;

typedef struct flash {
    RgbColor* color;
    uint8_t   count;
} flash_t;

typedef void(*handler_f)(uint8_t cmd);

badge_t badge;
flash_t flash;

// ---------- BADGE GLOBALS - CAN BE REMOVED ----------

// ---------- OTA GLOBALS - DO NOT MODIFY ----------

char moduleName[15];

bool setupMode = false;
bool connected = false;

int   i;
File  file;
uint  size;
char  name[256];
char  temp[2048];

// ---------- OTA GLOBALS - DO NOT MODIFY ----------

// ---------- OTA FUNCTIONS - DO NOT MODIFY ----------

Adafruit_SSD1306 oled(16);
NeoPixelBus<NeoGrbFeature, NeoEsp8266Uart800KbpsMethod> pixel(PixelCount, 0);
WiFiUDP udp;

void otaStart() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(FPSTR(STR_OTA_UPDATE));
    oled.setCursor(0, 10);
    oled.print(FPSTR(STR_WAIT));
    oled.display();
}

void otaEnd() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(FPSTR(STR_OTA_UPDATE));
    oled.setCursor(0, 10);
    oled.print(FPSTR(STR_OTA_DONE));
    oled.display();
}

void enableSetupMode(const char* msg) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(msg);
    oled.setCursor(0, 10);
    oled.print("Running Setup Mode");
    oled.display();
    delay(2000);
    setupMode = true;
}

void runSetupMode() {
    setupMode = true;

    // Disconnect from any existing network
    WiFi.disconnect();
    delay(10);

    // Switch to AP mode
    WiFi.mode(WIFI_AP);
    delay(10);
    WiFi.softAP(moduleName);

    // Render details
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print("AP: ");
    oled.print(moduleName);
    oled.setCursor(0, 10);
    oled.print(WiFi.softAPIP());
    oled.setCursor(0, 20);
    oled.print("Setup Mode");
    oled.display();
}

// ---------- OTA FUNCTIONS - DO NOT MODIFY ----------

// ---------- BADGE FUNCTIONS - CAN BE REMOVED ----------

void updateName(const char* name) {
    // Either copy the name or the default name
    strncpy(
        badge.name,
        (name != NULL) ? name : STR_NAME_DEFAULT,
        sizeof(badge.name)-2
    );

    // Make sure the string is terminated with a newline and a null character
    badge.name[strnlen(name, sizeof(badge.name)-2)  ] = '\n';
    badge.name[strnlen(name, sizeof(badge.name)-2)+1] = '\0';

    // Update the name rendering length
    int16_t ox, oy; uint16_t ow, oh;
    oled.setTextSize(2);
    oled.getTextBounds(badge.name, 0, 0, &ox, &oy, &ow, &oh);
    badge.scroll.length = -1 * ow;
    badge.scroll.offset = SSD1306_LCDWIDTH;
}

void updateColor(const RgbColor* color, uint8_t count) {
    flash.count = (count == 0) ? 0 : count * 2 + 1;
    flash.color = const_cast<RgbColor*>(color);
}

void saveConfig() {
    if((file = SPIFFS.open(STR_FILE_CONF, "w"))) {
        StaticJsonBuffer<JSON_BUFF_SIZE> buff;
        JsonObject& root = buff.createObject();

        root["team"] = badge.team;
        root["id"  ] = badge.id;
        JsonArray& color = root.createNestedArray("color");
        color.add(badge.color.R);
        color.add(badge.color.G);
        color.add(badge.color.B);

        // Trim off newline when saving the name
        size = strnlen(badge.name, sizeof(badge.name)-1) - 1;
        strncpy(name, badge.name, size);
        name[size] = '\0';
        root["name"] = name;

        size = root.prettyPrintTo(temp, sizeof(temp)-1);
        temp[size] = '\0';
        file.print(temp);
        file.close();
    }
}

void renderRSSI() {
    int rssi = WiFi.RSSI();
    oled.fillRect(114, 0, 14, 8, BLACK);

    if(rssi > SIG_LOW )
         oled.fillRect(114, 5, 4, 3, WHITE);
    else oled.drawRect(114, 5, 4, 3, WHITE);

    if(rssi > SIG_MED )
         oled.fillRect(119, 3, 4, 5, WHITE);
    else oled.drawRect(119, 3, 4, 5, WHITE);

    if(rssi > SIG_HIGH)
         oled.fillRect(124, 0, 4, 8, WHITE);
    else oled.drawRect(124, 0, 4, 8, WHITE);
}

void renderName() {
    oled.fillRect(0, 10, 128, 22, BLACK);
    oled.setTextSize(2);
    oled.setCursor(badge.scroll.offset, 10);
    oled.print(badge.name);
    oled.display();

    badge.scroll.offset -= 3;
    if(badge.scroll.offset < badge.scroll.length)
        badge.scroll.offset = SSD1306_LCDWIDTH;
}

void renderTeam() {
    oled.fillRect(80, 0, 30, 10, BLACK);
    oled.setTextSize(1);
    oled.setCursor(80, 0);
    oled.printf("%02d-%02d", badge.team, badge.id);
    oled.display();
}

// void renderColor() {
//     pixel.ClearTo((flash.count & 1) ? COLOR_OFF : *(flash.color));
//     pixel.Show();
//     if(flash.count >  0) flash.count--;
//     if(flash.count == 0) flash.color = &(badge.color);
// }

event_t event[] = {
    {  33, renderName , 0 },
    {  40, renderRSSI , 0 },
    {  40, renderTeam , 0 },
//    { 250, renderColor, 0 }
};
const int RENDERNAME_EVENT = 0;
const int RENDERRSSI_EVENT = 1;
const int RENDERTEAM_EVENT = 2;
const int RENDERCOLOR_EVENT = 3;

void runEvents() {
    ulong curr = millis();
    for(i = 0; i < 4; ++i) {
        if((curr - event[i].prev) >= event[i].interval) {
            event[i].prev = curr;
            event[i].callback();
        }
    }
}

// void handleTeamChange(uint8_t cmd) {
//     uint8_t team = udp.read();
//     uint8_t id   = udp.read();
//     badge.team = constrain(team, 1, BADGE_TEAM_MAX);
//     badge.id   = constrain(id,   1, BADGE_ID_MAX);
//     saveConfig();
// }

// void handleColorChange(uint8_t cmd) {
//     uint8_t r = udp.read();
//     uint8_t g = udp.read();
//     uint8_t b = udp.read();
//     badge.color.R = constrain(r, 0, 255);
//     badge.color.G = constrain(g, 0, 255);
//     badge.color.B = constrain(b, 0, 255);
//     saveConfig();
// }

// void handleTeamColorChange(uint8_t cmd) {
//     uint8_t team = udp.read();
//     team = constrain(team, 1, BADGE_TEAM_MAX);
//     if(team == badge.team) handleColorChange(cmd);
// }

// void handleNameChange(uint8_t cmd) {
//     uint8_t length = std::min(size, sizeof(badge.name)-2);
//     udp.read(name, length);
//     name[length] = '\0';
//     updateName(name);
//     saveConfig();
// }

// void handleEcho(uint8_t cmd) {
//     if(badge.team > 8) return;
//     udp.beginPacket(NET_SERVER, NET_PORT);
//     udp.write(cmd);
//     udp.write(badge.team);
//     udp.write(badge.id);
//     udp.endPacket();
//     updateColor(&COLOR_ECHO, 2);
// }

handler_f handler[] = {
//     handleTeamChange,
//     handleColorChange,
//     handleTeamColorChange,
//     handleNameChange,
//     handleEcho
};

void handleRequests() {
    if(udp.parsePacket() > 0) {
        if((size = udp.available()) > 0) {
            uint8_t cmd = udp.read(); size--;
            if(cmd > 0 && cmd < 6) handler[cmd-1](cmd);
        }
        udp.flush();
    }
}

// ---------- BADGE FUNCTIONS - CAN BE REMOVED ----------

// ---------- FADE IN OUT ANIMATION INIT ----------
void SetRandomSeed()
{
    uint32_t seed;

    // random works best with a seed that can use 31 bits
    // analogRead on a unconnected pin tends toward less than four bits
    seed = analogRead(0);
    delay(1);

    for (int shifts = 3; shifts < 31; shifts += 3)
    {
        seed ^= analogRead(0) << shifts;
        delay(1);
    }

    randomSeed(seed);
}

void FadeOutAnimUpdate(const AnimationParam& param)
{
    // this gets called for each animation on every time step
    // progress will start at 0.0 and end at 1.0
    // we use the blend function on the RgbColor to mix
    // color based on the progress given to us in the animation
    RgbColor updatedColor = RgbColor::LinearBlend(
        animationState[param.index].StartingColor,
        animationState[param.index].EndingColor,
        param.progress);
    // apply the color to the strip
    pixel.SetPixelColor(animationState[param.index].IndexPixel,
        colorGamma.Correct(updatedColor));
}

void LoopAnimUpdate(const AnimationParam& param)
{
    // wait for this animation to complete,
    // we are using it as a timer of sorts
    if (param.state == AnimationState_Completed)
    {
        // done, time to restart this position tracking animation/timer
        animations.RestartAnimation(param.index);

        // pick the next pixel inline to start animating
        //
        frontPixel = (frontPixel + 1) % PixelCount; // increment and wrap
        if (frontPixel == 0)
        {
            // we looped, lets pick a new front color
            frontColor = HslColor(random(360) / 360.0f, 1.0f, 0.25f);
        }

        uint16_t indexAnim;
        // do we have an animation available to use to animate the next front pixel?
        // if you see skipping, then either you are going to fast or need to increase
        // the number of animation channels
        if (animations.NextAvailableAnimation(&indexAnim, 1))
        {
            animationState[indexAnim].StartingColor = frontColor;
            animationState[indexAnim].EndingColor = RgbColor(0, 0, 0);
            animationState[indexAnim].IndexPixel = frontPixel;

            animations.StartAnimation(indexAnim, PixelFadeDuration, FadeOutAnimUpdate);
        }
    }
}

// ---------- WEB SERVER COMPONENT ----------
ESP8266WebServer web(80);

void route_allo() {
  web.send(200, "text/plain", "Hello from esp8266!");
}

void route_notFound() {
  String message = "Resource Not Found\n\n";
  message += "URI: ";
  message += web.uri();
  message += "\nMethod: ";
  message += (web.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += web.args();
  message += "\n";
  for (uint8_t i=0; i<web.args(); i++){
    message += " " + web.argName(i) + ": " + web.arg(i) + "\n";
  }
  web.send(404, "text/plain", message);
}

void route_v1_isSetupMode() {
  char outputBuffer[2048];

  uint outputLen;
  StaticJsonBuffer<JSON_BUFF_SIZE> buff;
  JsonObject& root = buff.createObject();

  root["status"] = "OK";
  root["data"] = setupMode;

  outputLen = root.prettyPrintTo(outputBuffer, sizeof(outputBuffer)-1);
  outputBuffer[outputLen] = '\0';

  web.send(200, "application/json", temp);
}

void route_v1_setWiFiAP() {

}

void setupModeWeb() {
  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  web.on("/", route_allo);
  web.on("/api/v1/setup/isSetupMode", HTTP_GET, route_v1_isSetupMode);
  // web.on("/api/v1/setup/setWiFiAP", HTTPMethod::HTTP_POST, route_v1_setWiFiAP);

  web.onNotFound(route_notFound);

  //web.begin();
  Serial.println("HTTP server started");
}

void setupProductionWeb() {
  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  web.on("/", route_allo);
  web.on("/api/v1/setup/isSetupMode", HTTP_GET, route_v1_isSetupMode);
  // web.on("/api/v1/setup/setWiFiAP", HTTPMethod::HTTP_POST, route_v1_setWiFiAP);

  web.onNotFound(route_notFound);

  // web.begin();
  Serial.println("HTTP server started");
}

// ---------- WEB SERVER COMPONENT ----------

// ---------- FADE IN OUT ANIMATION INIT ----------

void setup() {
    // ---------- BADGE INITIALIZATION - DO NOT MODIFY ----------

    sprintf(moduleName, "esp8266-%06x", ESP.getChipId());

    // Initialize the serial port
    Serial.begin(115200);
    Serial.println();
    Serial.printf("--- %s ---\n", moduleName);

    // Initialize the oled
    oled.begin();
    oled.clearDisplay();
    oled.setTextColor(WHITE);
    oled.setTextWrap(false);
    oled.display();

    // Initialize the neopixel
    pixel.Begin();
    pixel.Show();

    // Set random seed for animations
    SetRandomSeed();

    setupMode = false;
    connected = false;

    // Initialize the file system
    if(!SPIFFS.begin()) {
        enableSetupMode("SPIFFS mount failure");
    }

    // ---------- BADGE INITIALIZATION - DO NOT MODIFY ----------

    // ---------- BADGE CONFIGURATION - CAN BE REMOVED ----------

    // Set badge defaults
    badge.team  = BADGE_TEAM_DEFAULT;
    badge.id    = BADGE_ID_DEFAULT;
    badge.color = COLOR_DEFAULT;
    updateName(STR_NAME_DEFAULT);

    // Load the badge configuration file
    if(!setupMode && (file = SPIFFS.open(STR_FILE_CONF, "r"))) {
        size = file.readBytes(temp, file.size());
        if(size == file.size()) {

            // Parse the badge configuration
            temp[size] = '\0';
            StaticJsonBuffer<JSON_BUFF_SIZE> buff;
            JsonObject& root = buff.parseObject(temp);

            if(root.success()) {
                // Make sure things are in byte range
                // Also make sure that the default will be 1-1
                badge.team  = constrain(root["team"], 1, 255);
                badge.id    = constrain(root["id"]  , 1, 255);

                // Parse the badge color
                JsonArray& color = root["color"];
                if(color.success() && color.size() == 3) {
                    // Make sure each element is in range (0-255)
                    badge.color.R = constrain(color[0], 0, 255);
                    badge.color.G = constrain(color[1], 0, 255);
                    badge.color.B = constrain(color[2], 0, 255);
                }
                // Fall back to the default if the color isn't valid
                else badge.color = COLOR_DEFAULT;

                // Parse the badge name
                const char* name = root["name"];
                updateName(name);
            }
            else {
                enableSetupMode("Badge conf invalid");
            }
        }
        else {
            enableSetupMode("Badge conf not found");
        }
        file.close();
    }

    // Set the badge color
    updateColor(&(badge.color), 0);
    pixel.ClearTo(badge.color);
    pixel.Show();

    animations.StartAnimation(0, NextPixelMoveDuration, LoopAnimUpdate);

    // ---------- BADGE CONFIGURATION - CAN BE REMOVED ----------

    // ---------- OTA CONFIGURATION - DO NOT MODIFY ----------

    // Try to connect to the wifi defined by the configuration
    if(!setupMode && (file = SPIFFS.open(STR_FILE_WIFI, "r"))) {
        size = file.readBytes(temp, file.size());
        if(size == file.size()) {

            // Parse the wifi configuration
            temp[size] = '\0';
            StaticJsonBuffer<JSON_BUFF_SIZE> buff;
            JsonObject& root = buff.parseObject(temp);

            if(root.success()) {

                // Extract the SSID and password
                // This assumes that the parser will return NULL when
                // a key is unset
                const char* ssid = root["ssid"];
                const char* pass = root["pass"];

                if(ssid != NULL) {
                    oled.clearDisplay();
                    oled.setTextSize(1);
                    oled.setCursor(0, 0);
                    oled.printf("SSID: %s", ssid);
                    oled.setCursor(0, 10);
                    oled.print("Connecting:");
                    oled.setCursor(0, 20);
                    oled.display();

                    // The ESP needs a little time to change modes
                    WiFi.mode(WIFI_STA);
                    delay(10);

                    // Try to connect for allotted time
                    WiFi.begin(ssid, pass);
                    ulong start = millis();
                    uint  count = 0;
                    while(!(connected = (WiFi.status() == WL_CONNECTED)) &&
                          (millis()-start) < WIFI_TIMEOUT) {
                        if((count = (count + 1) % 4) == 3) {
                            oled.print('.');
                            oled.display();
                        }
                        delay(500);
                    }

                    // If we're still not connected, use setup mode
                    if(!connected) enableSetupMode("Unable to connect WiFi");
                }
                else {
                    enableSetupMode("SSID unset");
                }
            }
            else {
                enableSetupMode("WiFi conf invalid");
            }
        }
        else {
            enableSetupMode("WiFi conf not found");
        }
        file.close();
    }

    if(connected) {
        // If we did connect, display the IP we got and the badge info
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setCursor(0, 0);
        oled.print(WiFi.localIP());
        oled.display();

        // Start the UDP server
        udp.beginMulticast(WiFi.localIP(), NET_GROUP, NET_PORT);

        // Start the web server (production mode)
        setupProductionWeb();
    }
    // If unable to connect to the configured station, use AP mode
    else {
        runSetupMode();

        // Start the web server (setup mode)
        setupModeWeb();
    }

    // Setup OTA updates
    ArduinoOTA.onStart(otaStart);
    ArduinoOTA.onEnd(otaEnd);
    ArduinoOTA.begin();

    // ---------- OTA CONFIGURATION - DO NOT MODIFY ----------
}

void loop() {
    // PIXEL ANIMATION
    animations.UpdateAnimations();
    pixel.Show();

    // Handle OTA requests
    ArduinoOTA.handle();

    // Handle web requests
    web.handleClient();

    // ---------- USER CODE GOES HERE ----------

    // Process all time-based events without blocking
    runEvents();

    // Handle incoming UDP requests
    handleRequests();

    // ---------- USER CODE GOES HERE ----------

    // Setup should only handle OTA requests
    // if(setupMode) return yield();

    // Let the ESP do any other background things
    yield();
}
