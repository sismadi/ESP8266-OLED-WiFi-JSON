/*
  ESP8266 ESP-12E (NodeMCU) + OLED SSD1306 (128x64)
  Fitur:
    - AP (Setup) + STA (Normal)  -> / , /save
    - HTTP/HTTPS GET/POST JSON   -> config apiGet/apiPost (EEPROM)
    - Wi-Fi Scanner              -> /scan, /scan.json, /scan_oled
    - AP Clients                 -> /clients, /clients.json
    - OLED judul besar + word-wrap (2-warna: bar atas kuning)

  OLED I2C (sesuai board pada foto):
    SDA -> D5 (GPIO14)
    SCL -> D6 (GPIO12)
    VCC -> 3V3, GND -> GND
    I2C address: 0x3C
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

extern "C" {         // untuk daftar klien AP (SDK)
  #include <user_interface.h>
}

// ===================== OLED =====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----- OLED helpers: judul besar + word-wrap -----
void printWrapped(int x, int y, const String &text, uint8_t size, int maxW = 128) {
  const int charW = 6 * size;     // font default Adafruit_GFX
  const int lineH = 8 * size;
  const int maxChars = max(1, maxW / charW);

  String line, word;
  int cy = y;

  for (uint16_t i = 0; i <= text.length(); i++) {
    char c = (i < text.length()) ? text[i] : ' '; // paksa flush akhir
    if (c == ' ' || c == '\n' || i == text.length()) {
      String candidate = line + (line.length() ? " " : "") + word;
      if ((int)candidate.length() > maxChars || c == '\n') {
        display.setCursor(x, cy); display.println(line);
        line = word; cy += lineH;
      } else {
        line = candidate;
      }
      word = "";
      if (c == '\n') continue;
    } else {
      word += c;
    }
  }
  if (line.length()) { display.setCursor(x, cy); display.println(line); }
}

void showStatusOLED(const String &title, const String &body, const String &footer) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Judul besar di bar kuning (atas)
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(title);

  // Isi ter-wrap
  display.setTextSize(1);
  printWrapped(0, 18, body, 1);

  // Footer di bawah
  if (footer.length()) {
    display.setCursor(0, 54);
    display.println(footer);
  }
  display.display();
}

// ===================== CONFIG / EEPROM =====================
struct Config {
  uint32_t magic;           // "CFG1"
  char ssid[32];
  char pass[32];
  char apiGet[128];         // contoh: https://jsonplaceholder.typicode.com/users/1
  char apiPost[128];        // contoh: https://api.example.com/iot/post
};
Config config;
const uint32_t CFG_MAGIC = 0x43464731;

void saveConfig() { EEPROM.put(0, config); EEPROM.commit(); }
void loadConfig() {
  EEPROM.get(0, config);
  if (config.magic != CFG_MAGIC) {
    memset(&config, 0, sizeof(config));
    config.magic = CFG_MAGIC;
    strcpy(config.apiGet,  "https://jsonplaceholder.typicode.com/users/1");
    strcpy(config.apiPost, "");
    saveConfig();
  }
}
bool hasWiFiCreds() { return strlen(config.ssid) > 0; }

// ===================== WEB SERVER (AP Setup) =====================
ESP8266WebServer server(80);

String htmlHeader() {
  return F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<style>body{font-family:Arial;padding:16px}input,button{width:100%;padding:8px;margin:6px 0}"
           "label{display:block;margin-top:8px}code{background:#eee;padding:2px 4px;border-radius:3px}"
           "table{border-collapse:collapse;width:100%}td,th{border:1px solid #ddd;padding:6px}</style>"
           "</head><body>");
}
String htmlFooter() { return F("</body></html>"); }

// Map tipe enkripsi (ESP8266)
String encTypeToString(uint8_t t) {
  switch (t) {
    case ENC_TYPE_NONE: return "OPEN";
    case ENC_TYPE_WEP:  return "WEP";
    case ENC_TYPE_TKIP: return "WPA/TKIP";
    case ENC_TYPE_CCMP: return "WPA2/CCMP";
    case ENC_TYPE_AUTO: return "WPA/WPA2";
    default:            return String("UNK(") + String(t) + ")";
  }
}
String macToStr(const uint8_t* mac) {
  char b[18];
  sprintf(b, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

// ---------- Pages ----------
void handleRoot() {
  String h = htmlHeader();
  h += F("<h2>Setup WiFi & API</h2><form action='/save' method='GET'>");
  h += String("SSID:<input name='ssid' value='") + String(config.ssid) + "'><br>";
  h += String("Password:<input name='pass' type='password' value='") + String(config.pass) + "'><br>";
  h += String("API GET URL:<input name='get' value='") + String(config.apiGet) + "' "
       "placeholder='https://api.example.com/iot'><br>";
  h += String("API POST URL:<input name='post' value='") + String(config.apiPost) + "' "
       "placeholder='https://api.example.com/iot/post'><br>";
  h += F("<input type='submit' value='Save'></form><hr>");
  h += "<p>STA IP: <code>" + WiFi.localIP().toString() + "</code> | AP IP: <code>" + WiFi.softAPIP().toString() + "</code></p>";
  h += F("<a href='/scan'><button>Scan Wi-Fi (HTML)</button></a>");
  h += F("<a href='/scan_oled'><button>Scan Wi-Fi -> OLED (Top-3)</button></a>");
  h += F("<a href='/clients'><button>AP Clients (HTML)</button></a>");
  h += F("<p>AP Clients API: <a href='/clients.json'>/clients.json</a></p>");
  h += F("<p>Scanner API: <a href='/scan.json'>/scan.json</a></p>");
  h += htmlFooter();
  server.send(200, "text/html", h);
}

void handleSave() {
  if (server.hasArg("ssid")) server.arg("ssid").toCharArray(config.ssid, sizeof(config.ssid));
  if (server.hasArg("pass")) server.arg("pass").toCharArray(config.pass, sizeof(config.pass));
  if (server.hasArg("get"))  server.arg("get").toCharArray(config.apiGet, sizeof(config.apiGet));
  if (server.hasArg("post")) server.arg("post").toCharArray(config.apiPost, sizeof(config.apiPost));
  config.magic = CFG_MAGIC;
  saveConfig();
  server.send(200, "text/html", "Saved! Restarting...");
  delay(700);
  ESP.restart();
}

// ---------- Scanner helpers ----------
void scanTop3ToOLED() {
  showStatusOLED("SCAN", "Scanning Wi-Fi...", "");
  int n = WiFi.scanNetworks(); // blocking
  if (n <= 0) { showStatusOLED("SCAN", "No networks found.", ""); return; }

  int i1=-1,i2=-1,i3=-1; long r1=-999,r2=-999,r3=-999;
  for (int i=0;i<n;i++) {
    long r = WiFi.RSSI(i);
    if (r>r1){r3=r2;i3=i2;r2=r1;i2=i1;r1=r;i1=i;}
    else if (r>r2){r3=r2;i3=i2;r2=r;i2=i;}
    else if (r>r3){r3=r;i3=i;}
  }
  auto mk=[&](int idx)->String{
    if(idx<0) return "";
    String s=WiFi.SSID(idx); s.replace("\n"," ");
    return s + " (" + String(WiFi.RSSI(idx)) + "dBm)";
  };
  String body = mk(i1) + "\n" + (i2>=0? mk(i2): "") + (i3>=0? "\n"+mk(i3): "");
  showStatusOLED("TOP Wi-Fi", body, "");
}

void handleScanHTML() {
  int n = WiFi.scanNetworks();
  String h = htmlHeader();
  h += "<h2>Wi-Fi Scan (" + String(n) + ")</h2>";
  h += "<p><a href='/'>Back</a> | <a href='/scan'>Refresh</a> | <a href='/scan_oled'>Show Top-3 on OLED</a></p>";
  if (n <= 0) {
    h += "<p><b>No networks found.</b></p>";
  } else {
    h += "<table><tr><th>#</th><th>SSID</th><th>RSSI</th><th>Ch</th><th>Enc</th></tr>";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i); ssid.replace("<","&lt;"); ssid.replace(">","&gt;");
      h += "<tr>";
      h += "<td>" + String(i+1) + "</td>";
      h += "<td>" + ssid + "</td>";
      h += "<td>" + String(WiFi.RSSI(i)) + "</td>";
      h += "<td>" + String(WiFi.channel(i)) + "</td>";
      h += "<td>" + encTypeToString(WiFi.encryptionType(i)) + "</td>";
      h += "</tr>";
    }
    h += "</table>";
  }
  h += htmlFooter();
  server.send(200, "text/html", h);
}

void handleScanJSON() {
  int n = WiFi.scanNetworks();
  StaticJsonDocument<4096> doc;
  doc["count"] = n;
  JsonArray arr = doc.createNestedArray("networks");
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["channel"] = WiFi.channel(i);
    o["encryption"] = encTypeToString(WiFi.encryptionType(i));
    o["bssid"] = WiFi.BSSIDstr(i);
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleScanOLED() {
  scanTop3ToOLED();
  server.send(200, "text/plain", "Top-3 SSID ditampilkan di OLED.");
}

// ---------- AP Clients ----------
void handleClientsHTML() {
  String h = htmlHeader();
  int cnt = WiFi.softAPgetStationNum();
  h += "<h2>AP Clients (" + String(cnt) + ")</h2>";
  h += "<p>AP SSID: <b>SetupNodeMCU</b> | AP IP: <code>" + WiFi.softAPIP().toString() + "</code></p>";
  h += "<p><a href='/'>Back</a> | <a href='/clients'>Refresh</a></p>";
  h += "<table><tr><th>#</th><th>MAC</th><th>IP</th></tr>";

  struct station_info *stat_info = wifi_softap_get_station_info();
  int idx = 1;
  while (stat_info) {
    IPAddress ip = IPAddress((uint32_t)stat_info->ip.addr);
    h += "<tr><td>" + String(idx++) + "</td><td>" + macToStr(stat_info->bssid) +
         "</td><td>" + ip.toString() + "</td></tr>";
    stat_info = STAILQ_NEXT(stat_info, next);
  }
  wifi_softap_free_station_info();

  h += "</table>" + htmlFooter();
  server.send(200, "text/html", h);
}

void handleClientsJSON() {
  StaticJsonDocument<1024> doc;
  doc["ssid"]  = "SetupNodeMCU";
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["count"] = WiFi.softAPgetStationNum();

  JsonArray arr = doc.createNestedArray("clients");
  struct station_info *stat_info = wifi_softap_get_station_info();
  while (stat_info) {
    JsonObject o = arr.createNestedObject();
    o["mac"] = macToStr(stat_info->bssid);
    IPAddress ip = IPAddress((uint32_t)stat_info->ip.addr);
    o["ip"]  = ip.toString();
    stat_info = STAILQ_NEXT(stat_info, next);
  }
  wifi_softap_free_station_info();

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ===================== WIFI (AP + STA) =====================
void startAP_STA() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("SetupNodeMCU", "12345678");  // AP selalu aktif
  delay(100);

  if (hasWiFiCreds()) {
    WiFi.begin(config.ssid, config.pass);
    showStatusOLED("CONNECT", config.ssid, "AP: " + WiFi.softAPIP().toString());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(300); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      showStatusOLED("WiFi OK", WiFi.localIP().toString(),
                     "AP: " + WiFi.softAPIP().toString());
    } else {
      showStatusOLED("AP MODE", "SSID: SetupNodeMCU",
                     "IP: " + WiFi.softAPIP().toString());
    }
  } else {
    showStatusOLED("AP MODE", "SSID: SetupNodeMCU",
                   "IP: " + WiFi.softAPIP().toString());
  }

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/scan", handleScanHTML);
  server.on("/scan.json", handleScanJSON);
  server.on("/scan_oled", handleScanOLED);
  server.on("/clients", handleClientsHTML);
  server.on("/clients.json", handleClientsJSON);
  server.begin();
  Serial.println("Setup page: http://192.168.4.1/");
}

// ===================== HTTP Utils =====================
bool isHttpsUrl(const String &url) { return url.startsWith("https://"); }

bool httpGET_JSON(const String &url, String &payloadOut, int &httpCodeOut) {
  payloadOut = ""; httpCodeOut = 0;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  bool ok = false;

  if (isHttpsUrl(url)) {
    WiFiClientSecure cli; cli.setInsecure();
    if (http.begin(cli, url)) {
      http.addHeader("Accept", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.setTimeout(15000);
      httpCodeOut = http.GET();
      if (httpCodeOut > 0) {
        payloadOut = http.getString();
        ok = (httpCodeOut == HTTP_CODE_OK || httpCodeOut == HTTP_CODE_MOVED_PERMANENTLY);
      }
      http.end();
    }
  } else {
    WiFiClient cli;
    if (http.begin(cli, url)) {
      http.addHeader("Accept", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.setTimeout(15000);
      httpCodeOut = http.GET();
      if (httpCodeOut > 0) {
        payloadOut = http.getString();
        ok = (httpCodeOut == HTTP_CODE_OK || httpCodeOut == HTTP_CODE_MOVED_PERMANENTLY);
      }
      http.end();
    }
  }
  return ok;
}

bool httpPOST_JSON(const String &url, const String &jsonPayload, String &respOut, int &httpCodeOut) {
  respOut = ""; httpCodeOut = 0;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  bool ok = false;

  if (isHttpsUrl(url)) {
    WiFiClientSecure cli; cli.setInsecure();
    if (http.begin(cli, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.setTimeout(15000);
      httpCodeOut = http.POST(jsonPayload);
      if (httpCodeOut > 0) { respOut = http.getString(); ok = true; }
      http.end();
    }
  } else {
    WiFiClient cli;
    if (http.begin(cli, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.setTimeout(15000);
      httpCodeOut = http.POST(jsonPayload);
      if (httpCodeOut > 0) { respOut = http.getString(); ok = true; }
      http.end();
    }
  }
  return ok;
}

// ===================== LOGIKA UTAMA =====================
unsigned long lastGet = 0;
unsigned long lastPost = 0;

void fetchAndDisplayJson() {
  String url = String(config.apiGet);
  if (url.length() == 0) {
    showStatusOLED("GET", "GET URL not set", "Open: 192.168.4.1");
    return;
  }

  showStatusOLED("GET", "Fetching...\n" + url.substring(0, 32), "");
  String payload; int code;
  bool ok = httpGET_JSON(url, payload, code);

  if (!ok) {
    showStatusOLED("GET ERR", "HTTP code: " + String(code), "");
    Serial.printf("[HTTP] GET failed code=%d\n", code);
    return;
  }

  Serial.println("\n--- RAW JSON BODY (GET) ---");
  Serial.println(payload);
  Serial.println("---------------------------");

  // Parse JSON dan ambil 'name' (dengan fallback)
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    showStatusOLED("JSON ERR", err.c_str(), "");
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return;
  }

  String name = "No Name";
  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (root.is<JsonObjectConst>()) {
    JsonObjectConst o = root.as<JsonObjectConst>();
    if (o["name"].is<const char*>())        name = o["name"].as<const char*>();
    else if (o["nama"].is<const char*>())   name = o["nama"].as<const char*>();
    else if (o["message"].is<const char*>())name = o["message"].as<const char*>();
    else if (o["title"].is<const char*>())  name = o["title"].as<const char*>();
  }
  if (name == "No Name" && root.is<JsonArrayConst>()) {
    JsonArrayConst a = root.as<JsonArrayConst>();
    if (a.size() > 0 && a[0]["name"].is<const char*>()) name = a[0]["name"].as<const char*>();
  }

  showStatusOLED("GET OK", name, "IP: " + WiFi.localIP().toString());
  Serial.printf("Name: %s\n", name.c_str());
}

void maybePostJson() {
  if (strlen(config.apiPost) == 0) return;  // POST opsional
  StaticJsonDocument<256> p;
  p["device"] = "ESP8266-ESP12E";
  p["rssi"] = WiFi.RSSI();
  p["uptime_ms"] = millis();
  String payload; serializeJson(p, payload);

  String resp; int code;
  bool ok = httpPOST_JSON(String(config.apiPost), payload, resp, code);
  if (ok) {
    Serial.printf("[POST] code=%d resp.len=%u\n", code, (unsigned)resp.length());
  } else {
    Serial.printf("[POST] failed code=%d\n", code);
  }
}

void setup() {
  Serial.begin(115200);

  // I2C OLED: gunakan pin dari board (SDA=D5, SCL=D6)
  Wire.begin(D5, D6);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED not found at 0x3C"));
    while (true) { delay(1000); }
  }
  display.dim(false);  // brightness normal

  // Splash
  showStatusOLED("BOOT", "ESP8266 ESP-12E\nWiFi + OLED Ready", "SDA=D5  SCL=D6");
  delay(900);

  EEPROM.begin(512);
  loadConfig();
  startAP_STA();
}

void loop() {
  server.handleClient();

  // GET setiap 10s
  if (millis() - lastGet > 10000) {
    lastGet = millis();
    if (WiFi.status() == WL_CONNECTED) fetchAndDisplayJson();
    else showStatusOLED("NO WiFi", "Open setup portal", "192.168.4.1");
  }

  // POST setiap 30s bila diisi
  if (millis() - lastPost > 30000) {
    lastPost = millis();
    if (WiFi.status() == WL_CONNECTED) maybePostJson();
  }
}
