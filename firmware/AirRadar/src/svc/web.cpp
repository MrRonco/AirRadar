// web.cpp — on-device web server: config UI, JSON API, screenshot, metrics, OTA.
//
// Ported from the proven v6 handlers (handleRoot/handleSave/handleWifi/
// handleNet/handleForget/startServer) with the v7 surface added:
// /integrations, /api/state, /api/config (GET+POST), /screen.bmp, /metrics,
// /update (OTA). All handlers run in loop context (webLoop -> handleClient),
// so touching g_tracks / g_set / NVS here is legal per the threading contract.
//
// Security: HTTP Basic auth ("admin"/<panelPass>) guards EVERY handler when a
// panel password is set. Stored passwords are never echoed into HTML or JSON.
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "web.h"
#include "mqtt.h"
#include "../config.h"
#include "../core/state.h"
#include "../core/tracks.h"
#include "../net/feeder.h"
#include "../net/enrich.h"
#include "../net/maptiles.h"
#include "../ui/theme.h"
#include "../hal/hal_display.h"

static WebServer server(80);
static bool s_serverUp = false;

// ---------- file-local constants (no magic numbers) ----------
static const uint32_t kRebootDelayMs   = 400;    // let the response flush out
static const size_t   kRootPageReserve = 4096;   // root page String pre-alloc
static const size_t   kStateDocBytes   = 16384;  // /api/state JSON budget
static const size_t   kCfgDocBytes     = 3072;   // /api/config JSON budget
static const size_t   kStateOutReserve = 4096;   // serialize buffer pre-alloc
static const int      kBmpHeaderBytes  = 54;     // BMP file+DIB header
static const int      kBmpDibBytes     = 40;     // BITMAPINFOHEADER
static const int      kBmpBpp          = 24;     // 24-bit RGB
static const int      kBmpRowBytes     = SCR_W * 3;
static const int      kAltFilterMaxFt  = 60000;  // sanity cap for alt filters
static const size_t   kTzMaxLen        = 64;     // POSIX TZ string cap
static const char*    kNtpServer1      = "pool.ntp.org";
static const char*    kNtpServer2      = "time.nist.gov";

// ============================================================
//  Small helpers
// ============================================================
static bool authed() {
  // Cross-site guard: browsers auto-attach Basic credentials, so a malicious
  // page could POST here from another origin. A browser always sends Origin
  // on cross-site POSTs — reject any Origin that isn't this device.
  if (server.hasHeader("Origin")) {
    String o = server.header("Origin");
    String hostHdr = server.hasHeader("Host") ? server.header("Host") : "";
    if (o.length() && hostHdr.length() && o.indexOf(hostHdr) < 0) {
      Serial.printf("[web] cross-origin request blocked: %s\n", o.c_str());
      server.send(403, "text/plain", "cross-origin request rejected");
      return false;
    }
  }
  if (!g_set.panelPass.length()) return true;
  if (server.authenticate("admin", g_set.panelPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

// Escape user-stored strings before placing them inside HTML attributes.
static String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += F("&amp;");  break;
      case '<':  o += F("&lt;");   break;
      case '>':  o += F("&gt;");   break;
      case '"':  o += F("&quot;"); break;
      case '\'': o += F("&#39;");  break;
      default:   o += c;           break;
    }
  }
  return o;
}

// "mqtt://user:pass@host:port" -> "mqtt://host:port" (never leak credentials).
static String mqttUriRedacted(const String& uri) {
  int sep = uri.indexOf("://");
  if (sep < 0) return uri;
  int at = uri.lastIndexOf('@');
  if (at <= sep) return uri;
  return uri.substring(0, sep + 3) + uri.substring(at + 1);
}

static bool argBool(const String& v) {
  return v == "1" || v == "true" || v == "on";
}

static bool rangeStepValid(int km) {
  static const int steps[] = AR_RANGE_STEPS;
  for (int s : steps)
    if (km == s) return true;
  return false;
}

// Invalidate every track after home/feed changes (loop context only).
static void resetTracks() {
  for (int i = 0; i < AR_MAX_TRACKS; i++) g_tracks[i].valid = false;
  g_selHex[0] = 0;
  g_orderN = 0;
}

// Send an HTML notice, then reboot (WiFi / network / DHCP switches).
static void webReboot(const String& msg) {
  String h = F("<!doctype html><meta name=viewport content='width=device-width,"
               "initial-scale=1'><body style='font-family:system-ui;background:#0b0f15;"
               "color:#dfe8f2;padding:2em'>");
  h += msg;
  h += F("</body>");
  server.send(200, "text/html", h);
  delay(kRebootDelayMs);
  ESP.restart();
}

// ============================================================
//  Root page — v6 look (dark #0b0f15, cyan buttons) + v7 sections
// ============================================================
static void htmlAppendHead(String& h) {
  h += F("<!doctype html><html><head><meta name=viewport content='width=device-width,"
         "initial-scale=1'><title>AirRadar</title><style>body{font-family:system-ui;"
         "background:#0b0f15;color:#dfe8f2;max-width:420px;margin:2em auto;padding:0 1em}"
         "input,select{width:100%;padding:9px;margin:4px 0 14px;background:#151d28;"
         "color:#dfe8f2;border:1px solid #33455c;border-radius:8px}"
         "input[type=checkbox]{width:auto;margin:0 8px 0 0}input[type=file]{padding:6px}"
         "button{padding:10px 20px;background:#4cc2ff;color:#08131c;border:0;"
         "border-radius:8px;margin-right:8px;font-weight:600}.d{background:#e05555;"
         "color:#fff}h3{margin:26px 0 6px;color:#8fa3b8}.n{color:#5f7488;font-size:.85em}"
         "a{color:#8fa3b8}</style></head><body><h2>AirRadar settings</h2>");
}

static void htmlAppendSettings(String& h) {
  h += F("<form method=post action=/save>Latitude<input name=lat value='");
  h += String(g_set.homeLat, 6);
  h += F("'>Longitude<input name=lon value='");
  h += String(g_set.homeLon, 6);
  h += F("'>Feeder URL<input name=feed value='");
  h += htmlEscape(g_set.feedUrl);
  h += F("'>Labels <select name=lbl><option value=1");
  if (g_set.showLabels) h += F(" selected");
  h += F(">On</option><option value=0");
  if (!g_set.showLabels) h += F(" selected");
  h += F(">Off</option></select><br><br><button type=submit>Save</button></form>");
}

static void htmlAppendWifi(String& h) {
  h += F("<h3>Wi-Fi</h3>"
         "<form method=post action=/wifi onsubmit='return confirm(\"Save Wi-Fi and reboot?\")'>"
         "SSID<input name=ssid value='");
  h += htmlEscape(g_set.wifiSsid);
  h += F("'>Password<input type=password name=pass>"
         "<button type=submit>Save &amp; reboot</button></form>");
}

static void htmlAppendNetwork(String& h, const String& pIp, const String& pGw,
                              const String& pMk, const String& pDn) {
  h += F("<h3>Network</h3>"
         "<form method=post action=/net onsubmit='return confirm(\"Apply network settings and reboot?\")'>"
         "Mode <select name=mode><option value=dhcp");
  if (!g_set.netStatic) h += F(" selected");
  h += F(">DHCP</option><option value=static");
  if (g_set.netStatic) h += F(" selected");
  h += F(">Static</option></select>IP address<input name=nip value='");
  h += htmlEscape(pIp);
  h += F("'>Gateway<input name=ngw value='");
  h += htmlEscape(pGw);
  h += F("'>Subnet mask<input name=nmask value='");
  h += htmlEscape(pMk);
  h += F("'>DNS (blank = gateway)<input name=ndns value='");
  h += htmlEscape(pDn);
  h += F("'><button type=submit>Save &amp; reboot</button></form>");
}

static void htmlAppendIntegrations(String& h) {
  h += F("<h3>Integrations</h3><form method=post action=/integrations>"
         "<label><input type=checkbox name=mqtten value=1");
  if (g_set.mqttEn) h += F(" checked");
  h += F(">MQTT enabled</label><br><br>"
         "MQTT URI (blank = keep, - = clear)<input name=mqtturi placeholder='");
  if (g_set.mqttUri.length()) h += htmlEscape(mqttUriRedacted(g_set.mqttUri));
  else h += F("mqtt://user:pass@host:1883");
  h += F("'>Timezone (POSIX TZ)<input name=tz value='");
  h += htmlEscape(g_set.tz);
  h += F("'>Panel password (blank = keep, - = clear)"
         "<input type=password name=ppass>"
         "<button type=submit>Save</button></form>");
}

static void htmlAppendFirmware(String& h) {
  h += F("<h3>Firmware</h3><p class=n>Current: " AR_VERSION "</p>"
         "<form method=post action=/update enctype='multipart/form-data' "
         "onsubmit='return confirm(\"Flash this firmware and reboot?\")'>"
         "<input type=file name=fw accept='.bin'>"
         "<button type=submit>Upload &amp; flash</button></form>");
}

static void htmlAppendFooter(String& h) {
  h += F("<br><form method=post action=/forget onsubmit='return confirm(\"Forget WiFi and reboot?\")'>"
         "<button class=d>Forget Wi-Fi</button></form>"
         "<p class=n>AirRadar " AR_VERSION " &middot; "
         "<a href='https://" AR_REPO_URL "'>" AR_REPO_URL "</a><br>"
         AR_AUTHOR_LINE "<br>"
         "<a href='/api/state'>/api/state</a> &middot; "
         "<a href='/screen.bmp'>/screen.bmp</a> &middot; "
         "<a href='/metrics'>/metrics</a></p></body></html>");
}

static void handleRoot() {
  if (!authed()) return;
  String pIp = g_set.netIp, pGw = g_set.netGw, pMk = g_set.netMask, pDn = g_set.netDns;
  if (!g_set.netStatic && WiFi.status() == WL_CONNECTED) {
    pIp = WiFi.localIP().toString();
    pGw = WiFi.gatewayIP().toString();
    pMk = WiFi.subnetMask().toString();
    pDn = WiFi.dnsIP().toString();
  }
  String h;
  h.reserve(kRootPageReserve);
  htmlAppendHead(h);
  htmlAppendSettings(h);
  htmlAppendWifi(h);
  htmlAppendNetwork(h, pIp, pGw, pMk, pDn);
  htmlAppendIntegrations(h);
  htmlAppendFirmware(h);
  htmlAppendFooter(h);
  server.send(200, "text/html", h);
}

// ============================================================
//  Form handlers (v6 semantics)
// ============================================================
static void handleSave() {
  if (!authed()) return;
  if (server.hasArg("lat")) {
    double v = server.arg("lat").toDouble();
    if (v >= -90 && v <= 90) g_set.homeLat = v;
  }
  if (server.hasArg("lon")) {
    double v = server.arg("lon").toDouble();
    if (v >= -180 && v <= 180) g_set.homeLon = v;
  }
  if (server.hasArg("feed")) {
    String v = server.arg("feed");
    v.trim();
    if (v.startsWith("http://") || v.startsWith("https://")) {
      g_set.feedUrl = v;
      g_prefs.putString("feed", g_set.feedUrl);
    }
  }
  if (server.hasArg("lbl")) g_set.showLabels = (server.arg("lbl") == "1");
  settingsSaveLocation();
  settingsSaveDisplay();
  resetTracks();
  feederUpdateSrcName();
  feederKick();
  mapRequestRefresh();
  enrichKickWeather();
  redirectHome();
}

static void handleWifi() {
  if (!authed()) return;
  String s = server.arg("ssid");
  s.trim();
  if (!s.length()) {
    server.send(400, "text/plain", "SSID required");
    return;
  }
  g_set.wifiSsid = s;
  g_set.wifiPass = server.arg("pass");
  g_prefs.putString("ssid", g_set.wifiSsid);
  g_prefs.putString("pass", g_set.wifiPass);
  webReboot("Joining " + htmlEscape(s) +
            " - rebooting. Reconnect at the IP shown on the display.");
}

static void handleNet() {
  if (!authed()) return;
  if (server.arg("mode") != "static") {
    bool was = g_set.netStatic;
    g_set.netStatic = false;
    g_prefs.putBool("nstat", false);
    if (was) {
      webReboot("Switching to DHCP - rebooting. Reconnect at the IP shown on the display.");
      return;
    }
    redirectHome();
    return;
  }
  String vip = server.arg("nip"), vgw = server.arg("ngw");
  String vmk = server.arg("nmask"), vdn = server.arg("ndns");
  vip.trim(); vgw.trim(); vmk.trim(); vdn.trim();
  IPAddress ip, gw, mk, dn;
  bool ok = ip.fromString(vip) && gw.fromString(vgw) && mk.fromString(vmk);
  if (ok && vdn.length()) ok = dn.fromString(vdn);
  if (!ok) {
    server.send(400, "text/plain", "Invalid address - go back and check the fields.");
    return;
  }
  g_set.netStatic = true;
  g_set.netIp = vip; g_set.netGw = vgw; g_set.netMask = vmk; g_set.netDns = vdn;
  g_prefs.putBool("nstat", true);
  g_prefs.putString("nip", g_set.netIp);
  g_prefs.putString("ngw", g_set.netGw);
  g_prefs.putString("nmask", g_set.netMask);
  g_prefs.putString("ndns", g_set.netDns);
  webReboot("Applying network settings - reconnect at http://" + htmlEscape(vip) + "/");
}

static void handleForget() {
  if (!authed()) return;
  server.send(200, "text/plain", "Forgetting WiFi, rebooting...");
  g_prefs.remove("ssid");
  g_prefs.remove("pass");
  delay(kRebootDelayMs);
  ESP.restart();
}

static void handleIntegrations() {
  if (!authed()) return;
  String uri = server.arg("mqtturi");
  uri.trim();
  if (uri.length() && uri != "-" && !uri.startsWith("mqtt://")) {
    server.send(400, "text/plain", "MQTT URI must start with mqtt://");
    return;
  }
  String tz = server.arg("tz");
  tz.trim();
  if (tz.length() >= kTzMaxLen) {
    server.send(400, "text/plain", "Timezone string too long");
    return;
  }
  g_set.mqttEn = server.hasArg("mqtten");
  if (uri == "-") g_set.mqttUri = "";          // "-" clears
  else if (uri.length()) g_set.mqttUri = uri;  // blank keeps (never echoed back)
  if (tz.length()) g_set.tz = tz;
  String pp = server.arg("ppass");
  if (pp == "-") g_set.panelPass = "";         // "-" clears
  else if (pp.length()) g_set.panelPass = pp;  // blank keeps
  settingsSaveNetworkExtras();
  mqttRestart();
  configTzTime(g_set.tz.c_str(), kNtpServer1, kNtpServer2);
  redirectHome();
}

// ============================================================
//  JSON API
// ============================================================
static void jsonFillFlight(JsonObject o, const Track& t) {
  o["hex"] = t.hex;
  o["flight"] = t.flight;
  o["reg"] = t.reg;
  o["type"] = t.typeCode;
  o["op"] = t.ownOp;
  o["lat"] = t.lat;
  o["lon"] = t.lon;
  o["alt_ft"] = t.altFt;
  o["gs_kt"] = t.gsKt;
  o["track_deg"] = t.trackDeg;
  o["vs_fpm"] = t.vRateFpm;
  o["squawk"] = t.squawk;
  o["mil"] = t.mil;
  o["origin"] = t.origin;
  o["dest"] = t.dest;
  o["dist_km"] = haversineKm(g_set.homeLat, g_set.homeLon, t.lat, t.lon);
}

static void handleApiState() {
  if (!authed()) return;
  DynamicJsonDocument doc(kStateDocBytes);
  doc["version"] = AR_VERSION;
  doc["uptime_s"] = (uint32_t)(millis() / 1000UL);
  doc["source"] = !g_wifiUp ? "offline" : (g_feedIsLocal ? "local" : "cloud");
  doc["src_name"] = g_localSrcName;
  doc["in_range"] = g_orderN;
  doc["heard"] = g_heardCount;
  doc["msg_rate"] = g_feedMsgRate;
  doc["lat"] = g_set.homeLat;
  doc["lon"] = g_set.homeLon;
  doc["range_km"] = g_set.rangeKm;
  WeatherState wx; IssState iss;                 // copy under the data mutex
  portENTER_CRITICAL(&g_dataMux);
  wx = g_wx; iss = g_iss;
  portEXIT_CRITICAL(&g_dataMux);
  JsonObject jwx = doc.createNestedObject("wx");
  jwx["valid"] = wx.valid;
  jwx["temp_c"] = wx.tempC;
  jwx["wind_kmh"] = wx.windKmh;
  jwx["wind_dir"] = wx.windDirDeg;
  jwx["code"] = wx.wmoCode;
  jwx["word"] = wxWordFor(wx.wmoCode);
  JsonObject jiss = doc.createNestedObject("iss");
  jiss["valid"] = iss.valid;
  jiss["lat"] = iss.lat;
  jiss["lon"] = iss.lon;
  Track* near = tracksNearest();
  if (near) {
    JsonObject jn = doc.createNestedObject("nearest");
    jn["hex"] = near->hex;
    jn["flight"] = near->flight;
    jn["dist_km"] = haversineKm(g_set.homeLat, g_set.homeLon, near->lat, near->lon);
    jn["bearing_deg"] = bearingTo(g_set.homeLat, g_set.homeLon, near->lat, near->lon);
  }
  Track* sel = tracksSelected();
  if (sel) jsonFillFlight(doc.createNestedObject("selected"), *sel);
  JsonArray fl = doc.createNestedArray("flights");
  for (int i = 0; i < g_orderN; i++)
    jsonFillFlight(fl.createNestedObject(), g_tracks[g_orderIdx[i]]);
  String out;
  out.reserve(kStateOutReserve);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiConfigGet() {
  if (!authed()) return;
  DynamicJsonDocument doc(kCfgDocBytes);
  doc["version"] = AR_VERSION;
  doc["ssid"] = g_set.wifiSsid;                  // pass intentionally omitted
  doc["lat"] = g_set.homeLat;
  doc["lon"] = g_set.homeLon;
  doc["rng"] = g_set.rangeKm;
  doc["lbl"] = g_set.showLabels;
  doc["feed"] = g_set.feedUrl;
  doc["tz"] = g_set.tz;
  doc["nstat"] = g_set.netStatic;
  doc["nip"] = g_set.netIp;
  doc["ngw"] = g_set.netGw;
  doc["nmask"] = g_set.netMask;
  doc["ndns"] = g_set.netDns;
  doc["mqtten"] = g_set.mqttEn;
  doc["mqtturi"] = mqttUriRedacted(g_set.mqttUri); // credentials stripped
  doc["nighten"] = g_set.nightEn;
  doc["nightfr"] = g_set.nightFromMin;
  doc["nightto"] = g_set.nightToMin;
  doc["wxen"] = g_set.wxEn;
  doc["issen"] = g_set.issEn;
  doc["logoen"] = g_set.logoEn;
  doc["mapen"] = g_set.mapEn;
  doc["fcls"] = g_set.filtCls;
  doc["faltlo"] = g_set.filtAltLo;
  doc["falthi"] = g_set.filtAltHi;
  doc["watch"] = g_set.watchlist;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Validate every present arg BEFORE applying anything (no partial updates).
static bool apiCfgValidate(String& err) {
  if (server.hasArg("lat")) {
    double v = server.arg("lat").toDouble();
    if (isnan(v) || v < -90 || v > 90) { err = "lat out of range"; return false; }
  }
  if (server.hasArg("lon")) {
    double v = server.arg("lon").toDouble();
    if (isnan(v) || v < -180 || v > 180) { err = "lon out of range"; return false; }
  }
  if (server.hasArg("feed")) {
    String v = server.arg("feed"); v.trim();
    if (!v.startsWith("http://") && !v.startsWith("https://")) {
      err = "feed must start with http(s)://"; return false;
    }
  }
  if (server.hasArg("rng") && !rangeStepValid(server.arg("rng").toInt())) {
    err = "rng not a valid step"; return false;
  }
  if (server.hasArg("tz")) {
    String v = server.arg("tz"); v.trim();
    if (!v.length() || v.length() >= kTzMaxLen) { err = "tz invalid"; return false; }
  }
  if (server.hasArg("mqtturi")) {
    String v = server.arg("mqtturi"); v.trim();
    if (v.length() && !v.startsWith("mqtt://")) {
      err = "mqtturi must start with mqtt://"; return false;
    }
  }
  if (server.hasArg("fcls")) {
    int v = server.arg("fcls").toInt();
    if (v < 1 || v > AR_FILT_CLS_ALL) { err = "fcls out of range"; return false; }
  }
  int lo = server.hasArg("faltlo") ? server.arg("faltlo").toInt() : g_set.filtAltLo;
  int hi = server.hasArg("falthi") ? server.arg("falthi").toInt() : g_set.filtAltHi;
  if (lo < 0 || lo > kAltFilterMaxFt || hi < 0 || hi > kAltFilterMaxFt) {
    err = "alt filter out of range"; return false;
  }
  if (lo > 0 && hi > 0 && lo > hi) { err = "faltlo above falthi"; return false; }
  if (server.hasArg("watch") && server.arg("watch").length() > AR_WATCHLIST_LEN) {
    err = "watch too long"; return false;
  }
  return true;
}

// Apply pre-validated args, persist via settingsSave* and fire side-effect kicks.
static void apiCfgApply() {
  bool loc = false, disp = false, filt = false, extras = false;
  bool feedCh = false, mqttCh = false, tzCh = false;
  if (server.hasArg("lat")) { g_set.homeLat = server.arg("lat").toDouble(); loc = true; }
  if (server.hasArg("lon")) { g_set.homeLon = server.arg("lon").toDouble(); loc = true; }
  if (server.hasArg("rng")) { g_set.rangeKm = server.arg("rng").toInt(); loc = true; }
  if (server.hasArg("feed")) {
    String v = server.arg("feed"); v.trim();
    g_set.feedUrl = v;
    g_prefs.putString("feed", g_set.feedUrl);
    feedCh = true;
  }
  if (server.hasArg("lbl"))    { g_set.showLabels = argBool(server.arg("lbl"));  disp = true; }
  if (server.hasArg("wxen"))   { g_set.wxEn   = argBool(server.arg("wxen"));   disp = true; }
  if (server.hasArg("issen"))  { g_set.issEn  = argBool(server.arg("issen"));  disp = true; }
  if (server.hasArg("logoen")) { g_set.logoEn = argBool(server.arg("logoen")); disp = true; }
  if (server.hasArg("mapen"))  { g_set.mapEn  = argBool(server.arg("mapen"));  disp = true; }
  if (server.hasArg("tz")) {
    String v = server.arg("tz"); v.trim();
    g_set.tz = v; extras = true; tzCh = true;
  }
  if (server.hasArg("mqtten")) { g_set.mqttEn = argBool(server.arg("mqtten")); extras = true; mqttCh = true; }
  if (server.hasArg("mqtturi")) {
    String v = server.arg("mqtturi"); v.trim();
    g_set.mqttUri = v; extras = true; mqttCh = true;
  }
  if (server.hasArg("fcls"))   { g_set.filtCls = (uint8_t)server.arg("fcls").toInt(); filt = true; }
  if (server.hasArg("faltlo")) { g_set.filtAltLo = server.arg("faltlo").toInt(); filt = true; }
  if (server.hasArg("falthi")) { g_set.filtAltHi = server.arg("falthi").toInt(); filt = true; }
  if (server.hasArg("watch"))  { g_set.watchlist = server.arg("watch"); filt = true; }
  if (loc)    settingsSaveLocation();
  if (disp)   settingsSaveDisplay();
  if (filt)   settingsSaveFilters();
  if (extras) settingsSaveNetworkExtras();
  if (feedCh) feederUpdateSrcName();
  if (loc || feedCh) { resetTracks(); feederKick(); }
  if (loc)    { mapRequestRefresh(); enrichKickWeather(); }
  if (mqttCh) mqttRestart();
  if (tzCh)   configTzTime(g_set.tz.c_str(), kNtpServer1, kNtpServer2);
}

static void handleApiConfigPost() {
  if (!authed()) return;
  String err;
  if (!apiCfgValidate(err)) {
    Serial.printf("[web] /api/config rejected: %s\n", err.c_str());
    server.send(400, "application/json",
                String(F("{\"ok\":false,\"err\":\"")) + err + F("\"}"));
    return;
  }
  apiCfgApply();
  server.send(200, "application/json", F("{\"ok\":true}"));
}

// ============================================================
//  /screen.bmp — live framebuffer as a 24-bit BMP stream
// ============================================================
static void wrLE32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wrLE16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void buildBmpHeader(uint8_t* h, uint32_t imgBytes) {
  memset(h, 0, kBmpHeaderBytes);
  h[0] = 'B'; h[1] = 'M';
  wrLE32(h + 2, kBmpHeaderBytes + imgBytes);   // file size
  wrLE32(h + 10, kBmpHeaderBytes);             // pixel data offset
  wrLE32(h + 14, kBmpDibBytes);                // BITMAPINFOHEADER size
  wrLE32(h + 18, SCR_W);
  wrLE32(h + 22, SCR_H);                       // positive height = bottom-up
  wrLE16(h + 26, 1);                           // planes
  wrLE16(h + 28, kBmpBpp);
  wrLE32(h + 34, imgBytes);                    // raw image size
}

static void handleScreenBmp() {
  if (!authed()) return;
  const uint32_t imgBytes = (uint32_t)kBmpRowBytes * SCR_H;
  uint8_t hdr[kBmpHeaderBytes];
  buildBmpHeader(hdr, imgBytes);
  server.setContentLength(kBmpHeaderBytes + imgBytes);
  server.send(200, "image/bmp", "");
  WiFiClient c = server.client();
  c.write(hdr, kBmpHeaderBytes);
  uint16_t row[SCR_W];
  uint8_t rowbuf[kBmpRowBytes];
  for (int y = SCR_H - 1; y >= 0; y--) {       // BMP rows run bottom-up
    if (!c.connected()) {
      Serial.println("[web] screen.bmp client dropped - aborting");
      return;
    }
    halReadRect(0, y, SCR_W, 1, row);
    for (int x = 0; x < SCR_W; x++) {
      uint16_t px = row[x];
      uint8_t r5 = (px >> 11) & 0x1F;
      uint8_t g6 = (px >> 5) & 0x3F;
      uint8_t b5 = px & 0x1F;
      rowbuf[x * 3 + 0] = (uint8_t)((b5 << 3) | (b5 >> 2));
      rowbuf[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
      rowbuf[x * 3 + 2] = (uint8_t)((r5 << 3) | (r5 >> 2));
    }
    c.write(rowbuf, kBmpRowBytes);
  }
}

// ============================================================
//  /metrics — Prometheus text exposition
// ============================================================
// GET /api/probe?url=http://x/y — the device fetches the URL itself and
// reports what IT saw. Settles "is it my firewall or the firmware?" forever.
// Plain-HTTP only (LAN diagnostics); blocking (~4s max) is fine for a manual
// support call. Auth-guarded like everything else.
static void handleApiProbe() {
  if (!authed()) return;
  String url = server.arg("url");
  url.trim();
  if (!url.startsWith("http://")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"err\":\"http:// urls only\"}");
    return;
  }
  WiFiClient net;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  http.useHTTP10(true);
  uint32_t t0 = millis();
  int code = -100;                     // begin() refused the URL
  int len = -1;
  if (http.begin(net, url)) {
    code = http.GET();                 // negative = HTTPClient error code
    if (code > 0) len = http.getSize();
    http.end();
  }
  char out[128];
  snprintf(out, sizeof(out), "{\"ok\":true,\"code\":%d,\"ms\":%lu,\"len\":%d}",
           code, (unsigned long)(millis() - t0), len);
  server.send(200, "application/json", out);
}

static void handleMetrics() {
  if (!authed()) return;
  String s;
  s.reserve(896);
  char l[96];
  s += F("# TYPE airradar_in_range gauge\n");
  snprintf(l, sizeof(l), "airradar_in_range %d\n", g_orderN); s += l;
  s += F("# TYPE airradar_heard gauge\n");
  snprintf(l, sizeof(l), "airradar_heard %d\n", g_heardCount); s += l;
  s += F("# TYPE airradar_msg_rate gauge\n");
  snprintf(l, sizeof(l), "airradar_msg_rate %.2f\n", (double)g_feedMsgRate); s += l;
  s += F("# TYPE airradar_feed_local gauge\n");
  snprintf(l, sizeof(l), "airradar_feed_local %d\n", g_feedIsLocal ? 1 : 0); s += l;
  s += F("# TYPE airradar_wifi_rssi gauge\n");
  snprintf(l, sizeof(l), "airradar_wifi_rssi %d\n", g_wifiUp ? (int)WiFi.RSSI() : 0); s += l;
  s += F("# TYPE airradar_heap_free gauge\n");
  snprintf(l, sizeof(l), "airradar_heap_free %u\n", (unsigned)ESP.getFreeHeap()); s += l;
  s += F("# TYPE airradar_psram_free gauge\n");
  snprintf(l, sizeof(l), "airradar_psram_free %u\n", (unsigned)ESP.getFreePsram()); s += l;
  // Leak-vs-fragmentation forensics: min-ever free + largest single block.
  s += F("# TYPE airradar_heap_min gauge\n");
  snprintf(l, sizeof(l), "airradar_heap_min %u\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)); s += l;
  s += F("# TYPE airradar_heap_largest gauge\n");
  snprintf(l, sizeof(l), "airradar_heap_largest %u\n",
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)); s += l;
  // Rising tls_shed = optional fetches refused for want of internal RAM, i.e.
  // blank routes/weather are a memory problem, not a data problem.
  s += F("# TYPE airradar_tls_shed counter\n");
  snprintf(l, sizeof(l), "airradar_tls_shed %lu\n",
           (unsigned long)g_tlsShedCount); s += l;
  s += F("# TYPE airradar_tls_conn counter\n");
  snprintf(l, sizeof(l), "airradar_tls_conn %lu\n",
           (unsigned long)g_tlsConnCount); s += l;
  s += F("# TYPE airradar_uptime_seconds gauge\n");
  snprintf(l, sizeof(l), "airradar_uptime_seconds %lu\n",
           (unsigned long)(millis() / 1000UL)); s += l;
  server.send(200, "text/plain", s);
}

// ============================================================
//  /update — OTA firmware upload (Update.h, app slot)
// ============================================================
static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!authed()) return;
    Serial.printf("[web] OTA start: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Serial.printf("[web] OTA begin failed: %s\n", Update.errorString());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Serial.printf("[web] OTA write failed: %s\n", Update.errorString());
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      Serial.printf("[web] OTA done: %u bytes\n", (unsigned)up.totalSize);
    else
      Serial.printf("[web] OTA end failed: %s\n", Update.errorString());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[web] OTA aborted by client");
  }
}

static void handleUpdateDone() {
  if (!authed()) return;
  if (Update.hasError()) {
    server.send(500, "text/plain",
                String(F("Update failed: ")) + Update.errorString());
    return;
  }
  server.send(200, "text/plain", "OK, rebooting");
  delay(kRebootDelayMs);
  ESP.restart();
}

// ============================================================
//  Lifecycle
// ============================================================
static void handleNotFound() {
  if (!authed()) return;
  server.send(404, "text/plain", "Not found");
}

void webBegin() {
  if (s_serverUp) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/net", HTTP_POST, handleNet);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/integrations", HTTP_POST, handleIntegrations);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/config", HTTP_GET, handleApiConfigGet);
  server.on("/api/config", HTTP_POST, handleApiConfigPost);
  server.on("/screen.bmp", HTTP_GET, handleScreenBmp);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.on("/api/probe", HTTP_GET, handleApiProbe);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  // WebServer only retains headers we explicitly collect (for the CSRF guard).
  static const char* kHdrs[] = {"Origin", "Host"};
  server.collectHeaders(kHdrs, 2);
  if (MDNS.begin(AR_MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  else Serial.println("[web] mDNS start failed");
  server.begin();
  s_serverUp = true;
  Serial.printf("[web] up at http://%s/ (http://%s.local/)\n",
                WiFi.localIP().toString().c_str(), AR_MDNS_NAME);
  if (!g_set.panelPass.length())
    Serial.println("[web] WARNING: no panel password set - web config + OTA unprotected");
}

void webLoop() {
  if (s_serverUp) server.handleClient();
}
