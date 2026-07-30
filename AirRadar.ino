// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// AirRadar v6
// Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB, GT911 touch, CH422G expander)
//
// v6 changes:
//   - Settings card removed from the radar view; coords + labels live on the gear
//     screen only. Both side columns are mirrored stacks (tall card over short) that
//     run top-edge to floor; the top-left brand strip is gone to buy that height.
//   - Time card moved to bottom-left; a full-width SETTINGS button (was the corner
//     cog) sits bottom-right.
//   - Overview: HOME location in the header, in-range/heard, conditional emergency
//     line, nearest target + bearing, feed rate (stats.json), altitude colour key.
//   - Selected Aircraft: operator/airframe/year (aircraft DB), tail, selected
//     altitude, always-on squawk, live/coast status, and origin->dest route (adsbdb).
//   - Satellite base map under the scope: keyless Esri World Imagery for the entered
//     coordinates, slate-tinted, rescaled on range change.
//   - Emergency targets flash red on the scope in step with the Overview line.
//
// Data: local ADS-B feeder (tar1090 on :8080) first, airplanes.live fallback.
//
// Board settings (Tools):
//   ESP32S3 Dev Module · USB CDC On Boot: Enabled · Flash: QIO 80MHz, 16MB
//   Partition: 16M Flash (3MB APP/9.9MB FATFS) · PSRAM: OPI PSRAM · 921600

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <math.h>
#include "time.h"

#include "LGFX_Waveshare_7.h"

// ============================================================
//  Defaults (editable on-device / in browser afterwards)
// ============================================================
// Set your own location on first boot (coordinate numpad or web UI); these are
// just neutral placeholders so the repo ships no real address.
#define DEFAULT_LAT   0.0
#define DEFAULT_LON   0.0
#define HOME_LABEL   "HOME"
// POSIX TZ string - set yours (e.g. "EST5EDT,M3.2.0,M11.1.0" for US/Canada Eastern)
#define TZ_STRING    "UTC0"
#define MDNS_NAME    "airradar"

// about / credit (shown on the settings screen and web page)
#define APP_VERSION  "v6.0"
#define REPO_URL     "github.com/MrRonco/AirRadar"
#define AUTHOR_LINE  "A hobby ADS-B radar by Franco Raso"

// Local ADS-B feeder - tried first every poll; cloud is fallback.
// adsb.im image: tar1090 is on port 8080 (port 80 is the feeder config app).
// Set your feeder's address on first boot (Settings card or web UI).
#define LOCAL_FEED_URL_DEFAULT "http://adsb.local:8080/data/aircraft.json"
#define LOCAL_FEED_NAME        "LOCAL"

const uint32_t POLL_LOCAL_MS    = 2000;    // own feeder: no rate limit
const uint32_t POLL_CLOUD_MS    = 8000;    // airplanes.live courtesy
const uint32_t POS_TICK_MS      = 2000;
const uint32_t STALE_TRACK_MS   = 20000;
const uint32_t DROP_TRACK_MS    = 60000;
const uint32_t STALE_FEED_MS    = 30000;
const int      TRAIL_LEN        = 5;
const int      TRAIL_EVERY_TICK = 5;
#define CLOCK_12H 1
// ============================================================

LGFX lcd;
LGFX_Sprite bg(&lcd);      // 800x480 static chrome
LGFX_Sprite plot(&lcd);    // dynamic radar region

Preferences prefs;
WebServer server(80);

double  homeLat = DEFAULT_LAT, homeLon = DEFAULT_LON;
bool    showLabels = true;
int     rangeKm = 100;                    // 50 / 100 / 150 / 250 (NVS)
String  wifiSsid, wifiPass;
bool    serverUp = false;
String  feedUrl;
bool    feedIsLocal = true;

// network config (NVS)
bool    netStatic = false;
String  netIp, netGw, netMask, netDns;

// ---------- layout ----------
const int RCX = 400, RCY = 226, R = 195;
const float X_CORR = 0.93f;
const int PLOT_X = 200, PLOT_Y = 22;
const int PLOT_W = 400, PLOT_H = 408;

// v6 mirrored columns: tall card (14..374) over short card (382..470), both sides
const int OV_X=12,  OV_Y=14,  OV_W=168, OV_H=360;   // overview card (tall, left)
const int TM_X=12,  TM_Y=382, TM_W=168, TM_H=88;    // time card (short, left)
const int SC_X=620, SC_Y=14,  SC_W=168, SC_H=360;   // selected craft card (tall, right)
const int SET_X=620,SET_Y=382, SET_W=168,SET_H=88;  // settings button (short, right)
const int ARROW_L_X=216, ARROW_R_X=584, ARROW_Y=388, ARROW_R=22;

// ---------- palette ----------
uint16_t colTxtHi, colTxtMd, colTxtLo, colAcc, colAccDim;
uint16_t colHighA, colMidA, colLowA, colBad;
uint16_t colBorder, colBorderHi, colCard, colKey, colKeyAcc;
const uint8_t BG1[3]={10,14,19}, BG2[3]={14,20,32};
const uint8_t GLASS[3]={26,36,50};

// ---------- tracks ----------
struct Track {
  bool valid;
  char hex[8], flight[12], typeCode[8], category[5], squawk[6], reg[10];
  char desc[24], ownOp[20], year[6];   // aircraft DB (may be blank on some feeds)
  char origin[5], dest[5];             // route from adsbdb (Tier C), "" until looked up
  bool routeTried;                     // route lookup attempted for this hex
  bool mil;                            // dbFlags bit0 = military / interesting
  double lat, lon;
  float gsKt, trackDeg;
  int altFt, vRateFpm, navAltFt;       // navAltFt = autopilot selected altitude (-1 none)
  uint32_t lastApiMs;
  float trailX[TRAIL_LEN], trailY[TRAIL_LEN];
  int trailCount, trailHead;
};
const int MAX_TRACKS = 40;
Track tracks[MAX_TRACKS];

struct ApiPlane {
  char hex[8], flight[12], typeCode[8], category[5], squawk[6], reg[10];
  char desc[24], ownOp[20], year[6];
  bool mil;
  double lat, lon; float gsKt, trackDeg; int altFt, vRateFpm, navAltFt;
};
ApiPlane pendingPlanes[MAX_TRACKS];
int  pendingCount=0, pendingHeard=0;
bool pendingReady=false, pendingOk=false, pendingLocal=true, fetchInProgress=false;
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastFetchStart=0, lastGoodApply=0, lastPosTick=0, lastClockTick=0;
int posTickCounter=0, lastSubMin=-1, visibleCount=0, heardCount=0;

char selHex[8]="";
int  orderIdx[MAX_TRACKS], orderN=0;

// feed rate (stats.json, local feeder only) — msgs/sec, -1 = unknown
volatile float feedMsgRate=-1.0f;
uint32_t lastStatsStart=0;
const uint32_t POLL_STATS_MS=15000;

// emergency blink phase (toggled on each radar tick)
bool emergBlink=false;

// satellite base map (Esri World Imagery, decoded into bg plot region)
uint8_t* satBuf=nullptr;         // PSRAM JPEG buffer
volatile int satLen=0;           // bytes ready to decode, 0 = none
volatile bool satFetching=false; // fetch task running
volatile bool satReady=false;    // new image waiting for main loop to decode
int satForRange=-1;              // range the pending/last image was fetched for
bool satEnabled=true;

// route lookup (adsbdb, Tier C) — single-slot request handed to a worker task
char routeReqHex[8]="", routeReqFlight[12]="";
char routeResHex[8]=""; char routeResOrigin[5]="", routeResDest[5]="";
volatile bool routeReqPending=false, routeResReady=false, routeFetching=false;

enum Screen { SCR_MAIN, SCR_WIFI_SCAN, SCR_WIFI_PASS, SCR_COORDS, SCR_MENU, SCR_NET };
Screen screen = SCR_MAIN;

String scanSsid[7]; int scanRssi[7]; int scanCount=0;
String pendSsid, kbText; bool kbMask=true; int kbLayer=0;
String coordLat, coordLon; int coordField=0;

// network editor state
bool   editStatic=false;
String netVals[4];          // IP, GW, MASK, DNS being edited
int    netField=0;
bool wasTouched=false;

void startServer();
void drawMainScreen();
void drawNetScreen();
void startSatFetch();
void requestRoute(const char* hex,const char* flight);
void drawSatelliteBase();
void fetchStats();

// ============================================================
//  Math helpers
// ============================================================
double deg2rad(double d){ return d*M_PI/180.0; }
double rad2deg(double r){ return r*180.0/M_PI; }
float haversineKm(double la1,double lo1,double la2,double lo2){
  double dLa=deg2rad(la2-la1), dLo=deg2rad(lo2-lo1);
  double a=sin(dLa/2)*sin(dLa/2)+cos(deg2rad(la1))*cos(deg2rad(la2))*sin(dLo/2)*sin(dLo/2);
  return 6371.0*2*atan2(sqrt(a),sqrt(1-a));
}
float bearingTo(double la1,double lo1,double la2,double lo2){
  double y=sin(deg2rad(lo2-lo1))*cos(deg2rad(la2));
  double x=cos(deg2rad(la1))*sin(deg2rad(la2))-sin(deg2rad(la1))*cos(deg2rad(la2))*cos(deg2rad(lo2-lo1));
  double b=rad2deg(atan2(y,x)); if(b<0)b+=360; return (float)b;
}
bool toScreen(float dKm,float brg,float &sx,float &sy){
  if(dKm>(float)rangeKm) return false;
  float rr=(dKm/(float)rangeKm)*R, rad=deg2rad(brg);
  sx=RCX+sinf(rad)*rr*X_CORR; sy=RCY-cosf(rad)*rr; return true;
}
void rotPt(float lx,float ly,float tDeg,float &ox,float &oy){
  float t=deg2rad(tDeg);
  ox=(lx*cosf(t)-ly*sinf(t))*X_CORR; oy=lx*sinf(t)+ly*cosf(t);
}
void altRGB(int alt,uint8_t &r,uint8_t &g,uint8_t &b){
  if(alt>=30000){ r=154; g=123; b=255; }
  else if(alt>=10000){ r=76; g=194; b=255; }
  else if(alt>=0){ r=255; g=180; b=84; }
  else { r=255; g=93; b=93; }
}
bool isEmergency(const char*sq){
  return !strcmp(sq,"7500")||!strcmp(sq,"7600")||!strcmp(sq,"7700");
}
const char* cardinal8(float deg){
  static const char* c[8]={"N","NE","E","SE","S","SW","W","NW"};
  return c[((int)((deg+22.5f)/45.0f))&7];
}
struct RegC{const char*p;const char*name;};
const RegC REGC[]={
  {"C-","CANADA"},{"CF-","CANADA"},{"N","USA"},{"XA-","MEXICO"},{"XB-","MEXICO"},
  {"G-","UK"},{"EI-","IRELAND"},{"D-","GERMANY"},{"F-","FRANCE"},{"PH-","NETHERL"},
  {"OO-","BELGIUM"},{"LX-","LUXEMBRG"},{"HB-","SWITZERL"},{"OE-","AUSTRIA"},
  {"I-","ITALY"},{"EC-","SPAIN"},{"CS-","PORTUGAL"},{"SE-","SWEDEN"},{"LN-","NORWAY"},
  {"OY-","DENMARK"},{"OH-","FINLAND"},{"TF-","ICELAND"},{"SP-","POLAND"},
  {"OK-","CZECHIA"},{"TC-","TURKIYE"},{"SX-","GREECE"},{"PP-","BRAZIL"},{"PR-","BRAZIL"},
  {"PT-","BRAZIL"},{"JA","JAPAN"},{"HL","S KOREA"},{"B-","CHN/TWN"},{"VT-","INDIA"},
  {"VH-","AUSTRAL"},{"ZK-","N ZEALND"},{"ZS-","S AFRICA"},{"A6-","UAE"},{"A7-","QATAR"},
  {"HZ-","SAUDI"},{"4X-","ISRAEL"},{"9M-","MALAYSIA"},{"PK-","INDONESA"},
};
const char* regCountry(const char* reg){
  if(!reg||!reg[0]) return "---";
  int bl=0; const char* best="INTL";
  for(auto &e:REGC){ int l=strlen(e.p);
    if(!strncmp(reg,e.p,l)&&l>bl){bl=l;best=e.name;} }
  return best;
}
const char* categoryName(const char* c){
  if(!c||!c[0]) return "---";
  if(!strcmp(c,"A1"))return"LIGHT"; if(!strcmp(c,"A2"))return"SMALL";
  if(!strcmp(c,"A3"))return"LARGE"; if(!strcmp(c,"A4"))return"B757";
  if(!strcmp(c,"A5"))return"HEAVY"; if(!strcmp(c,"A6"))return"HI-PERF";
  if(!strcmp(c,"A7"))return"ROTOR"; if(!strcmp(c,"B1"))return"GLIDER";
  if(!strcmp(c,"B2"))return"BALLOON"; if(!strcmp(c,"B4"))return"ULTRALT";
  if(!strcmp(c,"B6"))return"UAV";
  return c;
}
Track* findByHex(const char* h){
  if(!h[0]) return nullptr;
  for(int i=0;i<MAX_TRACKS;i++)
    if(tracks[i].valid&&!strcmp(tracks[i].hex,h)) return &tracks[i];
  return nullptr;
}

// ============================================================
//  Glass chrome helpers
// ============================================================
uint32_t noiseState=0x2f6e2b1;
int frostNoise(){ noiseState=noiseState*1664525u+1013904223u; return (int)((noiseState>>16)&3)-1; }

uint16_t blend565(uint16_t c,uint8_t tr,uint8_t tg,uint8_t tb,uint8_t a){
  int r=((c>>11)&0x1f)<<3, g=((c>>5)&0x3f)<<2, b=(c&0x1f)<<3;
  r+= (a*(tr-r))>>8; g+=(a*(tg-g))>>8; b+=(a*(tb-b))>>8;
  return lcd.color565(r,g,b);
}

void glassRect(int x,int y,int w,int h,int rad){
  for(int yy=0; yy<h; yy++){
    for(int xx=0; xx<w; xx++){
      int cx=-1, cy=-1;
      if(xx<rad&&yy<rad){cx=rad-1;cy=rad-1;}
      else if(xx>=w-rad&&yy<rad){cx=w-rad;cy=rad-1;}
      else if(xx<rad&&yy>=h-rad){cx=rad-1;cy=h-rad;}
      else if(xx>=w-rad&&yy>=h-rad){cx=w-rad;cy=h-rad;}
      if(cx>=0){ int dx=xx-cx,dy=yy-cy; if(dx*dx+dy*dy>rad*rad) continue; }
      uint16_t c=bg.readPixel(x+xx,y+yy);
      uint16_t o=blend565(c,GLASS[0],GLASS[1],GLASS[2],185);
      int n=frostNoise();
      if(n){ int r=((o>>11)&0x1f)<<3, g=((o>>5)&0x3f)<<2, b=(o&0x1f)<<3;
        r=constrain(r+n*2,0,255); g=constrain(g+n*2,0,255); b=constrain(b+n*2,0,255);
        o=lcd.color565(r,g,b); }
      if(yy<2) o=blend565(o,255,255,255,34);
      else if(yy>=h-3) o=blend565(o,0,0,0,40);
      bg.drawPixel(x+xx,y+yy,o);
    }
  }
  bg.drawRoundRect(x,y,w,h,rad,colBorder);
  bg.drawFastHLine(x+rad,y,w-rad*2,colBorderHi);
}

void glassCircle(int cx,int cy,int r){
  for(int yy=-r; yy<=r; yy++)
    for(int xx=-r; xx<=r; xx++){
      if(xx*xx+yy*yy>r*r) continue;
      uint16_t c=bg.readPixel(cx+xx,cy+yy);
      uint16_t o=blend565(c,GLASS[0],GLASS[1],GLASS[2],190);
      if(yy<-(r-3)) o=blend565(o,255,255,255,30);
      bg.drawPixel(cx+xx,cy+yy,o);
    }
  bg.drawCircle(cx,cy,r,colBorder);
}

void restore(int x,int y,int w,int h){
  lcd.setClipRect(x,y,w,h);
  bg.pushSprite(0,0);
  lcd.clearClipRect();
}

void gradientBg(){
  for(int y=0;y<480;y++){
    uint8_t r=BG1[0]+(BG2[0]-BG1[0])*y/479;
    uint8_t g=BG1[1]+(BG2[1]-BG1[1])*y/479;
    uint8_t b=BG1[2]+(BG2[2]-BG1[2])*y/479;
    lcd.drawFastHLine(0,y,800,lcd.color565(r,g,b));
  }
}

void btn(int x,int y,int w,int h,const char* label,bool accent=false){
  lcd.fillRoundRect(x,y,w,h,10,accent?colAcc:colKey);
  lcd.drawRoundRect(x,y,w,h,10,accent?colAcc:colBorder);
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(accent?lcd.color565(8,14,20):colTxtHi);
  lcd.drawString(label,x+w/2,y+h/2);
}
bool hit(int tx,int ty,int x,int y,int w,int h){
  return tx>=x&&tx<x+w&&ty>=y&&ty<y+h;
}
bool hitC(int tx,int ty,int cx,int cy,int r){
  int dx=tx-cx,dy=ty-cy; return dx*dx+dy*dy<=r*r;
}

// ============================================================
//  Static chrome build (runs once at boot)
// ============================================================
// Scope rings, spokes, compass, crosshair — drawn into bg. Split out so it can be
// re-laid over the satellite base after a new image is composited in.
void drawScopeRings(){
  uint16_t ringDim=lcd.color565(34,48,64), ringHi=lcd.color565(64,92,122);
  int rr[4]={49,98,146,R};
  for(int i=0;i<3;i++) bg.drawEllipse(RCX,RCY,(int)(rr[i]*X_CORR),rr[i],ringDim);
  bg.drawEllipse(RCX,RCY,(int)(R*X_CORR),R,ringHi);
  bg.drawEllipse(RCX,RCY,(int)(R*X_CORR)+1,R+1,ringHi);
  bg.drawFastHLine(RCX-(int)(R*X_CORR),RCY,(int)(R*X_CORR)*2,lcd.color565(22,32,44));
  bg.drawFastVLine(RCX,RCY-R,R*2,lcd.color565(22,32,44));
  for(int a=0;a<360;a+=30){
    float rad=deg2rad((float)a), sx=sinf(rad)*X_CORR, sv=-cosf(rad);
    bg.drawLine(RCX+sx*(R-8),RCY+sv*(R-8),RCX+sx*(R-1),RCY+sv*(R-1),ringHi);
  }
  bg.setTextDatum(middle_center);
  bg.setFont(&fonts::FreeSans9pt7b);
  bg.setTextColor(colAcc);   bg.drawString("N",RCX,RCY-R+20);
  bg.setTextColor(colTxtLo);
  bg.drawString("E",RCX+(int)((R-20)*X_CORR),RCY);
  bg.drawString("S",RCX,RCY+R-20);
  bg.drawString("W",RCX-(int)((R-20)*X_CORR),RCY);
  bg.drawLine(RCX,RCY-6,RCX+5,RCY,colAcc); bg.drawLine(RCX+5,RCY,RCX,RCY+6,colAcc);
  bg.drawLine(RCX,RCY+6,RCX-5,RCY,colAcc); bg.drawLine(RCX-5,RCY,RCX,RCY-6,colAcc);
}

void buildChrome(){
  for(int y=0;y<480;y++){
    uint8_t r=BG1[0]+(BG2[0]-BG1[0])*y/479;
    uint8_t g=BG1[1]+(BG2[1]-BG1[1])*y/479;
    uint8_t b=BG1[2]+(BG2[2]-BG1[2])*y/479;
    bg.drawFastHLine(0,y,800,lcd.color565(r,g,b));
  }
  uint16_t deco=lcd.color565(19,27,38);
  bg.drawEllipse(RCX,RCY,(int)(250*X_CORR),250,deco);
  bg.drawEllipse(RCX,RCY,(int)(310*X_CORR),310,deco);
  bg.drawEllipse(RCX,RCY,(int)(380*X_CORR),380,deco);

  drawScopeRings();

  // glass cards (mirrored columns + range pill + cycle arrows)
  glassRect(OV_X,OV_Y,OV_W,OV_H,14);
  glassRect(SC_X,SC_Y,SC_W,SC_H,14);
  glassRect(TM_X,TM_Y,TM_W,TM_H,14);
  glassRect(SET_X,SET_Y,SET_W,SET_H,14);
  glassRect(348,446,104,24,12);                 // range pill (text is dynamic)
  glassCircle(ARROW_L_X,ARROW_Y,ARROW_R);
  glassCircle(ARROW_R_X,ARROW_Y,ARROW_R);

  // ---- Overview header: eyebrow + HOME location (drawn home icon; ASCII-safe) ----
  bg.setFont(&fonts::FreeSansBold9pt7b);
  bg.setTextColor(colTxtLo);
  bg.setTextDatum(top_center);
  bg.drawString("OVERVIEW",OV_X+OV_W/2,OV_Y+9);
  {
    bg.setFont(&fonts::FreeSansBold12pt7b);
    int tw=bg.textWidth(HOME_LABEL);
    int total=tw+20;                            // 14px icon + 6px gap
    int lx=OV_X+OV_W/2-total/2, iy=OV_Y+30;
    bg.fillTriangle(lx,iy,lx+7,iy-8,lx+14,iy,colAcc);   // roof
    bg.fillRect(lx+2,iy,10,8,colAcc);                   // body
    bg.setTextDatum(top_left);
    bg.setTextColor(colAcc);
    bg.drawString(HOME_LABEL,lx+20,OV_Y+22);
  }
  bg.drawFastHLine(OV_X+12,OV_Y+50,OV_W-24,colBorder);
  bg.drawFastHLine(OV_X+12,OV_Y+290,OV_W-24,colBorder);

  // ---- Overview altitude colour key (static) ----
  bg.setTextDatum(top_left);
  bg.setFont(&fonts::Font0);
  bg.setTextColor(colTxtLo);
  bg.drawString("ALTITUDE",OV_X+14,OV_Y+298);
  {
    struct AK{int alt;const char*lab;};
    AK ak[3]={{5000,"<10k"},{20000,"10-30k"},{40000,">30k"}};
    int ay=OV_Y+314;
    for(int i=0;i<3;i++){
      uint8_t r,g,b; altRGB(ak[i].alt,r,g,b);
      int cx=OV_X+18+i*52;
      bg.fillCircle(cx,ay+5,4,bg.color565(r,g,b));
      bg.setTextColor(colTxtMd);
      bg.drawString(ak[i].lab,cx+8,ay+1);
    }
  }

  // ---- Selected Aircraft header ----
  bg.setFont(&fonts::FreeSansBold9pt7b);
  bg.setTextColor(colTxtLo);
  bg.setTextDatum(top_center);
  bg.drawString("SELECTED",SC_X+SC_W/2,SC_Y+8);
  bg.drawString("AIRCRAFT",SC_X+SC_W/2,SC_Y+26);

  // ---- Settings button: drawn gear + label, accent-outlined ----
  {
    int cx=SET_X+34, cy=SET_Y+SET_H/2;
    bg.drawCircle(cx,cy,7,colAcc);
    bg.drawCircle(cx,cy,2,colAcc);
    for(int a=0;a<360;a+=60){
      float rad=deg2rad((float)a);
      bg.drawLine(cx+cosf(rad)*7,cy+sinf(rad)*7,cx+cosf(rad)*11,cy+sinf(rad)*11,colAcc);
    }
    bg.setFont(&fonts::FreeSansBold12pt7b);
    bg.setTextColor(colTxtHi);
    bg.setTextDatum(middle_left);
    bg.drawString("SETTINGS",SET_X+56,cy);
    bg.drawRoundRect(SET_X+1,SET_Y+1,SET_W-2,SET_H-2,14,colAccDim);
  }

  // arrow chevrons (live inside the scope; copied into the plot each tick)
  bg.fillTriangle(ARROW_L_X+7,ARROW_Y-10,ARROW_L_X+7,ARROW_Y+10,ARROW_L_X-8,ARROW_Y,colAcc);
  bg.fillTriangle(ARROW_R_X-7,ARROW_Y-10,ARROW_R_X-7,ARROW_Y+10,ARROW_R_X+8,ARROW_Y,colAcc);
}

// Composite the fetched Esri JPEG into the bg scope circle, slate-tinted, then re-lay
// the rings and attribution over it. One-shot on boot / range change (never per-frame).
void drawSatelliteBase(){
  if(!satReady||satLen<=0||!satBuf){ satReady=false; return; }
  LGFX_Sprite sat(&lcd); sat.setPsram(true); sat.setColorDepth(16);
  if(!sat.createSprite(PLOT_W,PLOT_H)){ satReady=false; return; }
  sat.fillScreen(lcd.color565(BG1[0],BG1[1],BG1[2]));
  // image is requested at exactly PLOT_W x PLOT_H, so draw it 1:1
  sat.drawJpg(satBuf,(size_t)satLen,0,0);

  for(int yy=0; yy<PLOT_H; yy++){
    int gy=PLOT_Y+yy, dy=gy-RCY;
    for(int xx=0; xx<PLOT_W; xx++){
      int gx=PLOT_X+xx; float fx=(gx-RCX)/X_CORR;
      float rd2=fx*fx+(float)dy*dy;
      if(rd2>(float)R*R) continue;                       // clip to scope circle
      uint16_t c=sat.readPixel(xx,yy);
      int r=((c>>11)&0x1f)<<3, g=((c>>5)&0x3f)<<2, b=(c&0x1f)<<3;
      int lum=(r*77+g*150+b*29)>>8;                      // luminance
      // slate-cyan monochrome ramp (keeps the green earth out of the UI palette)
      r=(lum*90)>>8;  r+=8;
      g=(lum*128)>>8; g+=16;
      b=(lum*182)>>8; b+=26;
      float vig=1.0f-0.55f*(rd2/((float)R*R));            // darken toward the edge
      r=(int)(r*vig); g=(int)(g*vig); b=(int)(b*vig);
      bg.drawPixel(gx,gy,bg.color565(constrain(r,0,255),constrain(g,0,255),constrain(b,0,255)));
    }
  }
  sat.deleteSprite();

  drawScopeRings();                                       // rings back on top of imagery
  bg.setFont(&fonts::Font0);
  bg.setTextDatum(bottom_right);
  bg.setTextColor(lcd.color565(120,140,158));
  bg.drawString("Esri, Maxar",RCX+(int)(R*X_CORR)-4,RCY+R-1);   // required attribution
  bg.setTextDatum(top_left);

  satReady=false;
  if(screen==SCR_MAIN) drawMainScreen();                  // repaint everything from bg
}

// ============================================================
//  API fetch (core 0)
// ============================================================
void postPending(bool ok){
  portENTER_CRITICAL(&dataMux);
  pendingOk=ok; pendingReady=true;
  portEXIT_CRITICAL(&dataMux);
}

void fillFilter(JsonObject f){
  f["hex"]=true; f["flight"]=true; f["lat"]=true; f["lon"]=true;
  f["alt_baro"]=true; f["alt_geom"]=true; f["gs"]=true; f["track"]=true;
  f["t"]=true; f["category"]=true; f["baro_rate"]=true; f["geom_rate"]=true;
  f["squawk"]=true; f["r"]=true; f["seen_pos"]=true;
  // v6 enrichment (aircraft DB fields; absent on some feeds — parsed defensively)
  f["desc"]=true; f["ownOp"]=true; f["year"]=true; f["dbFlags"]=true;
  f["nav_altitude_mcp"]=true;
}

bool fetchParse(Stream &s, bool local){
  StaticJsonDocument<1024> filter;
  fillFilter(filter["ac"].createNestedObject());
  fillFilter(filter["aircraft"].createNestedObject());

  DynamicJsonDocument doc(65536);
  DeserializationError err=deserializeJson(doc,s,DeserializationOption::Filter(filter));
  if(err){ Serial.printf("JSON(%s): %s\n",local?"local":"cloud",err.c_str()); return false; }

  JsonArray arr=doc["aircraft"].as<JsonArray>();
  if(arr.isNull()) arr=doc["ac"].as<JsonArray>();
  if(arr.isNull()) return false;

  ApiPlane temp[MAX_TRACKS]; int n=0, heard=0;
  for(JsonObject ac: arr){
    if(!ac["lat"].is<float>()||!ac["lon"].is<float>()) continue;
    if(local){
      float sp=ac["seen_pos"]|999.0f;
      if(sp>15.0f) continue;
    }
    heard++;                                   // aircraft with a live position
    double lat=ac["lat"], lon=ac["lon"];
    if(haversineKm(homeLat,homeLon,lat,lon)>(float)rangeKm) continue;
    if(n>=MAX_TRACKS) continue;                // in range but table full
    ApiPlane &p=temp[n];
    memset(&p,0,sizeof(ApiPlane));
    strlcpy(p.hex,ac["hex"]|"",sizeof(p.hex));
    strlcpy(p.flight,ac["flight"]|"",sizeof(p.flight));
    strlcpy(p.typeCode,ac["t"]|"",sizeof(p.typeCode));
    strlcpy(p.category,ac["category"]|"",sizeof(p.category));
    strlcpy(p.squawk,ac["squawk"]|"",sizeof(p.squawk));
    strlcpy(p.reg,ac["r"]|"",sizeof(p.reg));
    strlcpy(p.desc,ac["desc"]|"",sizeof(p.desc));
    strlcpy(p.ownOp,ac["ownOp"]|"",sizeof(p.ownOp));
    if(ac["year"].is<const char*>()) strlcpy(p.year,ac["year"]|"",sizeof(p.year));
    else if(ac["year"].is<int>()) snprintf(p.year,sizeof(p.year),"%d",ac["year"].as<int>());
    int flags=ac["dbFlags"]|0; p.mil=((flags&3)!=0);
    p.lat=lat; p.lon=lon;
    p.gsKt=ac["gs"]|0.0f; p.trackDeg=ac["track"]|0.0f;
    if(ac["alt_baro"].is<int>()) p.altFt=ac["alt_baro"];
    else if(ac["alt_baro"].is<const char*>()) p.altFt=0;
    else if(ac["alt_geom"].is<int>()) p.altFt=ac["alt_geom"];
    else p.altFt=-1;
    if(ac["baro_rate"].is<int>()) p.vRateFpm=ac["baro_rate"];
    else if(ac["geom_rate"].is<int>()) p.vRateFpm=ac["geom_rate"];
    else p.vRateFpm=0;
    p.navAltFt=ac["nav_altitude_mcp"].is<int>()?ac["nav_altitude_mcp"].as<int>():-1;
    n++;
  }

  portENTER_CRITICAL(&dataMux);
  pendingCount=n; pendingHeard=heard;
  memcpy(pendingPlanes,temp,sizeof(ApiPlane)*n);
  pendingOk=true; pendingLocal=local; pendingReady=true;
  portEXIT_CRITICAL(&dataMux);
  Serial.printf("Fetch(%s): %d aircraft\n",local?"local":"cloud",n);
  return true;
}

void fetchAircraftData(){
  if(WiFi.status()!=WL_CONNECTED){ postPending(false); return; }

  // 1) local feeder - plain HTTP on the LAN, fast connect timeout
  {
    String urls[2]={feedUrl,String()};
    if(feedUrl.indexOf("/tar1090/data/")>=0){
      urls[1]=feedUrl; urls[1].replace("/tar1090/data/","/data/");
    }
    for(int u=0;u<2&&urls[u].length();u++){
      WiFiClient net;
      HTTPClient http;
      http.setConnectTimeout(1500);
      http.setTimeout(4000);
      http.useHTTP10(true);
      if(!http.begin(net,urls[u])) continue;
      int code=http.GET();
      bool ok=(code==200)&&fetchParse(http.getStream(),true);
      http.end();
      if(ok){
        if(millis()-lastStatsStart>=POLL_STATS_MS){ lastStatsStart=millis(); fetchStats(); }
        return;
      }
      Serial.printf("Local feed %s -> HTTP %d\n",urls[u].c_str(),code);
    }
  }

  // 2) cloud fallback - airplanes.live
  {
    int radiusNm=(int)ceilf(rangeKm/1.852f); if(radiusNm>250) radiusNm=250;
    String url="https://api.airplanes.live/v2/point/";
    url+=String(homeLat,4); url+="/"; url+=String(homeLon,4); url+="/";
    url+=String(radiusNm);
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true); http.setTimeout(12000);
    if(http.begin(client,url)){
      http.addHeader("User-Agent","ESP32-AirRadar/6.0");
      int code=http.GET();
      bool ok=(code==200)&&fetchParse(http.getStream(),false);
      http.end();
      if(ok) return;
      Serial.printf("Cloud API -> HTTP %d\n",code);
    }
  }
  postPending(false);
}

void fetchTask(void*){ fetchAircraftData(); fetchInProgress=false; vTaskDelete(NULL); }
void startFetch(){
  if(fetchInProgress) return;
  fetchInProgress=true; lastFetchStart=millis();
  xTaskCreatePinnedToCore(fetchTask,"fetch",12000,NULL,1,NULL,0);
}

// ---- Feed rate: tar1090/readsb stats.json (local feeder only) ----
void fetchStats(){
  String url=feedUrl;
  int q=url.lastIndexOf('/');
  if(q<0){ return; }
  url=url.substring(0,q+1)+"stats.json";
  WiFiClient net; HTTPClient http;
  http.setConnectTimeout(1500); http.setTimeout(4000); http.useHTTP10(true);
  if(!http.begin(net,url)) return;
  int code=http.GET();
  if(code==200){
    StaticJsonDocument<192> filt;
    filt["last1min"]["messages"]=true;
    DynamicJsonDocument doc(4096);
    if(!deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filt))){
      long m=doc["last1min"]["messages"]|(long)-1;
      if(m>=0) feedMsgRate=m/60.0f;
    }
  }
  http.end();
}

// ---- Satellite base map: keyless Esri World Imagery for the current view ----
const size_t SAT_MAX=320*1024;
void fetchSatellite(){
  if(!satEnabled||WiFi.status()!=WL_CONNECTED) return;
  if(!satBuf){ satBuf=(uint8_t*)ps_malloc(SAT_MAX); if(!satBuf) return; }
  const double ER=6378137.0;
  double cx=ER*deg2rad(homeLon);
  double cy=ER*log(tan(M_PI/4.0+deg2rad(homeLat)/2.0));
  double half=(double)rangeKm*1000.0;          // scope radius in metres
  char url[360];
  snprintf(url,sizeof(url),
    "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/export"
    "?bbox=%.1f,%.1f,%.1f,%.1f&bboxSR=3857&imageSR=3857&size=%d,%d&format=jpg&f=image",
    cx-half,cy-half,cx+half,cy+half,PLOT_W,PLOT_H);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(12000);
  if(!http.begin(client,url)) return;
  int code=http.GET();
  if(code!=200){ http.end(); Serial.printf("Esri -> HTTP %d\n",code); return; }
  int clen=http.getSize(); size_t total=0;
  WiFiClient* st=http.getStreamPtr();
  uint32_t t0=millis();
  while(http.connected() && (clen<0||(int)total<clen) && total<SAT_MAX){
    size_t avail=st->available();
    if(avail){
      size_t room=SAT_MAX-total; if(avail>room) avail=room;
      int r=st->readBytes(satBuf+total,avail);
      if(r>0){ total+=r; t0=millis(); }
    }else{ if(millis()-t0>4000) break; delay(4); }
  }
  http.end();
  if(total>1000){ satLen=(int)total; satForRange=rangeKm; satReady=true;
    Serial.printf("Esri image: %u bytes (range %d)\n",(unsigned)total,rangeKm); }
}
void satTask(void*){ fetchSatellite(); satFetching=false; vTaskDelete(NULL); }
void startSatFetch(){
  if(satFetching||!satEnabled) return;
  satReady=false;                    // drop any stale image the loop hasn't consumed
  satFetching=true;
  xTaskCreatePinnedToCore(satTask,"sat",12000,NULL,1,NULL,0);   // TLS needs headroom
}

// ---- Route lookup: adsbdb (keyless), single-slot request ----
void fetchRoute(){
  String c(routeReqFlight); c.trim();
  if(c.length()){
    String url="https://api.adsbdb.com/v0/callsign/"+c;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.setTimeout(8000);
    if(http.begin(client,url)){
      int code=http.GET();
      if(code==200){
        StaticJsonDocument<512> filt;
        filt["response"]["flightroute"]["origin"]["iata_code"]=true;
        filt["response"]["flightroute"]["destination"]["iata_code"]=true;
        DynamicJsonDocument doc(2048);
        if(!deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filt))){
          JsonObject fr=doc["response"]["flightroute"];
          strlcpy(routeResOrigin,fr["origin"]["iata_code"]|"",sizeof(routeResOrigin));
          strlcpy(routeResDest,  fr["destination"]["iata_code"]|"",sizeof(routeResDest));
        }
      }
      http.end();
    }
  }
  strlcpy(routeResHex,routeReqHex,sizeof(routeResHex));
  routeResReady=true;                          // ready (may be blank) — marks "tried"
}
void routeTask(void*){ fetchRoute(); routeFetching=false; vTaskDelete(NULL); }
void requestRoute(const char* hex,const char* flight){
  if(routeFetching) return;
  String f(flight); f.trim();
  if(!f.length()) return;
  routeResOrigin[0]=0; routeResDest[0]=0;
  strlcpy(routeReqHex,hex,sizeof(routeReqHex));
  strlcpy(routeReqFlight,f.c_str(),sizeof(routeReqFlight));
  routeFetching=true;
  xTaskCreatePinnedToCore(routeTask,"route",12000,NULL,1,NULL,0);   // TLS needs headroom
}

bool applyPending(){
  ApiPlane local[MAX_TRACKS]; int n=0, hd=0; bool ok=false, pl=true, ready;
  portENTER_CRITICAL(&dataMux);
  ready=pendingReady;
  if(ready){ ok=pendingOk; n=pendingCount; hd=pendingHeard; pl=pendingLocal;
    if(ok) memcpy(local,pendingPlanes,sizeof(ApiPlane)*n);
    pendingReady=false; }
  portEXIT_CRITICAL(&dataMux);
  if(!ready||!ok) return false;
  feedIsLocal=pl;
  heardCount=hd;
  if(!pl) feedMsgRate=-1.0f;                    // feed rate is local-feeder only

  uint32_t now=millis();
  for(int i=0;i<n;i++){
    ApiPlane &p=local[i];
    int slot=-1, freeSlot=-1;
    for(int t=0;t<MAX_TRACKS;t++){
      if(tracks[t].valid&&!strcmp(tracks[t].hex,p.hex)){slot=t;break;}
      if(!tracks[t].valid&&freeSlot<0) freeSlot=t;
    }
    if(slot<0){
      if(freeSlot<0) continue;
      slot=freeSlot;
      memset(&tracks[slot],0,sizeof(Track));
      tracks[slot].valid=true;
      strlcpy(tracks[slot].hex,p.hex,sizeof(tracks[slot].hex));
    }
    Track &t=tracks[slot];
    // callsign changed -> route cache is stale, allow a fresh lookup
    if(p.flight[0]&&strcmp(t.flight,p.flight)){ t.routeTried=false; t.origin[0]=0; t.dest[0]=0; }
    strlcpy(t.flight,p.flight,sizeof(t.flight));
    strlcpy(t.typeCode,p.typeCode,sizeof(t.typeCode));
    strlcpy(t.category,p.category,sizeof(t.category));
    strlcpy(t.squawk,p.squawk,sizeof(t.squawk));
    strlcpy(t.reg,p.reg,sizeof(t.reg));
    // enrichment: keep last-known value if this poll's field is blank
    if(p.desc[0])  strlcpy(t.desc,p.desc,sizeof(t.desc));
    if(p.ownOp[0]) strlcpy(t.ownOp,p.ownOp,sizeof(t.ownOp));
    if(p.year[0])  strlcpy(t.year,p.year,sizeof(t.year));
    t.mil = t.mil || p.mil;
    t.navAltFt=p.navAltFt;
    t.lat=p.lat; t.lon=p.lon; t.gsKt=p.gsKt; t.trackDeg=p.trackDeg;
    t.altFt=p.altFt; t.vRateFpm=p.vRateFpm; t.lastApiMs=now;
  }
  lastGoodApply=now;
  return true;
}

void deadReckon(float dtSec){
  for(int i=0;i<MAX_TRACKS;i++){
    Track &t=tracks[i];
    if(!t.valid) continue;
    if(millis()-t.lastApiMs>DROP_TRACK_MS){
      if(!strcmp(selHex,t.hex)) selHex[0]=0;
      t.valid=false; continue;
    }
    if(t.gsKt<1) continue;
    float dKm=t.gsKt*1.852f/3600.0f*dtSec, rad=deg2rad(t.trackDeg);
    t.lat+=(dKm*cosf(rad))/111.32;
    t.lon+=(dKm*sinf(rad))/(111.32*cos(deg2rad(t.lat)));
  }
}
void recordTrails(){
  for(int i=0;i<MAX_TRACKS;i++){
    Track &t=tracks[i];
    if(!t.valid) continue;
    float sx,sy;
    if(!toScreen(haversineKm(homeLat,homeLon,t.lat,t.lon),
                 bearingTo(homeLat,homeLon,t.lat,t.lon),sx,sy)) continue;
    t.trailX[t.trailHead]=sx; t.trailY[t.trailHead]=sy;
    t.trailHead=(t.trailHead+1)%TRAIL_LEN;
    if(t.trailCount<TRAIL_LEN) t.trailCount++;
  }
}
void clearTrails(){
  for(int i=0;i<MAX_TRACKS;i++){ tracks[i].trailCount=0; tracks[i].trailHead=0; }
}
void rebuildOrder(){
  float dist[MAX_TRACKS];
  orderN=0;
  for(int i=0;i<MAX_TRACKS;i++){
    if(!tracks[i].valid) continue;
    dist[i]=haversineKm(homeLat,homeLon,tracks[i].lat,tracks[i].lon);
    if(dist[i]>(float)rangeKm) continue;
    int j=orderN;
    while(j>0&&dist[orderIdx[j-1]]>dist[i]){orderIdx[j]=orderIdx[j-1];j--;}
    orderIdx[j]=i; orderN++;
  }
}

// ============================================================
//  Radar rendering
// ============================================================
void drawTarget(Track &t){
  float d=haversineKm(homeLat,homeLon,t.lat,t.lon);
  float b=bearingTo(homeLat,homeLon,t.lat,t.lon);
  float ax,ay;
  if(!toScreen(d,b,ax,ay)) return;
  float sx=ax-PLOT_X, sy=ay-PLOT_Y;

  bool sel=(!strcmp(selHex,t.hex));
  bool coasting=(millis()-t.lastApiMs>STALE_TRACK_MS);
  bool emerg=isEmergency(t.squawk);

  uint8_t r,g,bc;
  if(emerg){ if(emergBlink){r=255;g=60;bc=70;} else {r=120;g=36;bc=42;} }  // flash red
  else altRGB(t.altFt,r,g,bc);
  if(coasting){r=(uint8_t)(r*0.55f);g=(uint8_t)(g*0.55f);bc=(uint8_t)(bc*0.55f);}
  uint16_t col=plot.color565(r,g,bc);

  for(int i=0;i<t.trailCount;i++){
    int idx=(t.trailHead-1-i+TRAIL_LEN*2)%TRAIL_LEN;
    float f=0.65f-0.13f*i;
    plot.fillCircle((int)(t.trailX[idx]-PLOT_X),(int)(t.trailY[idx]-PLOT_Y),
                    (i==0)?2:1,plot.color565(r*f,g*f,bc*f));
  }

  // top-down airplane silhouette, rotated to heading (local +y = tail, -y = nose)
  // 0 nose,1 fuseL,2 fuseR,3 tail,4 wingRoot,5 wingTipL,6 wingTipR,7 stabL,8 stabR
  static const float PL[9][2]={
    {0,-8},{-1.6f,3},{1.6f,3},{0,7.5f},{0,-1},{-8,3.5f},{8,3.5f},{-3.6f,7.5f},{3.6f,7.5f}};
  float px[9],py[9];
  for(int i=0;i<9;i++){ float ox,oy; rotPt(PL[i][0],PL[i][1],t.trackDeg,ox,oy); px[i]=sx+ox; py[i]=sy+oy; }
  if(coasting){                      // hollow wireframe when dead-reckoning
    plot.drawLine(px[0],py[0],px[3],py[3],col);   // fuselage
    plot.drawLine(px[5],py[5],px[6],py[6],col);   // main wing
    plot.drawLine(px[7],py[7],px[8],py[8],col);   // tailplane
  }else{
    plot.fillTriangle(px[0],py[0],px[1],py[1],px[2],py[2],col);   // fuselage front
    plot.fillTriangle(px[1],py[1],px[2],py[2],px[3],py[3],col);   // fuselage rear
    plot.fillTriangle(px[4],py[4],px[5],py[5],px[1],py[1],col);   // wing L
    plot.fillTriangle(px[4],py[4],px[6],py[6],px[2],py[2],col);   // wing R
    plot.fillTriangle(px[1],py[1],px[7],py[7],px[3],py[3],col);   // stab L
    plot.fillTriangle(px[2],py[2],px[8],py[8],px[3],py[3],col);   // stab R
  }
  if(t.mil){ plot.drawRect(sx-8,sy-8,16,16,colTxtHi); }   // military / interesting
  if(sel){
    plot.drawEllipse(sx,sy,(int)(13*X_CORR)+1,13,colTxtHi);
    plot.drawEllipse(sx,sy,(int)(15*X_CORR)+1,15,colAccDim);
  }
  if(t.gsKt>20&&!coasting){
    float L=t.gsKt/16.0f; if(L>30)L=30;
    float ex,ey; rotPt(0,-8-L,t.trackDeg,ex,ey);
    plot.drawLine(px[0],py[0],sx+ex,sy+ey,col);      // speed leader from the nose
  }

  if(!showLabels&&!sel) return;

  char line1[12],line2[24];
  strlcpy(line1,t.flight,sizeof(line1));
  { String s(line1); s.trim(); if(!s.length()) s=t.hex; strlcpy(line1,s.c_str(),sizeof(line1)); }
  char altStr[10];
  if(t.altFt>=18000) snprintf(altStr,sizeof(altStr),"FL%03d",t.altFt/100);
  else if(t.altFt>=0) snprintf(altStr,sizeof(altStr),"%dft",t.altFt);
  else snprintf(altStr,sizeof(altStr),"---");
  if(emerg) snprintf(line2,sizeof(line2),"SQ%s %s",t.squawk,altStr);
  else if(coasting) snprintf(line2,sizeof(line2),"COAST %s",altStr);
  else snprintf(line2,sizeof(line2),"%s %dkt",altStr,(int)t.gsKt);

  int w=max(strlen(line1),strlen(line2))*6;
  int bx=(sx>PLOT_W-90)?sx-12-w:sx+12;
  int by=(sy<28)?sy+6:sy-14;

  plot.setTextDatum(top_left);
  plot.setFont(&fonts::Font0);
  plot.setTextColor(emerg?colBad:colTxtHi);
  plot.drawString(line1,bx,by);
  plot.setTextColor(emerg?colBad:col);
  plot.drawString(line2,bx,by+10);

  if(!coasting&&abs(t.vRateFpm)>300){
    int axx=bx+w+4, ayy=by+12;
    if(t.vRateFpm>0) plot.fillTriangle(axx,ayy+5,axx+6,ayy+5,axx+3,ayy,colAcc);
    else plot.fillTriangle(axx,ayy,axx+6,ayy,axx+3,ayy+5,colLowA);
  }
}

void renderPlot(){
  bg.pushSprite(&plot,-PLOT_X,-PLOT_Y);
  // dynamic ring labels (half + full range)
  plot.setTextDatum(top_left);
  plot.setFont(&fonts::Font0);
  plot.setTextColor(colTxtLo);
  int rr2[2]={98,R};
  for(int i=0;i<2;i++){
    int lx=RCX+(int)((rr2[i]+5)*0.707f*X_CORR)-PLOT_X+8;
    int ly=RCY-(int)((rr2[i]+5)*0.707f)-PLOT_Y;
    String lab=(i==0)?String(rangeKm/2):String(rangeKm)+"km";
    plot.drawString(lab,lx,ly);
  }
  rebuildOrder();
  visibleCount=orderN;
  int selDraw=-1;
  for(int i=0;i<orderN;i++){
    Track &tt=tracks[orderIdx[i]];
    if(!strcmp(selHex,tt.hex)){ selDraw=orderIdx[i]; continue; }
    drawTarget(tt);
  }
  if(selDraw>=0) drawTarget(tracks[selDraw]);
  if(orderN==0){
    plot.setTextDatum(middle_center);
    plot.setFont(&fonts::FreeSans9pt7b);
    plot.setTextColor(colTxtLo);
    plot.drawString("NO AIRCRAFT IN RANGE",RCX-PLOT_X,RCY-PLOT_Y+40);
  }
  plot.pushSprite(PLOT_X,PLOT_Y);
}

// ============================================================
//  Card field drawers
// ============================================================
void drawOverview(){
  rebuildOrder();
  int inRange=orderN; visibleCount=inRange;

  restore(OV_X+8,OV_Y+52,OV_W-16,234);
  bool link=(WiFi.status()==WL_CONNECTED);
  bool stale=(millis()-lastGoodApply>STALE_FEED_MS)||lastGoodApply==0;

  // hero: aircraft in range + total heard
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colAcc);
  lcd.drawString(String(inRange),OV_X+16,OV_Y+54);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(colTxtLo);
  lcd.drawString("IN RANGE",OV_X+72,OV_Y+62);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtMd);
  char hb[24]; snprintf(hb,sizeof(hb),"of %d heard",heardCount);
  lcd.drawString(hb,OV_X+16,OV_Y+88);

  // emergency line (only when a 7500/7600/7700 is in range)
  int em=-1;
  for(int i=0;i<orderN;i++){ if(isEmergency(tracks[orderIdx[i]].squawk)){em=orderIdx[i];break;} }
  if(em>=0){
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(emergBlink?colBad:lcd.color565(150,60,66));
    String ec(tracks[em].flight); ec.trim(); if(!ec.length()) ec=tracks[em].hex;
    char eb[28]; snprintf(eb,sizeof(eb),"! %s  %s",tracks[em].squawk,ec.substring(0,7).c_str());
    lcd.drawString(eb,OV_X+14,OV_Y+112);
  }
  lcd.drawFastHLine(OV_X+12,OV_Y+134,OV_W-24,colBorder);

  // nearest target
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtLo);
  lcd.drawString("NEAREST",OV_X+14,OV_Y+142);
  lcd.setTextDatum(top_right);
  if(inRange>0){
    Track &nt=tracks[orderIdx[0]];
    String nc(nt.flight); nc.trim(); if(!nc.length()) nc=nt.hex;
    lcd.setTextColor(colTxtHi);
    lcd.drawString(nc.substring(0,8),OV_X+OV_W-14,OV_Y+142);
    float nd=haversineKm(homeLat,homeLon,nt.lat,nt.lon);
    float nbg=bearingTo(homeLat,homeLon,nt.lat,nt.lon);
    char nbuf[24]; snprintf(nbuf,sizeof(nbuf),"%.1f km %s",nd,cardinal8(nbg));
    lcd.setTextColor(colAcc);
    lcd.drawString(nbuf,OV_X+OV_W-14,OV_Y+162);
  }else{
    lcd.setTextColor(colTxtLo);
    lcd.drawString("--",OV_X+OV_W-14,OV_Y+142);
  }

  // feed rate (local feeder only)
  lcd.setTextDatum(top_left);  lcd.setTextColor(colTxtLo);
  lcd.drawString("FEED",OV_X+14,OV_Y+186);
  lcd.setTextDatum(top_right);
  char fb[16];
  if(feedIsLocal&&feedMsgRate>=0) snprintf(fb,sizeof(fb),"%d/s",(int)(feedMsgRate+0.5f));
  else snprintf(fb,sizeof(fb),"--");
  lcd.setTextColor(colTxtHi);
  lcd.drawString(fb,OV_X+OV_W-14,OV_Y+186);

  lcd.setTextDatum(top_left);
  lcd.drawFastHLine(OV_X+12,OV_Y+210,OV_W-24,colBorder);

  // source + updated age
  lcd.setTextColor(colTxtLo);
  lcd.drawString("SOURCE",OV_X+14,OV_Y+218);
  lcd.setTextDatum(top_right);
  if(!link){ lcd.setTextColor(colBad); lcd.drawString("OFFLINE",OV_X+OV_W-14,OV_Y+218); }
  else if(feedIsLocal){ lcd.setTextColor(colAcc); lcd.drawString(LOCAL_FEED_NAME,OV_X+OV_W-14,OV_Y+218); }
  else{ lcd.setTextColor(colTxtMd); lcd.drawString("CLOUD",OV_X+OV_W-14,OV_Y+218); }

  lcd.setTextDatum(top_left);  lcd.setTextColor(colTxtLo);
  lcd.drawString("UPDATED",OV_X+14,OV_Y+242);
  lcd.setTextDatum(top_right);
  char ub[16]; uint16_t uc=colTxtHi;
  if(!link){ snprintf(ub,sizeof(ub),"NO LINK"); uc=colBad; }
  else if(stale){ snprintf(ub,sizeof(ub),"STALE"); uc=colLowA; }
  else{ uint32_t age=(millis()-lastGoodApply)/1000; snprintf(ub,sizeof(ub),"%lus",(unsigned long)age); }
  lcd.setTextColor(uc);
  lcd.drawString(ub,OV_X+OV_W-14,OV_Y+242);
  lcd.setTextDatum(top_left);
}

void drawRangePill(){
  restore(350,448,100,20);
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtMd);
  char b[16]; snprintf(b,sizeof(b),"< %d KM >",rangeKm);
  lcd.drawString(b,400,458);
  lcd.setTextDatum(top_left);
}

void drawSelectedCard(){
  restore(SC_X+6,SC_Y+44,SC_W-12,SC_H-50);
  Track* t=findByHex(selHex);
  lcd.setTextDatum(top_left);

  if(!t){
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(colTxtLo);
    lcd.setTextDatum(top_center);
    lcd.drawString("Tap a target or",SC_X+SC_W/2,SC_Y+170);
    lcd.drawString("use the arrows",SC_X+SC_W/2,SC_Y+194);
    lcd.setTextDatum(top_left);
    return;
  }

  String call(t->flight); call.trim(); if(!call.length()) call=String(t->hex);
  bool emerg=isEmergency(t->squawk);
  bool coasting=(millis()-t->lastApiMs>STALE_TRACK_MS);
  float d=haversineKm(homeLat,homeLon,t->lat,t->lon);

  // callsign
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(emerg?colBad:colTxtHi);
  lcd.drawString(call.substring(0,8),SC_X+14,SC_Y+46);

  // identity block (Tier B, drawn only when present — no blank rows)
  lcd.setFont(&fonts::FreeSans9pt7b);
  if(t->ownOp[0]){ lcd.setTextColor(colAcc);
    lcd.drawString(String(t->ownOp).substring(0,17),SC_X+14,SC_Y+78); }
  if(t->origin[0]&&t->dest[0]){
    char rt[16]; snprintf(rt,sizeof(rt),"%s > %s",t->origin,t->dest);
    lcd.setTextColor(colTxtHi); lcd.drawString(rt,SC_X+14,SC_Y+98); }
  { String frame(t->desc); frame.trim();
    if(!frame.length()){ frame=String(t->typeCode); frame.trim();
      if(!frame.length()) frame=categoryName(t->category); }
    lcd.setTextColor(colTxtMd);
    lcd.drawString(frame.substring(0,17),SC_X+14,SC_Y+116); }
  // tail . type . year (. MIL) — small
  { char idl[32]; String tail(t->reg); tail.trim();
    snprintf(idl,sizeof(idl),"%s  %s%s%s",
      tail.length()?tail.c_str():"----", t->typeCode[0]?t->typeCode:"",
      t->year[0]?"  ":"", t->year[0]?t->year:"");
    lcd.setFont(&fonts::Font0); lcd.setTextColor(colTxtLo);
    lcd.drawString(idl,SC_X+14,SC_Y+136);
    if(t->mil){ lcd.setTextColor(colAcc); lcd.setTextDatum(top_right);
      lcd.drawString("MIL",SC_X+SC_W-14,SC_Y+136); lcd.setTextDatum(top_left); } }

  lcd.drawFastHLine(SC_X+12,SC_Y+152,SC_W-24,colBorder);

  // number rows
  struct Row{const char* k; char v[16]; uint16_t c;};
  Row rows[6];
  uint8_t rr,gg,bb; altRGB(t->altFt,rr,gg,bb);
  char altStr[12];
  if(t->altFt>=18000) snprintf(altStr,sizeof(altStr),"FL%03d",t->altFt/100);
  else if(t->altFt>=0) snprintf(altStr,sizeof(altStr),"%dft",t->altFt);
  else snprintf(altStr,sizeof(altStr),"---");
  if(t->navAltFt>0) snprintf(rows[0].v,16,"%s>%d",altStr,t->navAltFt/100);
  else snprintf(rows[0].v,16,"%s",altStr);
  rows[0].k="ALT"; rows[0].c=lcd.color565(rr,gg,bb);
  snprintf(rows[1].v,16,"%d kt",(int)t->gsKt); rows[1].k="SPD"; rows[1].c=colTxtHi;
  snprintf(rows[2].v,16,"%d %s",(int)t->trackDeg,cardinal8(t->trackDeg));
  rows[2].k="HDG"; rows[2].c=colTxtHi;
  snprintf(rows[3].v,16,"%+d",t->vRateFpm); rows[3].k="V/S";
  rows[3].c=(t->vRateFpm>300)?colAcc:(t->vRateFpm<-300?colLowA:colTxtHi);
  snprintf(rows[4].v,16,"%.1f km %s",d,cardinal8(bearingTo(homeLat,homeLon,t->lat,t->lon)));
  rows[4].k="DIST"; rows[4].c=colTxtHi;
  snprintf(rows[5].v,16,"%s",t->squawk[0]?t->squawk:"----");
  rows[5].k="SQK"; rows[5].c=emerg?colBad:colTxtMd;

  lcd.setFont(&fonts::FreeSans9pt7b);
  for(int i=0;i<6;i++){
    int y=SC_Y+162+i*26;
    lcd.setTextDatum(top_left);
    lcd.setTextColor(colTxtLo);  lcd.drawString(rows[i].k,SC_X+14,y);
    lcd.setTextDatum(top_right);
    lcd.setTextColor(rows[i].c); lcd.drawString(rows[i].v,SC_X+SC_W-14,y);
  }

  // live / coast status
  uint16_t sc=coasting?colLowA:colAcc;
  uint32_t age=(millis()-t->lastApiMs)/1000;
  lcd.fillCircle(SC_X+18,SC_Y+326,4,sc);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(sc);
  lcd.setTextDatum(middle_left);
  char sb[16]; snprintf(sb,sizeof(sb),"%s  %lus",coasting?"COAST":"LIVE",(unsigned long)age);
  lcd.drawString(sb,SC_X+28,SC_Y+326);
  lcd.setTextDatum(top_left);
}

void drawTimeCard(bool force){
  struct tm ti;
  if(!getLocalTime(&ti,50)) return;
  restore(TM_X+8,TM_Y+14,TM_W-16,36);

#if CLOCK_12H
  int h=ti.tm_hour%12; if(h==0)h=12;
  char ts[12]; snprintf(ts,sizeof(ts),"%d:%02d:%02d",h,ti.tm_min,ti.tm_sec);
#else
  char ts[12]; snprintf(ts,sizeof(ts),"%02d:%02d:%02d",ti.tm_hour,ti.tm_min,ti.tm_sec);
#endif
  lcd.setTextDatum(top_center);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString(ts,TM_X+TM_W/2,TM_Y+16);

  if(force||ti.tm_min!=lastSubMin){
    lastSubMin=ti.tm_min;
    restore(TM_X+8,TM_Y+60,TM_W-16,32);
    char datebuf[16]; strftime(datebuf,sizeof(datebuf),"%a %b %d",&ti);
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextDatum(top_left);
    int dW=lcd.textWidth(datebuf);
#if CLOCK_12H
    const char* ap=(ti.tm_hour<12)?"AM":"PM";
    int apW=lcd.textWidth(ap);
    int sx=TM_X+(TM_W-(apW+10+dW))/2;
    lcd.setTextColor(colAcc);
    lcd.drawString(ap,sx,TM_Y+66);
    sx+=apW+10;
#else
    int sx=TM_X+(TM_W-dW)/2;
#endif
    lcd.setTextColor(colTxtLo);
    lcd.drawString(datebuf,sx,TM_Y+66);
  }
}

void drawMainScreen(){
  bg.pushSprite(0,0);
  drawOverview();
  drawSelectedCard();
  lastSubMin=-1;
  drawTimeCard(true);
  drawRangePill();
  renderPlot();
}

// ============================================================
//  Sub-screens: WiFi provisioning
// ============================================================
void doScan(){
  gradientBg();
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Scanning for networks...",400,240);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n=WiFi.scanNetworks();
  scanCount=0;
  for(int i=0;i<n&&scanCount<7;i++){
    String s=WiFi.SSID(i);
    if(!s.length()) continue;
    bool dup=false;
    for(int j=0;j<scanCount;j++) if(scanSsid[j]==s){dup=true;break;}
    if(dup) continue;
    scanSsid[scanCount]=s; scanRssi[scanCount]=WiFi.RSSI(i); scanCount++;
  }
  WiFi.scanDelete();
}

void drawScanScreen(){
  gradientBg();
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Select Wi-Fi network",40,22);
  for(int i=0;i<scanCount;i++){
    char label[48];
    snprintf(label,sizeof(label),"%s   (%d dBm)",scanSsid[i].substring(0,22).c_str(),scanRssi[i]);
    btn(40,80+i*54,540,46,label);
  }
  if(scanCount==0){
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(colLowA);
    lcd.drawString("No networks found",44,100);
  }
  btn(620,80,160,46,"RESCAN",true);
  if(wifiSsid.length()) btn(620,140,160,46,"CANCEL");
}

const char* KB_L[3]={"qwertyuiop","asdfghjkl","zxcvbnm"};
const char* KB_S[3]={"1234567890","!@#$%^&*()","-_=+/;:'\""};
const int KB_Y[4]={150,222,294,366};
const int KB_H=64;

void kbKeyRect(int row,int col,int nInRow,int &x,int &y,int &w){
  int totalW=nInRow*76-4;
  x=(800-totalW)/2+col*76; y=KB_Y[row]; w=72;
}
void kbKeyDraw(int x,int y,int w,const char* label,bool special){
  lcd.fillRoundRect(x,y,w,KB_H-8,10,special?colKeyAcc:colKey);
  lcd.drawRoundRect(x,y,w,KB_H-8,10,colBorder);
  lcd.setTextDatum(middle_center);
  lcd.setFont(strlen(label)>2?&fonts::FreeSansBold9pt7b:&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString(label,x+w/2,y+(KB_H-8)/2);
}
void drawKeyboard(){
  lcd.fillRect(0,KB_Y[0]-8,800,480-(KB_Y[0]-8),lcd.color565(BG2[0],BG2[1],BG2[2]));
  bool sym=(kbLayer==2);
  for(int r2=0;r2<3;r2++){
    const char* rowStr=sym?KB_S[r2]:KB_L[r2];
    int n=strlen(rowStr), extra=(r2==2)?2:0;
    int x,y,w;
    if(r2==2){ kbKeyRect(2,0,n+extra,x,y,w); kbKeyDraw(x,y,w,sym?" ":"SH",true); }
    for(int c=0;c<n;c++){
      kbKeyRect(r2,c+(r2==2?1:0),n+extra,x,y,w);
      char ch=rowStr[c];
      if(!sym&&kbLayer==1) ch=toupper(ch);
      char s[2]={ch,0};
      kbKeyDraw(x,y,w,s,false);
    }
    if(r2==2){ kbKeyRect(2,n+1,n+extra,x,y,w); kbKeyDraw(x,y,w,"<",true); }
  }
  kbKeyDraw( 38,KB_Y[3],110,kbLayer==2?"ABC":"?123",true);
  kbKeyDraw(156,KB_Y[3], 72,",",false);
  kbKeyDraw(236,KB_Y[3],320,"SPACE",false);
  kbKeyDraw(564,KB_Y[3], 72,".",false);
  lcd.fillRoundRect(644,KB_Y[3],118,KB_H-8,10,colAcc);
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(lcd.color565(8,14,20));
  lcd.drawString("OK",644+59,KB_Y[3]+(KB_H-8)/2);
}
void drawPassField(){
  lcd.fillRect(40,72,500,48,lcd.color565(BG1[0],BG1[1],BG1[2]));
  lcd.drawRoundRect(40,72,500,48,10,colBorder);
  String shown;
  if(kbMask){ for(size_t i=0;i<kbText.length();i++) shown+='*'; }
  else shown=kbText;
  if((int)shown.length()>34) shown=shown.substring(shown.length()-34);
  lcd.setTextDatum(middle_left);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString(shown,54,96);
}
void drawPassScreen(){
  gradientBg();
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Password for "+pendSsid.substring(0,22),40,26);
  drawPassField();
  btn(552,72,100,48,kbMask?"SHOW":"HIDE");
  btn(660,72,120,48,"CANCEL");
  drawKeyboard();
}

void applyNetConfig(){
  if(!netStatic) return;
  IPAddress ip,gw,mk,dn;
  if(!ip.fromString(netIp)||!gw.fromString(netGw)||!mk.fromString(netMask)) return;
  if(!netDns.length()||!dn.fromString(netDns)) dn=gw;
  WiFi.config(ip,gw,mk,dn);
}

void tryConnect(){
  gradientBg();
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Connecting to "+pendSsid+"...",400,240);
  WiFi.mode(WIFI_STA);
  applyNetConfig();
  WiFi.begin(pendSsid.c_str(),kbText.c_str());
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t0<14000) delay(200);
  if(WiFi.status()==WL_CONNECTED){
    wifiSsid=pendSsid; wifiPass=kbText;
    prefs.putString("ssid",wifiSsid);
    prefs.putString("pass",wifiPass);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    configTzTime(TZ_STRING,"pool.ntp.org","time.nist.gov");
    startServer();
    screen=SCR_MAIN; drawMainScreen(); startFetch();
  }else{
    lcd.fillRect(0,200,800,80,lcd.color565(BG1[0],BG1[1],BG1[2]));
    lcd.setTextColor(colBad);
    lcd.drawString("Connection failed - check password",400,240);
    delay(1800);
    screen=SCR_WIFI_PASS; drawPassScreen();
  }
}

// ============================================================
//  Coordinate editor
// ============================================================
void drawCoordField(int which){
  int x=(which==0)?60:330, y=140;
  bool act=(coordField==which);
  lcd.fillRect(x,y,240,52,lcd.color565(BG1[0],BG1[1],BG1[2]));
  lcd.drawRoundRect(x,y,240,52,10,act?colAcc:colBorder);
  lcd.setTextDatum(middle_left);
  lcd.setFont(&fonts::FreeSans9pt7b);
  String &v=(which==0)?coordLat:coordLon;
  lcd.setTextColor(v.length()?colTxtHi:colTxtLo);
  lcd.drawString(v.length()?v:String(which==0?"Latitude":"Longitude"),x+14,y+26);
}
void drawCoordScreen(){
  gradientBg();
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Edit coordinates",60,44);
  drawCoordField(0); drawCoordField(1);
  btn(60,340,180,56,"SAVE",true);
  btn(260,340,180,56,"CANCEL");
  const char* keys[4][4]={{"7","8","9","<"},{"4","5","6","+/-"},{"1","2","3","."},{"0","","",""}};
  for(int r2=0;r2<4;r2++)
    for(int c=0;c<4;c++){
      if(!keys[r2][c][0]) continue;
      int w=(r2==3&&c==0)?286:90;
      kbKeyDraw(520+c*98,120+r2*70,w,keys[r2][c],r2==0&&c==3);
    }
}
void coordKey(const char* k){
  String &v=(coordField==0)?coordLat:coordLon;
  if(!strcmp(k,"<")){ if(v.length()) v.remove(v.length()-1); }
  else if(!strcmp(k,"+/-")){ if(v.startsWith("-")) v.remove(0,1); else v="-"+v; }
  else if(!strcmp(k,".")){ if(v.indexOf('.')<0) v+='.'; }
  else if(v.length()<11) v+=k;
  drawCoordField(coordField);
}
void saveCoords(){
  double la=coordLat.toDouble(), lo=coordLon.toDouble();
  if(la<-90||la>90||lo<-180||lo>180||!coordLat.length()||!coordLon.length()){
    lcd.setTextDatum(top_left);
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(colBad);
    lcd.fillRect(60,300,400,24,lcd.color565(BG1[0],BG1[1],BG1[2]));
    lcd.drawString("Invalid range",60,300);
    return;
  }
  homeLat=la; homeLon=lo;
  prefs.putDouble("lat",homeLat); prefs.putDouble("lon",homeLon);
  for(int i=0;i<MAX_TRACKS;i++) tracks[i].valid=false;
  selHex[0]=0;
  screen=SCR_MAIN; drawMainScreen(); startFetch(); startSatFetch();
}

// ============================================================
//  Network settings screen
// ============================================================
const char* NET_LBL[4]={"IP ADDRESS","GATEWAY","SUBNET MASK","DNS"};

void netFieldRect(int i,int &x,int &y){ x=(i%2)?287:27; y=(i/2)?250:170; }

void drawNetField(int i){
  int x,y; netFieldRect(i,x,y);
  bool act=(netField==i)&&editStatic;
  lcd.fillRect(x,y,240,48,lcd.color565(BG1[0],BG1[1],BG1[2]));
  lcd.drawRoundRect(x,y,240,48,10,act?colAcc:(editStatic?colBorder:lcd.color565(34,44,56)));
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(colTxtLo);
  lcd.drawString(NET_LBL[i],x+2,y-12);
  lcd.setTextDatum(middle_left);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(editStatic?colTxtHi:colTxtLo);
  lcd.drawString(netVals[i],x+14,y+24);
}

void drawNetModeButtons(){
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.fillRoundRect(27,92,140,44,10,!editStatic?colAcc:colKey);
  lcd.drawRoundRect(27,92,140,44,10,!editStatic?colAcc:colBorder);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(!editStatic?lcd.color565(8,14,20):colTxtHi);
  lcd.drawString("DHCP",97,114);
  lcd.fillRoundRect(175,92,140,44,10,editStatic?colAcc:colKey);
  lcd.drawRoundRect(175,92,140,44,10,editStatic?colAcc:colBorder);
  lcd.setTextColor(editStatic?lcd.color565(8,14,20):colTxtHi);
  lcd.drawString("STATIC",245,114);
}

void drawNetScreen(){
  gradientBg();
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Network",27,20);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtMd);
  String info=wifiSsid.length()?wifiSsid:"(no network)";
  if(WiFi.status()==WL_CONNECTED) info+="   "+WiFi.localIP().toString();
  lcd.drawString(info,29,58);

  drawNetModeButtons();
  for(int i=0;i<4;i++) drawNetField(i);

  btn(27,340,190,56,"SAVE",true);
  btn(237,340,190,56,"CANCEL");
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(colTxtLo);
  lcd.drawString("STATIC SETTINGS APPLY AFTER AUTOMATIC REBOOT. BLANK DNS = GATEWAY.",27,412);

  const char* keys[4][3]={{"7","8","9"},{"4","5","6"},{"1","2","3"},{"0",".","<"}};
  for(int r2=0;r2<4;r2++)
    for(int c=0;c<3;c++)
      kbKeyDraw(547+c*78,120+r2*64,70,keys[r2][c],r2==3&&c==2);
}

void netKey(const char* k){
  if(!editStatic) return;
  String &v=netVals[netField];
  if(!strcmp(k,"<")){ if(v.length()) v.remove(v.length()-1); }
  else if(!strcmp(k,".")){ if(v.length()&&v[v.length()-1]!='.') v+='.'; }
  else if(v.length()<15) v+=k;
  drawNetField(netField);
}

void netReboot(const char* msg){
  gradientBg();
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString(msg,400,240);
  delay(900);
  ESP.restart();
}

void saveNet(){
  if(!editStatic){
    bool was=netStatic;
    netStatic=false;
    prefs.putBool("nstat",false);
    if(was) netReboot("Switching to DHCP...");
    screen=SCR_MAIN; drawMainScreen();
    return;
  }
  IPAddress ip,gw,mk,dn;
  bool ok=ip.fromString(netVals[0])&&gw.fromString(netVals[1])&&mk.fromString(netVals[2]);
  if(ok&&netVals[3].length()) ok=dn.fromString(netVals[3]);
  if(!ok){
    lcd.setTextDatum(top_left);
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(colBad);
    lcd.fillRect(27,308,400,24,lcd.color565(BG1[0],BG1[1],BG1[2]));
    lcd.drawString("Invalid address",29,310);
    return;
  }
  netStatic=true;
  netIp=netVals[0]; netGw=netVals[1]; netMask=netVals[2]; netDns=netVals[3];
  prefs.putBool("nstat",true);
  prefs.putString("nip",netIp);   prefs.putString("ngw",netGw);
  prefs.putString("nmask",netMask); prefs.putString("ndns",netDns);
  netReboot("Applying network settings...");
}

void openNetScreen(){
  editStatic=netStatic;
  netField=0;
  if(netStatic){
    netVals[0]=netIp; netVals[1]=netGw; netVals[2]=netMask; netVals[3]=netDns;
  }else if(WiFi.status()==WL_CONNECTED){
    netVals[0]=WiFi.localIP().toString();
    netVals[1]=WiFi.gatewayIP().toString();
    netVals[2]=WiFi.subnetMask().toString();
    netVals[3]=WiFi.dnsIP().toString();
  }else{
    netVals[0]=""; netVals[1]=""; netVals[2]="255.255.255.0"; netVals[3]="";
  }
  screen=SCR_NET;
  drawNetScreen();
}

// ============================================================
//  Menu screen
// ============================================================
void drawMenuScreen(){
  gradientBg();
  lcd.fillRoundRect(180,50,440,380,16,lcd.color565(20,28,38));
  lcd.drawRoundRect(180,50,440,380,16,colBorder);
  lcd.drawFastHLine(196,51,408,colBorderHi);
  btn(740,16,44,40,"X");
  btn(240,84,320,50,"CHANGE WI-FI");
  btn(240,148,320,50,"NETWORK");
  btn(240,212,320,50,"EDIT COORDINATES");
  btn(240,276,320,50,showLabels?"LABELS: ON":"LABELS: OFF");
  btn(240,340,320,50,"REBOOT");
  lcd.setTextDatum(top_center);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(colTxtMd);
  String ipl=(WiFi.status()==WL_CONNECTED)?
    ("Browser config: http://"+WiFi.localIP().toString()+"/  (http://" MDNS_NAME ".local/)"):
    String("Not connected");
  lcd.drawString(ipl,400,392);
  lcd.setTextColor(colTxtLo);
  lcd.drawString("AirRadar " APP_VERSION "   " REPO_URL,400,406);
  lcd.drawString(AUTHOR_LINE,400,418);
}

// ============================================================
//  Web config server
// ============================================================
void handleRoot(){
  String pIp=netIp, pGw=netGw, pMk=netMask, pDn=netDns;
  if(!netStatic&&WiFi.status()==WL_CONNECTED){
    pIp=WiFi.localIP().toString(); pGw=WiFi.gatewayIP().toString();
    pMk=WiFi.subnetMask().toString(); pDn=WiFi.dnsIP().toString();
  }
  String h=F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>AirRadar</title><style>body{font-family:system-ui;background:#0b0f15;color:#dfe8f2;max-width:420px;"
    "margin:2em auto;padding:0 1em}input,select{width:100%;padding:9px;margin:4px 0 14px;background:#151d28;"
    "color:#dfe8f2;border:1px solid #33455c;border-radius:8px}button{padding:10px 20px;background:#4cc2ff;"
    "color:#08131c;border:0;border-radius:8px;margin-right:8px;font-weight:600}.d{background:#e05555;color:#fff}"
    "h3{margin:26px 0 6px;color:#8fa3b8}</style></head><body><h2>AirRadar settings</h2>"
    "<form method=post action=/save>Latitude<input name=lat value='");
  h+=String(homeLat,6);
  h+=F("'>Longitude<input name=lon value='");
  h+=String(homeLon,6);
  h+=F("'>Feeder URL<input name=feed value='");
  h+=feedUrl;
  h+=F("'>Labels <select name=lbl><option value=1");
  if(showLabels) h+=F(" selected");
  h+=F(">On</option><option value=0");
  if(!showLabels) h+=F(" selected");
  h+=F(">Off</option></select><br><br><button type=submit>Save</button></form>"
    "<h3>Wi-Fi</h3>"
    "<form method=post action=/wifi onsubmit='return confirm(\"Save Wi-Fi and reboot?\")'>"
    "SSID<input name=ssid value='");
  h+=wifiSsid;
  h+=F("'>Password<input type=password name=pass>"
    "<button type=submit>Save &amp; reboot</button></form>"
    "<h3>Network</h3>"
    "<form method=post action=/net onsubmit='return confirm(\"Apply network settings and reboot?\")'>"
    "Mode <select name=mode><option value=dhcp");
  if(!netStatic) h+=F(" selected");
  h+=F(">DHCP</option><option value=static");
  if(netStatic) h+=F(" selected");
  h+=F(">Static</option></select>IP address<input name=nip value='");
  h+=pIp;
  h+=F("'>Gateway<input name=ngw value='");
  h+=pGw;
  h+=F("'>Subnet mask<input name=nmask value='");
  h+=pMk;
  h+=F("'>DNS (blank = gateway)<input name=ndns value='");
  h+=pDn;
  h+=F("'><button type=submit>Save &amp; reboot</button></form><br>"
    "<form method=post action=/forget onsubmit='return confirm(\"Forget WiFi and reboot?\")'>"
    "<button class=d>Forget Wi-Fi</button></form>"
    "<p style='margin-top:28px;color:#5f7488;font-size:.85em'>AirRadar " APP_VERSION
    " &middot; <a style='color:#8fa3b8' href='https://" REPO_URL "'>" REPO_URL "</a><br>"
    AUTHOR_LINE "</p>"
    "</body></html>");
  server.send(200,"text/html",h);
}
void handleSave(){
  if(server.hasArg("lat")){ double v=server.arg("lat").toDouble(); if(v>=-90&&v<=90) homeLat=v; }
  if(server.hasArg("lon")){ double v=server.arg("lon").toDouble(); if(v>=-180&&v<=180) homeLon=v; }
  if(server.hasArg("feed")){ String v=server.arg("feed"); v.trim();
    if(v.startsWith("http://")||v.startsWith("https://")){ feedUrl=v; prefs.putString("feed",feedUrl); } }
  if(server.hasArg("lbl")) showLabels=(server.arg("lbl")=="1");
  prefs.putDouble("lat",homeLat); prefs.putDouble("lon",homeLon);
  prefs.putBool("lbl",showLabels);
  for(int i=0;i<MAX_TRACKS;i++) tracks[i].valid=false;
  selHex[0]=0;
  server.sendHeader("Location","/"); server.send(303);
  if(screen==SCR_MAIN) drawMainScreen();
  startFetch(); startSatFetch();
}
void handleForget(){
  server.send(200,"text/plain","Forgetting WiFi, rebooting...");
  prefs.remove("ssid"); prefs.remove("pass");
  delay(400); ESP.restart();
}
void webReboot(String msg){
  String h=F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;background:#0b0f15;color:#dfe8f2;padding:2em'>");
  h+=msg;
  h+=F("</body>");
  server.send(200,"text/html",h);
  delay(500);
  ESP.restart();
}

void handleWifi(){
  String s=server.arg("ssid"); s.trim();
  if(!s.length()){ server.send(400,"text/plain","SSID required"); return; }
  wifiSsid=s; wifiPass=server.arg("pass");
  prefs.putString("ssid",wifiSsid);
  prefs.putString("pass",wifiPass);
  webReboot("Joining "+s+" - rebooting. Reconnect at the IP shown on the display.");
}

void handleNet(){
  if(server.arg("mode")!="static"){
    bool was=netStatic;
    netStatic=false;
    prefs.putBool("nstat",false);
    if(was){ webReboot("Switching to DHCP - rebooting. Reconnect at the IP shown on the display."); return; }
    server.sendHeader("Location","/"); server.send(303);
    return;
  }
  String vip=server.arg("nip"), vgw=server.arg("ngw");
  String vmk=server.arg("nmask"), vdn=server.arg("ndns");
  vip.trim(); vgw.trim(); vmk.trim(); vdn.trim();
  IPAddress ip,gw,mk,dn;
  bool ok=ip.fromString(vip)&&gw.fromString(vgw)&&mk.fromString(vmk);
  if(ok&&vdn.length()) ok=dn.fromString(vdn);
  if(!ok){ server.send(400,"text/plain","Invalid address - go back and check the fields."); return; }
  netStatic=true;
  netIp=vip; netGw=vgw; netMask=vmk; netDns=vdn;
  prefs.putBool("nstat",true);
  prefs.putString("nip",netIp);     prefs.putString("ngw",netGw);
  prefs.putString("nmask",netMask); prefs.putString("ndns",netDns);
  webReboot("Applying network settings - reconnect at http://"+vip+"/");
}

void startServer(){
  if(serverUp) return;
  server.on("/",HTTP_GET,handleRoot);
  server.on("/save",HTTP_POST,handleSave);
  server.on("/wifi",HTTP_POST,handleWifi);
  server.on("/net",HTTP_POST,handleNet);
  server.on("/forget",HTTP_POST,handleForget);
  server.begin();
  MDNS.begin(MDNS_NAME);
  serverUp=true;
}

// ============================================================
//  Touch dispatch
// ============================================================
void selectByOrder(int dir){
  rebuildOrder();
  if(!orderN){ selHex[0]=0; return; }
  int cur=-1;
  for(int i=0;i<orderN;i++)
    if(!strcmp(tracks[orderIdx[i]].hex,selHex)){cur=i;break;}
  cur=(cur<0)?(dir>0?0:orderN-1):(cur+dir+orderN)%orderN;
  strlcpy(selHex,tracks[orderIdx[cur]].hex,sizeof(selHex));
}

void cycleRange(){
  if(rangeKm==50) rangeKm=100;
  else if(rangeKm==100) rangeKm=150;
  else if(rangeKm==150) rangeKm=250;
  else rangeKm=50;
  prefs.putInt("rng",rangeKm);
  clearTrails();
  drawRangePill();
  drawSelectedCard();
  drawOverview();
  renderPlot();
  startFetch();
  startSatFetch();          // re-frame the satellite base to the new range
}

void mainTouch(int tx,int ty){
  if(hit(tx,ty,SET_X,SET_Y,SET_W,SET_H)){ screen=SCR_MENU; drawMenuScreen(); return; }
  if(hitC(tx,ty,ARROW_L_X,ARROW_Y,ARROW_R+6)){
    selectByOrder(-1); drawSelectedCard(); renderPlot(); return; }
  if(hitC(tx,ty,ARROW_R_X,ARROW_Y,ARROW_R+6)){
    selectByOrder(+1); drawSelectedCard(); renderPlot(); return; }
  if(hit(tx,ty,340,440,120,36)){ cycleRange(); return; }
  if(hit(tx,ty,PLOT_X,PLOT_Y,PLOT_W,PLOT_H)){
    float bestD=26*26; int best=-1;
    for(int i=0;i<MAX_TRACKS;i++){
      if(!tracks[i].valid) continue;
      float sx,sy;
      if(!toScreen(haversineKm(homeLat,homeLon,tracks[i].lat,tracks[i].lon),
                   bearingTo(homeLat,homeLon,tracks[i].lat,tracks[i].lon),sx,sy)) continue;
      float dx=sx-tx, dy=sy-ty, dd=dx*dx+dy*dy;
      if(dd<bestD){bestD=dd;best=i;}
    }
    if(best>=0) strlcpy(selHex,tracks[best].hex,sizeof(selHex));
    else selHex[0]=0;
    drawSelectedCard(); renderPlot();
  }
}

void scanTouch(int tx,int ty){
  for(int i=0;i<scanCount;i++)
    if(hit(tx,ty,40,80+i*54,540,46)){
      pendSsid=scanSsid[i]; kbText=""; kbLayer=0; kbMask=true;
      screen=SCR_WIFI_PASS; drawPassScreen(); return;
    }
  if(hit(tx,ty,620,80,160,46)){ doScan(); drawScanScreen(); return; }
  if(wifiSsid.length()&&hit(tx,ty,620,140,160,46)){ screen=SCR_MAIN; drawMainScreen(); }
}

void passTouch(int tx,int ty){
  if(hit(tx,ty,552,72,100,48)){ kbMask=!kbMask; drawPassScreen(); return; }
  if(hit(tx,ty,660,72,120,48)){ screen=SCR_WIFI_SCAN; drawScanScreen(); return; }
  bool sym=(kbLayer==2);
  for(int r2=0;r2<3;r2++){
    const char* rowStr=sym?KB_S[r2]:KB_L[r2];
    int n=strlen(rowStr), extra=(r2==2)?2:0;
    for(int c=0;c<n+extra;c++){
      int x,y,w; kbKeyRect(r2,c,n+extra,x,y,w);
      if(!hit(tx,ty,x,y,w,KB_H-8)) continue;
      if(r2==2&&c==0){ if(!sym) kbLayer=(kbLayer==1)?0:1; drawKeyboard(); return; }
      if(r2==2&&c==n+1){
        if(kbText.length()) kbText.remove(kbText.length()-1);
        drawPassField(); return;
      }
      char ch=rowStr[c-(r2==2?1:0)];
      if(!sym&&kbLayer==1) ch=toupper(ch);
      if(kbText.length()<63) kbText+=ch;
      drawPassField(); return;
    }
  }
  if(hit(tx,ty, 38,KB_Y[3],110,KB_H-8)){ kbLayer=(kbLayer==2)?0:2; drawKeyboard(); return; }
  if(hit(tx,ty,156,KB_Y[3], 72,KB_H-8)){ if(kbText.length()<63)kbText+=','; drawPassField(); return; }
  if(hit(tx,ty,236,KB_Y[3],320,KB_H-8)){ if(kbText.length()<63)kbText+=' '; drawPassField(); return; }
  if(hit(tx,ty,564,KB_Y[3], 72,KB_H-8)){ if(kbText.length()<63)kbText+='.'; drawPassField(); return; }
  if(hit(tx,ty,644,KB_Y[3],118,KB_H-8)){ tryConnect(); return; }
}

void coordTouch(int tx,int ty){
  if(hit(tx,ty,60,140,240,52)){ coordField=0; drawCoordField(0); drawCoordField(1); return; }
  if(hit(tx,ty,330,140,240,52)){ coordField=1; drawCoordField(0); drawCoordField(1); return; }
  if(hit(tx,ty,60,340,180,56)){ saveCoords(); return; }
  if(hit(tx,ty,260,340,180,56)){ screen=SCR_MAIN; drawMainScreen(); return; }
  const char* keys[4][4]={{"7","8","9","<"},{"4","5","6","+/-"},{"1","2","3","."},{"0","","",""}};
  for(int r2=0;r2<4;r2++)
    for(int c=0;c<4;c++){
      if(!keys[r2][c][0]) continue;
      int w=(r2==3&&c==0)?286:90;
      if(hit(tx,ty,520+c*98,120+r2*70,w,60)){ coordKey(keys[r2][c]); return; }
    }
}

void netTouch(int tx,int ty){
  if(hit(tx,ty,27,92,140,44)){ editStatic=false; drawNetModeButtons(); for(int i=0;i<4;i++) drawNetField(i); return; }
  if(hit(tx,ty,175,92,140,44)){ editStatic=true;  drawNetModeButtons(); for(int i=0;i<4;i++) drawNetField(i); return; }
  for(int i=0;i<4;i++){
    int x,y; netFieldRect(i,x,y);
    if(hit(tx,ty,x,y,240,48)){
      if(editStatic){ int old=netField; netField=i; drawNetField(old); drawNetField(i); }
      return;
    }
  }
  if(hit(tx,ty,27,340,190,56)){ saveNet(); return; }
  if(hit(tx,ty,237,340,190,56)){ screen=SCR_MAIN; drawMainScreen(); return; }
  const char* keys[4][3]={{"7","8","9"},{"4","5","6"},{"1","2","3"},{"0",".","<"}};
  for(int r2=0;r2<4;r2++)
    for(int c=0;c<3;c++)
      if(hit(tx,ty,547+c*78,120+r2*64,70,56)){ netKey(keys[r2][c]); return; }
}

void menuTouch(int tx,int ty){
  if(hit(tx,ty,740,16,44,40)){ screen=SCR_MAIN; drawMainScreen(); return; }
  if(hit(tx,ty,240,84,320,50)){ doScan(); screen=SCR_WIFI_SCAN; drawScanScreen(); return; }
  if(hit(tx,ty,240,148,320,50)){ openNetScreen(); return; }
  if(hit(tx,ty,240,212,320,50)){
    coordLat=String(homeLat,6); coordLon=String(homeLon,6); coordField=0;
    screen=SCR_COORDS; drawCoordScreen(); return;
  }
  if(hit(tx,ty,240,276,320,50)){
    showLabels=!showLabels; prefs.putBool("lbl",showLabels);
    drawMenuScreen(); return;
  }
  if(hit(tx,ty,240,340,320,50)){ ESP.restart(); }
}

// ============================================================
//  Setup / loop
// ============================================================
void setup(){
  Serial.begin(115200);

  ch422g_init();
  lcd.init();

  colTxtHi=lcd.color565(230,237,244); colTxtMd=lcd.color565(143,163,184);
  colTxtLo=lcd.color565(96,114,134);  colAcc=lcd.color565(76,194,255);
  colAccDim=lcd.color565(38,97,128);  colHighA=lcd.color565(154,123,255);
  colMidA=lcd.color565(76,194,255);   colLowA=lcd.color565(255,180,84);
  colBad=lcd.color565(255,93,93);
  colBorder=lcd.color565(58,74,94);   colBorderHi=lcd.color565(104,132,162);
  colCard=lcd.color565(20,28,38);     colKey=lcd.color565(26,36,50);
  colKeyAcc=lcd.color565(34,52,72);

  lcd.fillScreen(lcd.color565(BG1[0],BG1[1],BG1[2]));
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtMd);
  lcd.drawString("Starting...",400,240);

  bg.setPsram(true);   bg.setColorDepth(16);
  plot.setPsram(true); plot.setColorDepth(16);
  bool okBg=bg.createSprite(800,480);
  bool okPl=plot.createSprite(PLOT_W,PLOT_H);
  if(!okBg||!okPl) Serial.println("Sprite create failed - check PSRAM = OPI PSRAM");

  buildChrome();

  memset(tracks,0,sizeof(tracks));

  prefs.begin("radar",false);
  homeLat=prefs.getDouble("lat",DEFAULT_LAT);
  homeLon=prefs.getDouble("lon",DEFAULT_LON);
  showLabels=prefs.getBool("lbl",true);
  rangeKm=prefs.getInt("rng",100);
  wifiSsid=prefs.getString("ssid","");
  wifiPass=prefs.getString("pass","");
  feedUrl=prefs.getString("feed",LOCAL_FEED_URL_DEFAULT);
  netStatic=prefs.getBool("nstat",false);
  netIp=prefs.getString("nip","");
  netGw=prefs.getString("ngw","");
  netMask=prefs.getString("nmask","255.255.255.0");
  netDns=prefs.getString("ndns","");

  if(!wifiSsid.length()){
    doScan();
    screen=SCR_WIFI_SCAN;
    drawScanScreen();
    return;
  }

  gradientBg();
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(colTxtHi);
  lcd.drawString("Connecting to "+wifiSsid+"...",400,240);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  applyNetConfig();
  WiFi.begin(wifiSsid.c_str(),wifiPass.c_str());
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t0<15000) delay(250);
  WiFi.setSleep(false);

  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("WiFi up, IP %s (%s)\n",WiFi.localIP().toString().c_str(),
                  netStatic?"static":"DHCP");
    configTzTime(TZ_STRING,"pool.ntp.org","time.nist.gov");
    startServer();
  }else{
    Serial.println("Stored WiFi failed - offline; gear > Change Wi-Fi");
  }

  screen=SCR_MAIN;
  drawMainScreen();
  startFetch();
  startSatFetch();                 // pull the satellite base for the current view
  lastPosTick=lastClockTick=millis();
}

void loop(){
  uint32_t now=millis();
  if(serverUp) server.handleClient();

  uint16_t tx,ty;
  bool touched=lcd.getTouch(&tx,&ty);
  if(touched&&!wasTouched){
    switch(screen){
      case SCR_MAIN:      mainTouch(tx,ty);  break;
      case SCR_WIFI_SCAN: scanTouch(tx,ty);  break;
      case SCR_WIFI_PASS: passTouch(tx,ty);  break;
      case SCR_COORDS:    coordTouch(tx,ty); break;
      case SCR_MENU:      menuTouch(tx,ty);  break;
      case SCR_NET:       netTouch(tx,ty);   break;
    }
  }
  wasTouched=touched;

  // a fresh satellite image is ready -> composite it into the chrome (heavy, one-shot)
  if(satReady) drawSatelliteBase();

  if(screen!=SCR_MAIN) return;

  // route lookup for the selected aircraft (Tier C) + apply any result
  if(selHex[0]&&!routeFetching){
    Track* s=findByHex(selHex);
    if(s&&!s->routeTried&&s->flight[0]) requestRoute(s->hex,s->flight);
  }
  if(routeResReady){
    routeResReady=false;
    Track* s=findByHex(routeResHex);
    if(s){ strlcpy(s->origin,routeResOrigin,sizeof(s->origin));
           strlcpy(s->dest,routeResDest,sizeof(s->dest)); s->routeTried=true;
           if(!strcmp(selHex,routeResHex)) drawSelectedCard(); }
  }

  if(applyPending()){
    drawSelectedCard(); drawOverview();
    renderPlot();
  }

  if(!fetchInProgress&&WiFi.status()==WL_CONNECTED&&
     now-lastFetchStart>=(feedIsLocal?POLL_LOCAL_MS:POLL_CLOUD_MS))
    startFetch();

  if(now-lastPosTick>=POS_TICK_MS){
    float dt=(now-lastPosTick)/1000.0f;
    lastPosTick=now;
    emergBlink=!emergBlink;              // flash emergency targets + Overview line
    deadReckon(dt);
    if(++posTickCounter>=TRAIL_EVERY_TICK){ posTickCounter=0; recordTrails(); }
    drawSelectedCard(); drawOverview();
    renderPlot();
  }

  if(now-lastClockTick>=1000){
    lastClockTick=now;
    drawTimeCard(false);
  }
}
