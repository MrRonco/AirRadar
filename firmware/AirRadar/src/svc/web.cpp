// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
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
static const size_t   kRootPageReserve = 12288;  // root page String pre-alloc
// >4 KB so it lands in PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096), which
// keeps the console's markup off the internal heap mbedTLS needs.
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
    // Exact match only. indexOf() accepted any Origin merely CONTAINING the
    // host, so http://airradar.local.evil.com passed against airradar.local —
    // and with no panel password set this guard is the only thing protecting
    // /forget and /update.
    bool sameOrigin = o.equalsIgnoreCase("http://" + hostHdr) ||
                      o.equalsIgnoreCase("https://" + hostHdr);
    if (o.length() && hostHdr.length() && !sameOrigin) {
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
  // Desktop-only management console: no viewport meta, no mobile column.
  // System font stacks only — the device may have no internet, so web fonts
  // are not an option. Palette mirrors the panel tokens (theme.h).
  h += F("<!doctype html><html lang=en><head><meta charset=utf-8>"
         "<title>AirRadar</title><style>"
         "*{box-sizing:border-box}"
         "body{font:15px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif;"
         "background:#05080d;color:#eef1f4;margin:0;padding:22px 20px 56px}"
         ".w{max-width:1240px;margin:0 auto}"
         ".hd{display:flex;align-items:center;gap:14px;margin-bottom:14px}"
         ".hd h1{font-size:19px;margin:0;font-weight:600;letter-spacing:-.01em}"
         ".hd .r{margin-left:auto;font:13px ui-monospace,SFMono-Regular,Menlo,monospace;"
         "color:#8e9baa}"
         ".g{display:grid;gap:12px;margin-bottom:12px;align-items:start}"
         ".c8{grid-template-columns:repeat(8,1fr)}"
         ".c3{grid-template-columns:repeat(3,1fr)}"
         ".c21{grid-template-columns:2fr 1fr}"
         ".b{background:#182231;border:1px solid rgba(180,205,230,.16);"
         "border-radius:13px;padding:14px}"
         ".t{font:500 11px/1 ui-monospace,SFMono-Regular,Menlo,monospace;letter-spacing:.09em;"
         "text-transform:uppercase;color:#8e9baa;margin:0 0 10px}"
         ".tile{background:#0d1420;border:1px solid rgba(180,205,230,.10);"
         "border-radius:8px;padding:9px 11px}"
         ".tk{font:500 10px/1 ui-monospace,SFMono-Regular,Menlo,monospace;letter-spacing:.08em;"
         "color:#75828f;text-transform:uppercase}"
         ".tv{font:500 22px/1.35 system-ui;font-variant-numeric:tabular-nums}"
         "label{display:block;font:500 11px/1.7 ui-monospace,SFMono-Regular,Menlo,monospace;"
         "color:#8e9baa;margin-top:9px}"
         "input,select{width:100%;padding:8px 9px;background:#0d1420;color:#eef1f4;"
         "border:1px solid #26313f;border-radius:6px;"
         "font:13px ui-monospace,SFMono-Regular,Menlo,monospace}"
         "input[type=checkbox]{width:auto;margin:0 7px 0 0}input[type=file]{padding:6px}"
         "button{padding:9px 15px;background:#54dcee;color:#05080d;border:0;border-radius:7px;"
         "font:600 13px system-ui;cursor:pointer;margin-top:11px}"
         "button.d{background:#ff6472;color:#fff}"
         "table{width:100%;border-collapse:collapse;"
         "font:13px ui-monospace,SFMono-Regular,Menlo,monospace}"
         "th{text-align:left;font:500 10px/1 ui-monospace,monospace;letter-spacing:.07em;"
         "text-transform:uppercase;color:#75828f;padding-bottom:7px}"
         "td{padding:5px 0;color:#aab4c0;border-top:1px solid rgba(180,205,230,.08);"
         "white-space:nowrap}"
         ".dz{border-color:rgba(255,100,114,.45)}.dz .t{color:#ff8a94}"
         ".n{color:#75828f;font-size:12px}a{color:#8e9baa}"
         ".mir{width:100%;display:block;border-radius:6px;border:1px solid #1a222e;"
         "background:#05080d}"
         ".pill{display:inline-block;width:7px;height:7px;border-radius:50%;"
         "background:#54dcee;margin-right:6px;vertical-align:middle}"
         "</style></head><body><div class=w>");
}

// Header + the eight-tile status strip. Values are filled by JS from /metrics;
// the markup ships with em-dashes so the page never renders a misleading zero.
static void htmlAppendStatus(String& h) {
  h += F("<div class=hd><h1>&#9992; AirRadar</h1>"
         "<span class=n id=host></span>"
         "<span class=r><span class=pill></span><span id=src>&mdash;</span>"
         " &nbsp; up <span id=up>&mdash;</span> &nbsp; " AR_VERSION "</span></div>"
         "<div class='g c8'>"
         "<div class=tile><div class=tk>in range</div><div class=tv id=mR>&mdash;</div></div>"
         "<div class=tile><div class=tk>heard</div><div class=tv id=mH>&mdash;</div></div>"
         "<div class=tile><div class=tk>msg rate</div><div class=tv id=mM>&mdash;</div></div>"
         "<div class=tile><div class=tk>source</div><div class=tv id=mS>&mdash;</div></div>"
         "<div class=tile><div class=tk>rssi</div><div class=tv id=mW>&mdash;</div></div>"
         "<div class=tile><div class=tk>heap free</div><div class=tv id=mF>&mdash;</div></div>"
         "<div class=tile><div class=tk>heap min</div><div class=tv id=mN>&mdash;</div></div>"
         "<div class=tile><div class=tk>tls shed</div><div class=tv id=mT>&mdash;</div></div>"
         "</div>");
}

static void htmlAppendLive(String& h) {
  h += F("<div class='g c21'>"
         "<div class=b><p class=t>Traffic</p>"
         "<table><thead><tr><th>callsign</th><th>type</th><th>operator</th>"
         "<th>route</th><th>alt</th><th>dist</th></tr></thead>"
         "<tbody id=tb><tr><td colspan=6 class=n>loading&hellip;</td></tr></tbody></table></div>"
         "<div class=b><p class=t>Panel mirror</p>"
         "<img class=mir id=mir alt='live panel' style=display:none>"
         "<p class=n id=mirh>Not loaded. The panel is only mirrored on demand.</p>"
         "<button type=button onclick=\"var m=$('mir');m.style.display='block';"
         "$('mirh').style.display='none';m.src='/screen.bmp?t='+Date.now()\">Refresh</button>"
         "<p class=n>Manual only &mdash; 1.1&nbsp;MB and it blocks the panel for ~2&nbsp;s.</p>"
         "</div></div>");
}

static void htmlAppendSettings(String& h) {
  h += F("<div class='g c3'>"
         "<div class=b><p class=t>Radar &amp; feed</p><form method=post action=/save>"
         "<label>Latitude</label><input name=lat value='");
  h += String(g_set.homeLat, 6);
  h += F("'><label>Longitude</label><input name=lon value='");
  h += String(g_set.homeLon, 6);
  h += F("'><label>Feeder URL</label><input name=feed value='");
  h += htmlEscape(g_set.feedUrl);
  h += F("'><label>Target labels</label><select name=lbl><option value=1");
  if (g_set.showLabels) h += F(" selected");
  h += F(">On</option><option value=0");
  if (!g_set.showLabels) h += F(" selected");
  h += F(">Off</option></select><button type=submit>Save</button></form></div>");
}

static void htmlAppendNetwork(String& h, const String& pIp, const String& pGw,
                              const String& pMk, const String& pDn) {
  h += F("<div class=b><p class=t>Network</p>"
         "<form method=post action=/net onsubmit='return confirm(\"Apply network settings? "
         "The device reboots if the DHCP/static mode changes.\")'>"
         "<label>Mode</label><select name=mode><option value=dhcp");
  if (!g_set.netStatic) h += F(" selected");
  h += F(">DHCP</option><option value=static");
  if (g_set.netStatic) h += F(" selected");
  h += F(">Static</option></select><label>IP address</label><input name=nip value='");
  h += htmlEscape(pIp);
  h += F("'><label>Gateway</label><input name=ngw value='");
  h += htmlEscape(pGw);
  h += F("'><label>Subnet mask</label><input name=nmask value='");
  h += htmlEscape(pMk);
  h += F("'><label>DNS (blank = gateway)</label><input name=ndns value='");
  h += htmlEscape(pDn);
  h += F("'><button type=submit>Save network</button></form>"
         "<form method=post action=/wifi onsubmit='return confirm(\"Reboot and join this "
         "network? If the password is wrong you must re-enter it on the panel.\")'>"
         "<label>Wi-Fi SSID</label><input name=ssid value='");
  h += htmlEscape(g_set.wifiSsid);
  h += F("'><label>Wi-Fi password (blank = keep current)</label>"
         "<input type=password name=pass autocomplete=off>"
         "<button type=submit>Save &amp; reboot</button></form></div>");
}

static void htmlAppendIntegrations(String& h) {
  h += F("<div class=b><p class=t>Integrations &amp; firmware</p>"
         "<form method=post action=/integrations>"
         "<label><input type=checkbox name=mqtten value=1");
  if (g_set.mqttEn) h += F(" checked");
  h += F(">MQTT enabled</label>"
         "<label>MQTT URI (blank = keep, - = clear)</label><input name=mqtturi placeholder='");
  if (g_set.mqttUri.length()) h += htmlEscape(mqttUriRedacted(g_set.mqttUri));
  else h += F("mqtt://user:pass@host:1883");
  h += F("'><label>Timezone (POSIX TZ)</label><input name=tz value='");
  h += htmlEscape(g_set.tz);
  h += F("'><label>Panel password (blank = keep, - = clear)</label>"
         "<input type=password name=ppass autocomplete=off>"
         "<button type=submit>Save</button></form>"
         "<form method=post action=/update enctype='multipart/form-data' "
         "onsubmit='return confirm(\"Flash this firmware and reboot?\")'>"
         "<label>Firmware &mdash; running " AR_VERSION "</label>"
         "<input type=file name=fw accept='.bin'>"
         "<button type=submit>Upload &amp; flash</button></form></div></div>");
}

static void htmlAppendFooter(String& h) {
  h += F("<div class='g'><div class='b dz'><p class=t>&#9888; Danger zone</p>"
         "<form method=post action=/forget onsubmit='return confirm(\"Forget Wi-Fi? The device "
         "reboots and can then only be re-provisioned at the physical touchscreen.\")'>"
         "<span class=n>Clears the stored network. The device reboots and can only be "
         "re-provisioned at the panel.</span><br>"
         "<button class=d>Forget Wi-Fi</button></form></div></div>"
         "<p class=n>AirRadar " AR_VERSION " &middot; "
         "<a href='https://" AR_REPO_URL "'>" AR_REPO_URL "</a> &middot; " AR_AUTHOR_LINE
         "<br><a href='/api/state'>/api/state</a> &middot; "
         "<a href='/screen.bmp'>/screen.bmp</a> &middot; "
         "<a href='/metrics'>/metrics</a></p></div>");
  // Polling is deliberately gentle: the web server runs inside loop() next to
  // LVGL, and every request is a fresh TCP connection against a 16-slot PCB
  // pool. Hidden tabs stop polling entirely.
  h += F("<script>"
         "var $=function(i){return document.getElementById(i)};"
         "function P(t){var m={};t.split('\\n').forEach(function(l){"
         "if(l&&l[0]!='#'){var i=l.indexOf(' ');if(i>0)m[l.slice(0,i)]=+l.slice(i+1)}});return m}"
         "function U(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),"
         "n=Math.floor(s%3600/60);return (d?d+'d ':'')+('0'+h).slice(-2)+':'+('0'+n).slice(-2)}"
         "function K(b){return b>=1024?Math.round(b/1024)+'K':b}"
         "function M(){fetch('/metrics').then(function(r){return r.text()}).then(function(t){"
         "var m=P(t);"
         "$('mR').textContent=m.airradar_in_range;"
         "$('mH').textContent=m.airradar_heard;"
         "$('mM').textContent=Math.round(m.airradar_msg_rate||0)+'/s';"
         "var lo=m.airradar_feed_local==1;"
         "$('mS').textContent=lo?'LOCAL':'CLOUD';"
         "$('mS').style.color=lo?'#54dcee':'#8e9baa';$('src').textContent=lo?'LOCAL':'CLOUD';"
         "$('mW').textContent=m.airradar_wifi_rssi;"
         "$('mF').textContent=K(m.airradar_heap_free);"
         "$('mN').textContent=K(m.airradar_heap_min);"
         "var sh=m.airradar_tls_shed;$('mT').textContent=sh;"
         "$('mT').style.color=sh>0?'#ffc061':'#eef1f4';"
         "$('up').textContent=U(m.airradar_uptime_seconds);"
         "}).catch(function(){})}"
         "function T(){fetch('/api/state').then(function(r){return r.json()}).then(function(d){"
         "var b='',f=d.flights||[];"
         "if(!f.length){b='<tr><td colspan=6 class=n>no aircraft in range</td></tr>'}"
         "else for(var i=0;i<f.length;i++){var a=f[i];"
         "b+='<tr><td>'+(a.flight||'').trim()+'</td><td>'+(a.type||'')+'</td><td>'+"
         "((a.op||'').slice(0,22))+'</td><td>'+((a.origin&&a.dest)?a.origin+'&rarr;'+a.dest:"
         "'<span class=n>&mdash;</span>')+'</td><td>'+(a.alt_ft>=18000?'FL'+Math.round(a.alt_ft/100):"
         "(a.alt_ft>=0?a.alt_ft+' ft':'&mdash;'))+'</td><td>'+a.dist_km.toFixed(1)+' km</td></tr>'}"
         "$('tb').innerHTML=b}).catch(function(){})}"
         "$('host').textContent=location.host;"
         "var t1,t2;function GO(){M();T();t1=setInterval(M,10000);t2=setInterval(T,15000)}"
         "function STOP(){clearInterval(t1);clearInterval(t2)}"
         "document.addEventListener('visibilitychange',function(){"
         "document.hidden?STOP():GO()});GO();"
         "</script></body></html>");
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
  htmlAppendStatus(h);
  htmlAppendLive(h);
  htmlAppendSettings(h);
  htmlAppendNetwork(h, pIp, pGw, pMk, pDn);
  htmlAppendIntegrations(h);
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
  g_prefs.putString("ssid", g_set.wifiSsid);
  // Blank = keep, matching the convention this page already uses for the MQTT
  // URI and the panel password. The field renders empty on every load (a stored
  // secret must never be echoed into HTML), so writing it unconditionally meant
  // "fix a typo in the SSID and save" erased the password and rebooted into a
  // device that could only be re-provisioned at the physical touchscreen.
  String pw = server.arg("pass");
  if (pw.length()) {
    g_set.wifiPass = pw;
    g_prefs.putString("pass", g_set.wifiPass);
  }
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
  s += F("# TYPE airradar_heap_delta_feeder counter\n");
  snprintf(l, sizeof(l), "airradar_heap_delta_feeder %ld\n",
           (long)g_heapDeltaFeeder); s += l;
  snprintf(l, sizeof(l), "airradar_feeder_runs %lu\n",
           (unsigned long)g_feederRuns); s += l;
  s += F("# TYPE airradar_heap_delta_iss counter\n");
  snprintf(l, sizeof(l), "airradar_heap_delta_iss %ld\n",
           (long)g_heapDeltaIss); s += l;
  snprintf(l, sizeof(l), "airradar_iss_runs %lu\n",
           (unsigned long)g_issRuns); s += l;
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
