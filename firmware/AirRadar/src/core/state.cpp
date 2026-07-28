// state.cpp — shared state storage, settings persistence, core math.
#include <math.h>
#include "state.h"

// ---------- live state ----------
Track    g_tracks[AR_MAX_TRACKS];
char     g_selHex[8] = "";
int      g_orderIdx[AR_MAX_TRACKS];
int      g_orderN = 0;
int      g_heardCount = 0;
bool     g_feedIsLocal = true;
char     g_localSrcName[AR_LOCAL_SRC_NAME_MAX] = "LOCAL";
uint32_t g_lastGoodApply = 0;
float    g_feedMsgRate = -1.0f;
WeatherState g_wx = {};
IssState     g_iss = {};

Settings    g_set;
Preferences g_prefs;

// ---------- pending ----------
portMUX_TYPE g_dataMux = portMUX_INITIALIZER_UNLOCKED;
ApiPlane g_pendingPlanes[AR_MAX_TRACKS];
int      g_pendingCount = 0;
int      g_pendingHeard = 0;
bool     g_pendingLocal = true;
volatile bool g_pendingReady = false;
volatile bool g_pendingOk = false;

char g_routeResHex[8] = "";
char g_routeResOrigin[5] = "", g_routeResDest[5] = "";
volatile bool g_routeResReady = false;

volatile bool g_fetchInProgress = false;
volatile bool g_routeFetching = false;
Screen g_screen = SCR_MAIN;
bool   g_wifiUp = false;
bool   g_timeSynced = false;

// ============================================================
//  Settings persistence
// ============================================================
void settingsLoad() {
  g_prefs.begin(AR_NVS_NS, false);
  g_set.homeLat   = g_prefs.getDouble("lat", AR_DEFAULT_LAT);
  g_set.homeLon   = g_prefs.getDouble("lon", AR_DEFAULT_LON);
  g_set.rangeKm   = g_prefs.getInt("rng", AR_RANGE_DEFAULT);
  g_set.showLabels= g_prefs.getBool("lbl", true);
  g_set.wifiSsid  = g_prefs.getString("ssid", "");
  g_set.wifiPass  = g_prefs.getString("pass", "");
  g_set.feedUrl   = g_prefs.getString("feed", AR_DEFAULT_FEED);
  g_set.tz        = g_prefs.getString(K_TZ, AR_DEFAULT_TZ);
  g_set.panelPass = g_prefs.getString(K_PPASS, "");
  g_set.netStatic = g_prefs.getBool("nstat", false);
  g_set.netIp     = g_prefs.getString("nip", "");
  g_set.netGw     = g_prefs.getString("ngw", "");
  g_set.netMask   = g_prefs.getString("nmask", "255.255.255.0");
  g_set.netDns    = g_prefs.getString("ndns", "");
  g_set.mqttEn    = g_prefs.getBool(K_MQTT_EN, false);
  g_set.mqttUri   = g_prefs.getString(K_MQTT_URI, "");
  g_set.nightEn   = g_prefs.getBool(K_NIGHT_EN, false);
  g_set.nightFromMin = g_prefs.getInt(K_NIGHT_FROM, 23 * 60);
  g_set.nightToMin   = g_prefs.getInt(K_NIGHT_TO, 6 * 60);
  g_set.wxEn      = g_prefs.getBool(K_WX_EN, true);
  g_set.issEn     = g_prefs.getBool(K_ISS_EN, true);
  g_set.logoEn    = g_prefs.getBool(K_LOGO_EN, true);
  g_set.mapEn     = g_prefs.getBool(K_MAP_EN, true);
  g_set.filtCls   = (uint8_t)g_prefs.getInt(K_FILT_CLS, AR_FILT_CLS_ALL);
  g_set.filtAltLo = g_prefs.getInt(K_FILT_ALT_LO, 0);
  g_set.filtAltHi = g_prefs.getInt(K_FILT_ALT_HI, 0);
  g_set.watchlist = g_prefs.getString(K_WATCH, "");
  for (int i = 0; i < AR_MAX_FAVS; i++) {
    char k[12];
    snprintf(k, sizeof(k), "%s%dlat", K_FAV_BASE, i);
    g_set.favLat[i] = g_prefs.getDouble(k, 0.0);
    snprintf(k, sizeof(k), "%s%dlon", K_FAV_BASE, i);
    g_set.favLon[i] = g_prefs.getDouble(k, 0.0);
    snprintf(k, sizeof(k), "%s%dnam", K_FAV_BASE, i);
    g_set.favName[i] = g_prefs.getString(k, "");
    g_set.favUsed[i] = g_set.favName[i].length() > 0;
  }
  // sanity clamps — never trust stored values blindly
  if (g_set.homeLat < -90 || g_set.homeLat > 90)   g_set.homeLat = AR_DEFAULT_LAT;
  if (g_set.homeLon < -180 || g_set.homeLon > 180) g_set.homeLon = AR_DEFAULT_LON;
  static const int steps[] = AR_RANGE_STEPS;
  bool okRange = false;
  for (int s : steps) if (g_set.rangeKm == s) okRange = true;
  if (!okRange) g_set.rangeKm = AR_RANGE_DEFAULT;
}

void settingsSaveLocation() {
  g_prefs.putDouble("lat", g_set.homeLat);
  g_prefs.putDouble("lon", g_set.homeLon);
  g_prefs.putInt("rng", g_set.rangeKm);
}
void settingsSaveDisplay() {
  g_prefs.putBool("lbl", g_set.showLabels);
  g_prefs.putBool(K_NIGHT_EN, g_set.nightEn);
  g_prefs.putInt(K_NIGHT_FROM, g_set.nightFromMin);
  g_prefs.putInt(K_NIGHT_TO, g_set.nightToMin);
  g_prefs.putBool(K_WX_EN, g_set.wxEn);
  g_prefs.putBool(K_ISS_EN, g_set.issEn);
  g_prefs.putBool(K_LOGO_EN, g_set.logoEn);
  g_prefs.putBool(K_MAP_EN, g_set.mapEn);
}
void settingsSaveFilters() {
  g_prefs.putInt(K_FILT_CLS, g_set.filtCls);
  g_prefs.putInt(K_FILT_ALT_LO, g_set.filtAltLo);
  g_prefs.putInt(K_FILT_ALT_HI, g_set.filtAltHi);
  g_prefs.putString(K_WATCH, g_set.watchlist);
}
void settingsSaveNetworkExtras() {
  g_prefs.putBool(K_MQTT_EN, g_set.mqttEn);
  g_prefs.putString(K_MQTT_URI, g_set.mqttUri);
  g_prefs.putString(K_PPASS, g_set.panelPass);
  g_prefs.putString(K_TZ, g_set.tz);
}
void settingsSaveFavs() {
  for (int i = 0; i < AR_MAX_FAVS; i++) {
    char k[12];
    snprintf(k, sizeof(k), "%s%dlat", K_FAV_BASE, i);
    g_prefs.putDouble(k, g_set.favLat[i]);
    snprintf(k, sizeof(k), "%s%dlon", K_FAV_BASE, i);
    g_prefs.putDouble(k, g_set.favLon[i]);
    snprintf(k, sizeof(k), "%s%dnam", K_FAV_BASE, i);
    g_prefs.putString(k, g_set.favUsed[i] ? g_set.favName[i] : "");
  }
}

// ============================================================
//  TLS gate (see state.h)
// ============================================================
static volatile bool s_tlsBusy = false;

bool tlsTryAcquire(bool essential) {
  if (!essential &&
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < AR_TLS_HEAP_FLOOR)
    return false;                      // shed eye-candy before the feed starves
  bool got = false;
  portENTER_CRITICAL(&g_dataMux);
  if (!s_tlsBusy) { s_tlsBusy = true; got = true; }
  portEXIT_CRITICAL(&g_dataMux);
  return got;
}

void tlsRelease() {
  portENTER_CRITICAL(&g_dataMux);
  s_tlsBusy = false;
  portEXIT_CRITICAL(&g_dataMux);
}

// ============================================================
//  Math
// ============================================================
static inline double d2r(double d) { return d * M_PI / 180.0; }

float haversineKm(double la1, double lo1, double la2, double lo2) {
  double dLa = d2r(la2 - la1), dLo = d2r(lo2 - lo1);
  double a = sin(dLa / 2) * sin(dLa / 2) +
             cos(d2r(la1)) * cos(d2r(la2)) * sin(dLo / 2) * sin(dLo / 2);
  return (float)(6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a)));
}

float bearingTo(double la1, double lo1, double la2, double lo2) {
  double y = sin(d2r(lo2 - lo1)) * cos(d2r(la2));
  double x = cos(d2r(la1)) * sin(d2r(la2)) -
             sin(d2r(la1)) * cos(d2r(la2)) * cos(d2r(lo2 - lo1));
  double b = atan2(y, x) * 180.0 / M_PI;
  if (b < 0) b += 360;
  return (float)b;
}

bool scopeToScreen(double lat, double lon, float& sx, float& sy) {
  float dKm = haversineKm(g_set.homeLat, g_set.homeLon, lat, lon);
  if (dKm > (float)g_set.rangeKm) return false;
  float brg = bearingTo(g_set.homeLat, g_set.homeLon, lat, lon);
  float r = (dKm / (float)g_set.rangeKm) * SCOPE_R;
  float rad = d2r(brg);
  sx = SCOPE_CX + sinf(rad) * r;
  sy = SCOPE_CY - cosf(rad) * r;
  return true;
}

// ============================================================
//  Classification / filters / colors
// ============================================================
uint8_t trackClass(const Track& t) {
  if (t.mil) return FCLS_MIL;
  const char* c = t.category;
  if (c[0] == 'A') {
    if (c[1] == '1' || c[1] == '2') return FCLS_LIGHT;
    if (c[1] == '3' || c[1] == '4' || c[1] == '5' || c[1] == '6') return FCLS_AIRLINER;
    if (c[1] == '7') return FCLS_HELI;
  }
  return FCLS_OTHER;
}

bool trackPassesFilters(const Track& t) {
  if (!(trackClass(t) & g_set.filtCls)) return false;
  if (g_set.filtAltLo > 0 && t.altFt >= 0 && t.altFt < g_set.filtAltLo) return false;
  if (g_set.filtAltHi > 0 && t.altFt >= 0 && t.altFt > g_set.filtAltHi) return false;
  return true;
}

bool trackOnWatchlist(const Track& t) {
  if (!g_set.watchlist.length()) return false;
  // comma-separated prefixes matched against reg and callsign (case-insensitive)
  String wl = g_set.watchlist;
  wl.toUpperCase();
  String reg(t.reg), fl(t.flight);
  reg.toUpperCase(); reg.trim();
  fl.toUpperCase();  fl.trim();
  int start = 0;
  while (start < (int)wl.length()) {
    int comma = wl.indexOf(',', start);
    if (comma < 0) comma = wl.length();
    String p = wl.substring(start, comma);
    p.trim();
    if (p.length()) {
      if (reg.length() && reg.startsWith(p)) return true;
      if (fl.length() && fl.startsWith(p)) return true;
    }
    start = comma + 1;
  }
  return false;
}

void altColorRGB(int altFt, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (altFt >= 30000)      { r = 0xa9; g = 0x8c; b = 0xff; }   // violet
  else if (altFt >= 10000) { r = 0x54; g = 0xdc; b = 0xee; }   // cyan
  else if (altFt >= 0)     { r = 0xf6; g = 0xb2; b = 0x4a; }   // amber
  else                     { r = 0xff; g = 0x64; b = 0x72; }   // unknown -> red-ish
}
