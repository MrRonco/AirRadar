// ui_settings.cpp — settings screen, Wi-Fi provisioning, coordinate editor,
// generic text editor, toast + async flows (settingsPoll).
#include <WiFi.h>
#include "ui.h"
#include "../core/tracks.h"
#include "../net/feeder.h"
#include "../net/enrich.h"
#include "../net/maptiles.h"
#include "../svc/web.h"
#include "../svc/mqtt.h"
#include "../hal/hal_display.h"

// ============================================================
//  File-local state
// ============================================================
static lv_obj_t* s_toast = nullptr;
static uint32_t  s_toastHideAt = 0;

// value labels / switches that settingsRefresh() re-reads
static lv_obj_t *s_vCoords, *s_vRange, *s_vNight, *s_vAlt, *s_vWatch,
                *s_vWifi, *s_vIpMode, *s_vFeed, *s_vPPass;
static lv_obj_t *s_swLabels, *s_swNight, *s_swWx, *s_swIss, *s_swLogo,
                *s_swMap, *s_swMqtt;
static lv_obj_t *s_favBtn[AR_MAX_FAVS];
static lv_obj_t *s_chip[4];   // AIRLINER LIGHT HELI MIL
static lv_obj_t *s_footIp;

// Wi-Fi flow
enum WifiFlow { WF_IDLE, WF_SCANNING, WF_CONNECTING };
static WifiFlow s_wifiFlow = WF_IDLE;
static uint32_t s_wifiT0 = 0;
static String   s_pendSsid, s_pendPass;
static lv_obj_t* s_wifiList = nullptr;     // container for scan results
static lv_obj_t* s_wifiMsg  = nullptr;     // "Scanning..." / "Connecting..."

// text editor
static lv_obj_t* s_teTitle = nullptr;
static lv_obj_t* s_teTa    = nullptr;
static lv_obj_t* s_teKb    = nullptr;
static void (*s_teOnSave)(const char*) = nullptr;
static uint32_t s_editSeq = 0;

// static-IP chained entry
static String s_ipVals[4];
static int    s_ipStep = -1;

// coords editor
static lv_obj_t *s_taLat, *s_taLon, *s_coordKb;

// ============================================================
//  Toast
// ============================================================
static void toast(const char* msg) {
  if (s_toast) { lv_obj_del(s_toast); s_toast = nullptr; }
  s_toast = lv_label_create(lv_layer_top());
  lv_label_set_text(s_toast, msg);
  lv_obj_set_style_text_font(s_toast, F_UI15, 0);
  lv_obj_set_style_text_color(s_toast, C_IVORY, 0);
  lv_obj_set_style_bg_color(s_toast, C_CARD_HI, 0);
  lv_obj_set_style_bg_opa(s_toast, 235, 0);
  lv_obj_set_style_radius(s_toast, 9, 0);
  lv_obj_set_style_pad_hor(s_toast, 16, 0);
  lv_obj_set_style_pad_ver(s_toast, 9, 0);
  lv_obj_set_style_border_color(s_toast, C_BORDER, 0);
  lv_obj_set_style_border_opa(s_toast, 60, 0);
  lv_obj_set_style_border_width(s_toast, 1, 0);
  lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -18);
  s_toastHideAt = millis() + 1800;
}

// ============================================================
//  Small builders
// ============================================================
static lv_obj_t* mkCloseBtn(lv_obj_t* parent, lv_event_cb_t cb) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_size(b, 40, 40);
  lv_obj_set_style_radius(b, 11, 0);
  lv_obj_set_style_bg_color(b, C_CARD_HI, 0);
  lv_obj_set_style_bg_opa(b, 150, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, 22);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(l, F_SYM16, 0);
  lv_obj_set_style_text_color(l, C_IVORY2, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static lv_obj_t* mkTitle(lv_obj_t* parent, const char* upperText) {
  lv_obj_t* t = lv_label_create(parent);
  lv_label_set_text(t, upperText);              // F_L28 is uppercase-only
  lv_obj_set_style_text_font(t, F_L28, 0);
  lv_obj_set_style_text_color(t, C_IVORY, 0);
  lv_obj_set_pos(t, 28, 24);
  return t;
}

static lv_obj_t* mkGroup(lv_obj_t* col, const char* title) {
  lv_obj_t* g = lv_obj_create(col);
  lv_obj_remove_style_all(g);
  lv_obj_set_width(g, 364);
  lv_obj_set_height(g, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g, C_CARD_HI, 0);
  lv_obj_set_style_bg_opa(g, 110, 0);
  lv_obj_set_style_radius(g, 13, 0);
  lv_obj_set_style_border_color(g, C_BORDER, 0);
  lv_obj_set_style_border_opa(g, 20, 0);
  lv_obj_set_style_border_width(g, 1, 0);
  lv_obj_set_style_pad_hor(g, 14, 0);
  lv_obj_set_style_pad_ver(g, 10, 0);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g, 0, 0);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t = lv_label_create(g);
  lv_label_set_text(t, title);
  lv_obj_add_style(t, &st_microlbl, 0);
  lv_obj_set_style_text_color(t, C_CY_SOFT, 0);
  lv_obj_set_style_pad_bottom(t, 4, 0);
  return g;
}

static lv_obj_t* mkRow(lv_obj_t* group, const char* key, bool divider) {
  if (divider) {
    lv_obj_t* h = lv_obj_create(group);
    lv_obj_remove_style_all(h);
    lv_obj_add_style(h, &st_hair, 0);
    lv_obj_set_size(h, LV_PCT(100), 1);
  }
  lv_obj_t* r = lv_obj_create(group);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_PCT(100), 33);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* k = lv_label_create(r);
  lv_label_set_text(k, key);
  lv_obj_set_style_text_font(k, F_UI15, 0);
  lv_obj_set_style_text_color(k, C_IVORY, 0);
  return r;
}

// right-side "value ›" cluster; returns the value label
static lv_obj_t* mkChevronValue(lv_obj_t* row, const char* val) {
  lv_obj_t* box = lv_obj_create(row);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(box, 7, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* v = lv_label_create(box);
  lv_label_set_text(v, val);
  lv_obj_set_style_text_font(v, F_MONO13, 0);
  lv_obj_set_style_text_color(v, C_IVORY2, 0);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_width(v, 210, 0);   // was 150 — values were ellipsizing early
  lv_obj_t* c = lv_label_create(box);
  lv_label_set_text(c, "›");                 // ›
  lv_obj_set_style_text_font(c, F_MONO13, 0);
  lv_obj_set_style_text_color(c, C_CY, 0);
  return v;
}

static lv_obj_t* mkSwitch(lv_obj_t* row, bool on, lv_event_cb_t cb) {
  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_set_size(sw, 40, 22);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0x3a4a5a), 0);
  lv_obj_set_style_bg_color(sw, C_CY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, C_INK, LV_PART_KNOB);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return sw;
}

static bool swOn(lv_obj_t* sw) { return lv_obj_has_state(sw, LV_STATE_CHECKED); }

// ============================================================
//  Refresh helpers
// ============================================================
static void refreshLocationRows() {
  char b[36];
  snprintf(b, sizeof(b), "%.3f, %.3f", g_set.homeLat, g_set.homeLon);
  lv_label_set_text(s_vCoords, b);
  snprintf(b, sizeof(b), "%d km", g_set.rangeKm);
  lv_label_set_text(s_vRange, b);
}
static void refreshNightRow() {
  char b[16];
  snprintf(b, sizeof(b), "%02d:%02d-%02d:%02d",
           g_set.nightFromMin / 60, g_set.nightFromMin % 60,
           g_set.nightToMin / 60, g_set.nightToMin % 60);
  lv_label_set_text(s_vNight, b);
}
static void refreshFilterRows() {
  if (g_set.filtAltLo == 0 && g_set.filtAltHi == 0)
    lv_label_set_text(s_vAlt, "any");
  else {
    char b[24];
    snprintf(b, sizeof(b), "%d-%d ft", g_set.filtAltLo, g_set.filtAltHi);
    lv_label_set_text(s_vAlt, b);
  }
  lv_label_set_text(s_vWatch,
                    g_set.watchlist.length() ? g_set.watchlist.c_str() : "none");
  static const uint8_t bits[4] = {FCLS_AIRLINER, FCLS_LIGHT, FCLS_HELI, FCLS_MIL};
  for (int i = 0; i < 4; i++) {
    bool on = g_set.filtCls & bits[i];
    lv_obj_set_style_bg_color(s_chip[i], on ? C_CY : C_FAINT, 0);
    lv_obj_set_style_bg_opa(s_chip[i], on ? LV_OPA_COVER : 60, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_chip[i], 0),
                                on ? C_INK : C_DIM, 0);
  }
}
static void refreshFavBtns() {
  for (int i = 0; i < AR_MAX_FAVS; i++) {
    lv_obj_t* l = lv_obj_get_child(s_favBtn[i], 0);
    char b[3];
    if (g_set.favUsed[i]) { snprintf(b, sizeof(b), "%d", i + 1); }
    else                  { snprintf(b, sizeof(b), "+"); }
    lv_label_set_text(l, b);
    lv_obj_set_style_bg_opa(s_favBtn[i], g_set.favUsed[i] ? 40 : 0, 0);
    lv_obj_set_style_border_color(s_favBtn[i],
                                  g_set.favUsed[i] ? C_CY : C_FAINT, 0);
    lv_obj_set_style_text_color(l, g_set.favUsed[i] ? C_CY : C_DIM, 0);
  }
}
static void refreshNetworkRows() {
  lv_label_set_text(s_vWifi,
                    g_set.wifiSsid.length() ? g_set.wifiSsid.c_str() : "none");
  lv_label_set_text(s_vIpMode, g_set.netStatic ? "STATIC" : "DHCP");
  // show host part of feeder url
  String u = g_set.feedUrl;
  int p = u.indexOf("://");
  if (p >= 0) u = u.substring(p + 3);
  p = u.indexOf('/');
  if (p >= 0) u = u.substring(0, p);
  lv_label_set_text(s_vFeed, u.c_str());
  lv_label_set_text(s_vPPass, g_set.panelPass.length() ? "set" : "off");
}

void settingsRefresh() {
  refreshLocationRows();
  refreshNightRow();
  refreshFilterRows();
  refreshFavBtns();
  refreshNetworkRows();
  lv_obj_add_state(s_swLabels, LV_STATE_CHECKED);
  if (!g_set.showLabels) lv_obj_clear_state(s_swLabels, LV_STATE_CHECKED);
  if (g_set.nightEn) lv_obj_add_state(s_swNight, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swNight, LV_STATE_CHECKED);
  if (g_set.wxEn) lv_obj_add_state(s_swWx, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swWx, LV_STATE_CHECKED);
  if (g_set.issEn) lv_obj_add_state(s_swIss, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swIss, LV_STATE_CHECKED);
  if (g_set.logoEn) lv_obj_add_state(s_swLogo, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swLogo, LV_STATE_CHECKED);
  if (g_set.mapEn) lv_obj_add_state(s_swMap, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swMap, LV_STATE_CHECKED);
  if (g_set.mqttEn) lv_obj_add_state(s_swMqtt, LV_STATE_CHECKED);
  else lv_obj_clear_state(s_swMqtt, LV_STATE_CHECKED);
  if (s_footIp) {
    if (g_wifiUp) {
      char b[40];
      snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
      lv_label_set_text(s_footIp, b);
    } else lv_label_set_text(s_footIp, "offline");
  }
}

// ============================================================
//  Location actions
// ============================================================
static void applyNewLocation() {
  settingsSaveLocation();
  for (int i = 0; i < AR_MAX_TRACKS; i++) g_tracks[i].valid = false;
  g_selHex[0] = 0;
  g_orderN = 0;          // stale order would report the OLD location's nearest
  feederKick();
  mapRequestRefresh();
  enrichKickWeather();
}

static void favClicked(lv_event_t* e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED ||
      (lv_event_get_code(e) == LV_EVENT_CLICKED && !g_set.favUsed[i])) {
    g_set.favLat[i] = g_set.homeLat;
    g_set.favLon[i] = g_set.homeLon;
    g_set.favName[i] = "SET";
    g_set.favUsed[i] = true;
    settingsSaveFavs();
    refreshFavBtns();
    toast("Location saved to slot");
    return;
  }
  if (lv_event_get_code(e) == LV_EVENT_CLICKED && g_set.favUsed[i]) {
    g_set.homeLat = g_set.favLat[i];
    g_set.homeLon = g_set.favLon[i];
    applyNewLocation();
    refreshLocationRows();
    toast("Location loaded");
  }
}

// ============================================================
//  Text-edit save callbacks (plain functions, chained where needed)
// ============================================================
static void saveNight(const char* v) {
  int fh, fm, th, tm;
  if (sscanf(v, "%d:%d-%d:%d", &fh, &fm, &th, &tm) == 4 &&
      fh >= 0 && fh < 24 && th >= 0 && th < 24 &&
      fm >= 0 && fm < 60 && tm >= 0 && tm < 60) {
    g_set.nightFromMin = fh * 60 + fm;
    g_set.nightToMin = th * 60 + tm;
    settingsSaveDisplay();
    refreshNightRow();
    toast("Night hours saved");
  } else toast("Use HH:MM-HH:MM");
}
static void saveAltFilter(const char* v) {
  int lo, hi;
  if (sscanf(v, "%d-%d", &lo, &hi) == 2 && lo >= 0 && hi >= 0 &&
      lo <= 60000 && hi <= 60000 && (hi == 0 || lo < hi)) {
    g_set.filtAltLo = lo; g_set.filtAltHi = hi;
    settingsSaveFilters();
    refreshFilterRows();
    tracksRebuildOrder();
    toast("Altitude filter saved");
  } else toast("Use LO-HI ft (0-0 = off)");
}
static void saveWatchlist(const char* v) {
  String s(v);
  s.trim();
  if (s.length() > AR_WATCHLIST_LEN) { toast("Too long"); return; }
  g_set.watchlist = s;
  settingsSaveFilters();
  refreshFilterRows();
  toast("Watchlist saved");
}
static void saveFeeder(const char* v) {
  String s(v);
  s.trim();
  if (!s.startsWith("http://") && !s.startsWith("https://")) {
    toast("Must start with http(s)://");
    return;
  }
  g_set.feedUrl = s;
  g_prefs.putString("feed", s);
  feederUpdateSrcName();
  feederKick();
  refreshNetworkRows();
  toast("Feeder saved");
}
static void saveMqttUri(const char* v) {
  String s(v);
  s.trim();
  if (s.length() && !s.startsWith("mqtt://")) { toast("Must start mqtt://"); return; }
  g_set.mqttUri = s;
  settingsSaveNetworkExtras();
  mqttRestart();
  toast("MQTT saved");
}
static void savePanelPass(const char* v) {
  g_set.panelPass = String(v);
  settingsSaveNetworkExtras();
  refreshNetworkRows();
  toast(g_set.panelPass.length() ? "Password set" : "Password off");
}

// --- static IP chain: IP -> GW -> MASK -> DNS -> confirm reboot ---
static void ipChainNext(const char* v);
static void ipChainOpen() {
  static const char* titles[4] = {"IP ADDRESS", "GATEWAY", "SUBNET MASK",
                                  "DNS (BLANK = GATEWAY)"};
  static const char* defs[4] = {"", "", "255.255.255.0", ""};
  texteditOpen(titles[s_ipStep],
               s_ipVals[s_ipStep].length() ? s_ipVals[s_ipStep].c_str()
                                           : defs[s_ipStep],
               false, ipChainNext);
}
static void rebootMsgboxCb(lv_event_t* e) {
  lv_obj_t* mb = lv_event_get_current_target(e);
  const char* txt = lv_msgbox_get_active_btn_text(mb);
  if (txt && !strcmp(txt, "Reboot")) { delay(150); ESP.restart(); }
  lv_msgbox_close(mb);
}
static void confirmReboot(const char* body) {
  static const char* btns[] = {"Reboot", "Cancel", ""};
  lv_obj_t* mb = lv_msgbox_create(NULL, "Apply settings", body, btns, false);
  lv_obj_center(mb);
  lv_obj_add_event_cb(mb, rebootMsgboxCb, LV_EVENT_VALUE_CHANGED, NULL);
}
static void ipChainNext(const char* v) {
  String s(v);
  s.trim();
  IPAddress tmp;
  bool blankDnsOk = (s_ipStep == 3 && s.length() == 0);
  if (!blankDnsOk && !tmp.fromString(s)) { toast("Invalid address"); ipChainOpen(); return; }
  s_ipVals[s_ipStep] = s;
  if (++s_ipStep < 4) { ipChainOpen(); return; }
  // done — persist + reboot confirm
  g_set.netStatic = true;
  g_set.netIp = s_ipVals[0]; g_set.netGw = s_ipVals[1];
  g_set.netMask = s_ipVals[2]; g_set.netDns = s_ipVals[3];
  g_prefs.putBool("nstat", true);
  g_prefs.putString("nip", g_set.netIp);
  g_prefs.putString("ngw", g_set.netGw);
  g_prefs.putString("nmask", g_set.netMask);
  g_prefs.putString("ndns", g_set.netDns);
  s_ipStep = -1;
  refreshNetworkRows();
  confirmReboot("Static IP saved. Reboot to apply?");
}

// ============================================================
//  Row / switch event handlers
// ============================================================
static void openCoords(lv_event_t*)   { uiShow(SCR_COORDS); }
static void cycleRangeRow(lv_event_t*){ uiCycleRange(+1); refreshLocationRows(); }
static void editNight(lv_event_t*) {
  char b[16];
  snprintf(b, sizeof(b), "%02d:%02d-%02d:%02d",
           g_set.nightFromMin / 60, g_set.nightFromMin % 60,
           g_set.nightToMin / 60, g_set.nightToMin % 60);
  texteditOpen("NIGHT HOURS", b, false, saveNight);
}
static void editAlt(lv_event_t*) {
  char b[24];
  snprintf(b, sizeof(b), "%d-%d", g_set.filtAltLo, g_set.filtAltHi);
  texteditOpen("ALTITUDE LO-HI FT", b, false, saveAltFilter);
}
static void editWatch(lv_event_t*) {
  texteditOpen("WATCHLIST (CSV PREFIXES)", g_set.watchlist.c_str(), false,
               saveWatchlist);
}
static void editFeeder(lv_event_t*) {
  texteditOpen("FEEDER URL", g_set.feedUrl.c_str(), false, saveFeeder);
}
static void editPPass(lv_event_t*) {
  texteditOpen("PANEL PASSWORD (EMPTY = OFF)", "", true, savePanelPass);
}
static void mqttRowLong(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
    texteditOpen("MQTT URI", g_set.mqttUri.c_str(), false, saveMqttUri);
}
static void toggleIpMode(lv_event_t*) {
  if (g_set.netStatic) {
    g_set.netStatic = false;
    g_prefs.putBool("nstat", false);
    refreshNetworkRows();
    confirmReboot("Switch to DHCP. Reboot to apply?");
  } else {
    s_ipStep = 0;
    s_ipVals[0] = g_set.netIp; s_ipVals[1] = g_set.netGw;
    s_ipVals[2] = g_set.netMask; s_ipVals[3] = g_set.netDns;
    ipChainOpen();
  }
}
static void rebootRow(lv_event_t*) { confirmReboot("Reboot the device now?"); }

static void swLabelsCb(lv_event_t*) { g_set.showLabels = swOn(s_swLabels); settingsSaveDisplay(); scopeUpdate(millis()); }
static void swNightCb(lv_event_t*)  { g_set.nightEn = swOn(s_swNight); settingsSaveDisplay(); if (!g_set.nightEn) halBacklight(true); }
static void swWxCb(lv_event_t*)     { g_set.wxEn = swOn(s_swWx); settingsSaveDisplay(); }
static void swIssCb(lv_event_t*)    { g_set.issEn = swOn(s_swIss); settingsSaveDisplay(); }
static void swLogoCb(lv_event_t*)   { g_set.logoEn = swOn(s_swLogo); settingsSaveDisplay(); }
static void swMapCb(lv_event_t*)    { g_set.mapEn = swOn(s_swMap); settingsSaveDisplay(); if (g_set.mapEn) mapRequestRefresh(); scopeApplyMapImage(); }
static void swMqttCb(lv_event_t*)   { g_set.mqttEn = swOn(s_swMqtt); settingsSaveNetworkExtras(); mqttRestart(); }

static void chipCb(lv_event_t* e) {
  static const uint8_t bits[4] = {FCLS_AIRLINER, FCLS_LIGHT, FCLS_HELI, FCLS_MIL};
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  g_set.filtCls ^= bits[i];
  g_set.filtCls |= FCLS_OTHER;                  // OTHER never filtered off
  settingsSaveFilters();
  refreshFilterRows();
  tracksRebuildOrder();
}

// ============================================================
//  Wi-Fi flow
// ============================================================
static String s_scanSsid[AR_MAX_SCAN_APS];    // full SSIDs (labels are lossy)

static void wifiStartScan() {
  s_wifiFlow = WF_SCANNING;
  s_wifiT0 = millis();
  if (s_wifiMsg) { lv_label_set_text(s_wifiMsg, "Scanning..."); lv_obj_clear_flag(s_wifiMsg, LV_OBJ_FLAG_HIDDEN); }
  if (s_wifiList) lv_obj_clean(s_wifiList);
  WiFi.scanDelete();
  WiFi.scanNetworks(true /*async*/);
}
void wifiScreenOpen() { wifiStartScan(); uiShow(SCR_WIFI); }
static void openWifi(lv_event_t*) { wifiScreenOpen(); }

static void wifiPassSaved(const char* pass) {
  s_pendPass = pass;
  s_wifiFlow = WF_CONNECTING;
  s_wifiT0 = millis();
  if (s_wifiMsg) {
    char b[64];
    snprintf(b, sizeof(b), "Connecting to %s ...", s_pendSsid.c_str());
    lv_label_set_text(s_wifiMsg, b);
    lv_obj_clear_flag(s_wifiMsg, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_wifiList) lv_obj_clean(s_wifiList);
  uiShow(SCR_WIFI);
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(s_pendSsid.c_str(), s_pendPass.c_str());
}
static void wifiNetClicked(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= AR_MAX_SCAN_APS || !s_scanSsid[idx].length()) return;
  s_pendSsid = s_scanSsid[idx];        // full SSID, never parsed off the label
  texteditOpen("PASSWORD", "", true, wifiPassSaved);
}
static void wifiRescan(lv_event_t*) { wifiStartScan(); }
static void wifiBack(lv_event_t*)   { uiShow(g_set.wifiSsid.length() ? SCR_SETTINGS : SCR_MAIN); }

static void wifiBuildList(int n) {
  if (!s_wifiList) return;
  lv_obj_clean(s_wifiList);
  if (s_wifiMsg) lv_obj_add_flag(s_wifiMsg, LV_OBJ_FLAG_HIDDEN);
  int shown = 0;
  for (int i = 0; i < n && shown < AR_MAX_SCAN_APS; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;                           // dedup by ssid
    for (int j = 0; j < i; j++) if (WiFi.SSID(j) == ssid) { dup = true; break; }
    if (dup) continue;
    s_scanSsid[shown] = ssid;                   // keep the FULL ssid for connect
    lv_obj_t* b = lv_btn_create(s_wifiList);
    lv_obj_set_size(b, LV_PCT(100), 46);
    lv_obj_set_style_bg_color(b, C_CARD_HI, 0);
    lv_obj_set_style_bg_opa(b, 140, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t* l = lv_label_create(b);
    char t[48];
    snprintf(t, sizeof(t), "%s  (%d)", ssid.substring(0, 24).c_str(), WiFi.RSSI(i));
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, F_UI15, 0);
    lv_obj_set_style_text_color(l, C_IVORY, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(b, wifiNetClicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)shown);
    shown++;
  }
  for (int i = shown; i < AR_MAX_SCAN_APS; i++) s_scanSsid[i] = "";
  WiFi.scanDelete();
}

void wifiScreenBuild() {
  lv_obj_t* root = uiScreenRoot(SCR_WIFI);
  mkTitle(root, "WI-FI");
  mkCloseBtn(root, wifiBack);
  s_wifiMsg = lv_label_create(root);
  lv_label_set_text(s_wifiMsg, "Scanning...");
  lv_obj_set_style_text_font(s_wifiMsg, F_UI15, 0);
  lv_obj_set_style_text_color(s_wifiMsg, C_IVORY2, 0);
  lv_obj_align(s_wifiMsg, LV_ALIGN_TOP_LEFT, 30, 84);

  s_wifiList = lv_obj_create(root);
  lv_obj_remove_style_all(s_wifiList);
  lv_obj_set_pos(s_wifiList, 28, 112);
  lv_obj_set_size(s_wifiList, 500, 350);
  lv_obj_set_flex_flow(s_wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_wifiList, 8, 0);

  lv_obj_t* rescan = lv_btn_create(root);
  lv_obj_set_size(rescan, 160, 46);
  lv_obj_set_pos(rescan, 610, 112);
  lv_obj_set_style_bg_color(rescan, C_CY, 0);
  lv_obj_set_style_radius(rescan, 10, 0);
  lv_obj_t* rl = lv_label_create(rescan);
  lv_label_set_text(rl, "RESCAN");
  lv_obj_set_style_text_font(rl, F_UI15, 0);
  lv_obj_set_style_text_color(rl, C_INK, 0);
  lv_obj_center(rl);
  lv_obj_add_event_cb(rescan, wifiRescan, LV_EVENT_CLICKED, NULL);
}

// ============================================================
//  Coordinates editor
// ============================================================
static void coordFocusCb(lv_event_t* e) {
  lv_keyboard_set_textarea(s_coordKb, lv_event_get_target(e));
}
static void coordSave(lv_event_t*) {
  double la = atof(lv_textarea_get_text(s_taLat));
  double lo = atof(lv_textarea_get_text(s_taLon));
  size_t ll = strlen(lv_textarea_get_text(s_taLat));
  size_t lo2 = strlen(lv_textarea_get_text(s_taLon));
  if (!ll || !lo2 || la < -90 || la > 90 || lo < -180 || lo > 180) {
    toast("Invalid coordinates");
    return;
  }
  g_set.homeLat = la;
  g_set.homeLon = lo;
  applyNewLocation();
  refreshLocationRows();
  toast("Location saved");
  uiShow(SCR_SETTINGS);
}
static void coordCancel(lv_event_t*) { uiShow(SCR_SETTINGS); }

void coordsScreenBuild() {
  lv_obj_t* root = uiScreenRoot(SCR_COORDS);
  mkTitle(root, "COORDINATES");
  mkCloseBtn(root, coordCancel);

  auto mkTa = [](lv_obj_t* root, int x, const char* ph) {
    lv_obj_t* ta = lv_textarea_create(root);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, ph);
    lv_textarea_set_max_length(ta, 12);
    lv_textarea_set_accepted_chars(ta, "0123456789.-");
    lv_obj_set_size(ta, 250, 52);
    lv_obj_set_pos(ta, x, 96);
    lv_obj_set_style_bg_color(ta, C_CARD_HI, 0);
    lv_obj_set_style_bg_opa(ta, 150, 0);
    lv_obj_set_style_text_font(ta, F_MONO13, 0);
    lv_obj_set_style_text_color(ta, C_IVORY, 0);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_add_event_cb(ta, coordFocusCb, LV_EVENT_FOCUSED, NULL);
    return ta;
  };
  s_taLat = mkTa(root, 28, "Latitude");
  s_taLon = mkTa(root, 296, "Longitude");

  lv_obj_t* save = lv_btn_create(root);
  lv_obj_set_size(save, 130, 52);
  lv_obj_set_pos(save, 570, 96);
  lv_obj_set_style_bg_color(save, C_CY, 0);
  lv_obj_set_style_radius(save, 10, 0);
  lv_obj_t* sl = lv_label_create(save);
  lv_label_set_text(sl, "SAVE");
  lv_obj_set_style_text_font(sl, F_UI15, 0);
  lv_obj_set_style_text_color(sl, C_INK, 0);
  lv_obj_center(sl);
  lv_obj_add_event_cb(save, coordSave, LV_EVENT_CLICKED, NULL);

  s_coordKb = lv_keyboard_create(root);
  lv_keyboard_set_mode(s_coordKb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(s_coordKb, SCR_W, 240);
  lv_obj_align(s_coordKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(s_coordKb, s_taLat);
}

// SCR_COORDS entry values refresh
static void coordsPrefill() {
  char b[16];
  snprintf(b, sizeof(b), "%.6f", g_set.homeLat);
  lv_textarea_set_text(s_taLat, b);
  snprintf(b, sizeof(b), "%.6f", g_set.homeLon);
  lv_textarea_set_text(s_taLon, b);
}

// ============================================================
//  Generic text editor
// ============================================================
static void teKbEvent(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    uint32_t seqBefore = s_editSeq;
    static char buf[192];
    strlcpy(buf, lv_textarea_get_text(s_teTa), sizeof(buf));
    if (s_teOnSave) s_teOnSave(buf);
    // if the save callback opened another editor, stay; else return to settings
    if (s_editSeq == seqBefore && g_screen == SCR_TEXTEDIT) uiShow(SCR_SETTINGS);
  } else if (code == LV_EVENT_CANCEL) {
    s_ipStep = -1;                              // abort any chain
    uiShow(SCR_SETTINGS);
  }
}

static void teCancelBtn(lv_event_t*) {
  s_ipStep = -1;                               // abort any chained flow
  uiShow(SCR_SETTINGS);
}

void texteditOpen(const char* title, const char* initial, bool password,
                  void (*onSave)(const char*)) {
  s_editSeq++;
  lv_obj_t* root = uiScreenRoot(SCR_TEXTEDIT);
  lv_obj_clean(root);
  s_teOnSave = onSave;

  mkCloseBtn(root, teCancelBtn);               // always an obvious way out

  s_teTitle = lv_label_create(root);
  lv_label_set_text(s_teTitle, title);
  lv_obj_add_style(s_teTitle, &st_microlbl, 0);
  lv_obj_set_style_text_color(s_teTitle, C_CY_SOFT, 0);
  lv_obj_set_pos(s_teTitle, 30, 30);

  s_teTa = lv_textarea_create(root);
  lv_textarea_set_one_line(s_teTa, true);
  lv_textarea_set_text(s_teTa, initial ? initial : "");
  lv_textarea_set_password_mode(s_teTa, password);
  lv_textarea_set_max_length(s_teTa, 160);
  lv_obj_set_size(s_teTa, 744, 52);
  lv_obj_set_pos(s_teTa, 28, 72);              // clear of the close button
  lv_obj_set_style_bg_color(s_teTa, C_CARD_HI, 0);
  lv_obj_set_style_bg_opa(s_teTa, 150, 0);
  lv_obj_set_style_text_font(s_teTa, F_UI15, 0);
  lv_obj_set_style_text_color(s_teTa, C_IVORY, 0);
  lv_obj_set_style_radius(s_teTa, 10, 0);

  s_teKb = lv_keyboard_create(root);
  lv_obj_set_size(s_teKb, SCR_W, 300);
  lv_obj_align(s_teKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(s_teKb, s_teTa);
  lv_obj_add_event_cb(s_teKb, teKbEvent, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(s_teKb, teKbEvent, LV_EVENT_CANCEL, NULL);

  if (g_screen != SCR_TEXTEDIT) uiShow(SCR_TEXTEDIT);
  else lv_scr_load(root);                        // chained editor: instant swap
}

// ============================================================
//  Settings screen build
// ============================================================
static void closeSettings(lv_event_t*) { uiShow(SCR_MAIN); }

void settingsBuild() {
  lv_obj_t* root = uiScreenRoot(SCR_SETTINGS);
  mkTitle(root, "SETTINGS");
  mkCloseBtn(root, closeSettings);

  lv_obj_t* colL = lv_obj_create(root);
  lv_obj_t* colR = lv_obj_create(root);
  for (lv_obj_t* c : {colL, colR}) {
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 364, 396);            // down to the screen edge
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    // Content is taller than the screen — columns scroll vertically.
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(c, 30, 0);   // room past the footer overlay
  }
  lv_obj_set_pos(colL, 24, 74);
  lv_obj_set_pos(colR, 412, 74);

  // ---------- LEFT ----------
  lv_obj_t* g = mkGroup(colL, "LOCATION");
  lv_obj_t* r = mkRow(g, "Coordinates", false);
  s_vCoords = mkChevronValue(r, "--");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, openCoords, LV_EVENT_CLICKED, NULL);

  r = mkRow(g, "Favourites", true);
  lv_obj_t* favBox = lv_obj_create(r);
  lv_obj_remove_style_all(favBox);
  lv_obj_set_size(favBox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(favBox, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(favBox, 6, 0);
  for (int i = 0; i < AR_MAX_FAVS; i++) {
    lv_obj_t* b = lv_btn_create(favBox);
    lv_obj_set_size(b, 28, 28);
    lv_obj_set_style_radius(b, 7, 0);
    lv_obj_set_style_bg_color(b, C_CY, 0);
    lv_obj_set_style_bg_opa(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, "+");
    lv_obj_set_style_text_font(l, F_MONO13, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, favClicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_add_event_cb(b, favClicked, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
    s_favBtn[i] = b;
  }

  r = mkRow(g, "Range", true);
  s_vRange = mkChevronValue(r, "--");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, cycleRangeRow, LV_EVENT_CLICKED, NULL);

  g = mkGroup(colL, "DISPLAY");
  r = mkRow(g, "Target labels", false);
  s_swLabels = mkSwitch(r, g_set.showLabels, swLabelsCb);
  r = mkRow(g, "Night mode", true);
  lv_obj_t* nbox = lv_obj_create(r);
  lv_obj_remove_style_all(nbox);
  lv_obj_set_size(nbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(nbox, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(nbox, 10, 0);
  lv_obj_set_flex_align(nbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  s_vNight = lv_label_create(nbox);
  lv_obj_set_style_text_font(s_vNight, F_MONO13, 0);
  lv_obj_set_style_text_color(s_vNight, C_IVORY2, 0);
  lv_obj_add_flag(s_vNight, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_vNight, editNight, LV_EVENT_CLICKED, NULL);
  s_swNight = mkSwitch(nbox, g_set.nightEn, swNightCb);
  r = mkRow(g, "Weather strip", true);
  s_swWx = mkSwitch(r, g_set.wxEn, swWxCb);

  g = mkGroup(colL, "LAYERS");
  r = mkRow(g, "ISS on radar", false);
  s_swIss = mkSwitch(r, g_set.issEn, swIssCb);
  r = mkRow(g, "Airline logos", true);
  s_swLogo = mkSwitch(r, g_set.logoEn, swLogoCb);
  r = mkRow(g, "Base map", true);
  s_swMap = mkSwitch(r, g_set.mapEn, swMapCb);

  // ---------- RIGHT ----------
  g = mkGroup(colR, "FILTERS");
  r = mkRow(g, "Aircraft class", false);
  lv_obj_t* chipBox = lv_obj_create(r);
  lv_obj_remove_style_all(chipBox);
  lv_obj_set_size(chipBox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(chipBox, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(chipBox, 5, 0);
  static const char* chipNames[4] = {"AIRLINER", "LIGHT", "HELI", "MIL"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t* b = lv_btn_create(chipBox);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, chipNames[i]);
    lv_obj_set_style_text_font(l, F_MONO11, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, chipCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    s_chip[i] = b;
  }
  r = mkRow(g, "Altitude", true);
  s_vAlt = mkChevronValue(r, "any");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, editAlt, LV_EVENT_CLICKED, NULL);
  r = mkRow(g, "Watchlist", true);
  s_vWatch = mkChevronValue(r, "none");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, editWatch, LV_EVENT_CLICKED, NULL);

  g = mkGroup(colR, "NETWORK");
  r = mkRow(g, "Wi-Fi", false);
  s_vWifi = mkChevronValue(r, "--");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, openWifi, LV_EVENT_CLICKED, NULL);
  r = mkRow(g, "IP mode", true);
  s_vIpMode = mkChevronValue(r, "DHCP");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, toggleIpMode, LV_EVENT_CLICKED, NULL);
  r = mkRow(g, "Feeder", true);
  s_vFeed = mkChevronValue(r, "--");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, editFeeder, LV_EVENT_CLICKED, NULL);
  r = mkRow(g, "Home Assistant", true);
  s_swMqtt = mkSwitch(r, g_set.mqttEn, swMqttCb);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, mqttRowLong, LV_EVENT_LONG_PRESSED, NULL);

  g = mkGroup(colR, "SYSTEM");
  r = mkRow(g, "Panel password", false);
  s_vPPass = mkChevronValue(r, "off");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, editPPass, LV_EVENT_CLICKED, NULL);
  r = mkRow(g, "Reboot device", true);
  mkChevronValue(r, "");
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, rebootRow, LV_EVENT_CLICKED, NULL);

  // ---------- footer ----------
  lv_obj_t* foot = lv_label_create(root);
  lv_label_set_recolor(foot, true);
  lv_label_set_text(foot,
      "airradar.local  ·  v" AR_VERSION "  ·  #3fb6c8 " AR_REPO_URL "#  ·  by Franco Raso");
  lv_obj_set_style_text_font(foot, F_MONO11, 0);
  lv_obj_set_style_text_color(foot, C_DIM, 0);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -18);
  s_footIp = lv_label_create(root);
  lv_label_set_text(s_footIp, "");
  lv_obj_set_style_text_font(s_footIp, F_MONO11, 0);
  lv_obj_set_style_text_color(s_footIp, C_FAINT, 0);
  lv_obj_align(s_footIp, LV_ALIGN_BOTTOM_MID, 0, -4);

  settingsRefresh();
}

// ============================================================
//  Async pump — toast timing, wifi scan/connect state machine
// ============================================================
void settingsPoll(uint32_t nowMs) {
  if (s_toast && (int32_t)(nowMs - s_toastHideAt) >= 0) {   // wrap-safe
    lv_obj_del(s_toast);
    s_toast = nullptr;
  }

  if (s_wifiFlow == WF_SCANNING) {
    int n = WiFi.scanComplete();
    if (n >= 0) { s_wifiFlow = WF_IDLE; wifiBuildList(n); }
    else if (n == WIFI_SCAN_FAILED || nowMs - s_wifiT0 > 12000) {
      s_wifiFlow = WF_IDLE;
      if (s_wifiMsg) lv_label_set_text(s_wifiMsg, "Scan failed - RESCAN to retry");
    }
  } else if (s_wifiFlow == WF_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      s_wifiFlow = WF_IDLE;
      WiFi.setSleep(false);                     // rule #2
      g_set.wifiSsid = s_pendSsid;
      g_set.wifiPass = s_pendPass;
      g_prefs.putString("ssid", g_set.wifiSsid);
      g_prefs.putString("pass", g_set.wifiPass);
      g_wifiUp = true;
      configTzTime(g_set.tz.c_str(), "pool.ntp.org", "time.nist.gov");
      webBegin();
      mqttBegin();
      feederKick();
      mapRequestRefresh();
      enrichKickWeather();
      refreshNetworkRows();
      toast("Connected");
      uiShow(SCR_MAIN);
    } else if (nowMs - s_wifiT0 > 15000) {
      s_wifiFlow = WF_IDLE;
      toast("Connection failed");
      wifiStartScan();
    }
  }

  // prefill coords when that screen becomes active
  static Screen lastScreen = SCR_MAIN;
  if (g_screen != lastScreen) {
    if (g_screen == SCR_COORDS) coordsPrefill();
    lastScreen = g_screen;
  }
}
