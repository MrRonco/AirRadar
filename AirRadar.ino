// AirRadar v5
// Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB, GT911 touch, CH422G expander)
//
// v5 changes:
//   - symmetric card layout: left column (Overview/Settings) fills to the same
//     baseline as the right column (Selected/Time)
//   - Overview: data age and source on separate lines (no more overlap)
//   - tappable range pill: 50 / 100 / 150 / 250 km, rings relabel live
//   - time card: local time only (UTC line removed), date below
//   - NETWORK settings screen: DHCP or static IP/gateway/mask/DNS on-device,
//     reached from the Settings card or the gear menu; bottom bar removed
//
// Data: local feeder (SUDCRATER, tar1090 on :8080) first, airplanes.live fallback.
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
#define DEFAULT_LAT   51.470000
#define DEFAULT_LON  -0.454300
#define HOME_LABEL   "SUDBURY"
#define TZ_STRING    "EST5EDT,M3.2.0,M11.1.0"
#define MDNS_NAME    "airradar"

// Local ADS-B feeder (SUDCRATER) - tried first every poll; cloud is fallback
// adsb.im image: tar1090 is on port 8080 (port 80 is the feeder config app)
#define LOCAL_FEED_URL_DEFAULT "http://adsb.local:8080/data/aircraft.json"
#define LOCAL_FEED_NAME        "SUDCRATER"

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

const int OV_X=12,  OV_Y=64,  OV_W=168, OV_H=176;   // overview card
const int ST_X=12,  ST_Y=248, ST_W=168, ST_H=188;   // settings card
const int SC_X=620, SC_Y=20,  SC_W=168, SC_H=300;   // selected craft card
const int TM_X=620, TM_Y=332, TM_W=168, TM_H=104;   // time card
const int ARROW_L_X=216, ARROW_R_X=584, ARROW_Y=388, ARROW_R=22;
const int GEAR_X=766, GEAR_Y=458, GEAR_R=16;

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
  double lat, lon;
  float gsKt, trackDeg;
  int altFt, vRateFpm;
  uint32_t lastApiMs;
  float trailX[TRAIL_LEN], trailY[TRAIL_LEN];
  int trailCount, trailHead;
};
const int MAX_TRACKS = 40;
Track tracks[MAX_TRACKS];

struct ApiPlane {
  char hex[8], flight[12], typeCode[8], category[5], squawk[6], reg[10];
  double lat, lon; float gsKt, trackDeg; int altFt, vRateFpm;
};
ApiPlane pendingPlanes[MAX_TRACKS];
int  pendingCount=0;
bool pendingReady=false, pendingOk=false, pendingLocal=true, fetchInProgress=false;
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastFetchStart=0, lastGoodApply=0, lastPosTick=0, lastClockTick=0;
int posTickCounter=0, lastSubMin=-1, visibleCount=0;

char selHex[8]="";
int  orderIdx[MAX_TRACKS], orderN=0;

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

  // glass cards
  glassRect(OV_X,OV_Y,OV_W,OV_H,14);
  glassRect(ST_X,ST_Y,ST_W,ST_H,14);
  glassRect(SC_X,SC_Y,SC_W,SC_H,14);
  glassRect(TM_X,TM_Y,TM_W,TM_H,14);
  glassRect(348,446,104,24,12);                 // range pill (text is dynamic)
  glassCircle(ARROW_L_X,ARROW_Y,ARROW_R);
  glassCircle(ARROW_R_X,ARROW_Y,ARROW_R);
  glassCircle(GEAR_X,GEAR_Y,GEAR_R);

  // brand
  bg.setTextDatum(top_left);
  bg.setFont(&fonts::FreeSansBold12pt7b);
  bg.setTextColor(colTxtHi); bg.drawString("AIR RADAR",14,14);
  bg.setFont(&fonts::FreeSans9pt7b);
  bg.setTextColor(colAcc);   bg.drawString(HOME_LABEL,15,40);

  // card headers + fixed labels
  bg.setFont(&fonts::FreeSansBold9pt7b);
  bg.setTextColor(colTxtLo);
  bg.setTextDatum(top_center);
  bg.drawString("OVERVIEW",OV_X+OV_W/2,OV_Y+10);
  bg.drawString("SETTINGS",ST_X+ST_W/2,ST_Y+10);
  bg.drawString("SELECTED",SC_X+SC_W/2,SC_Y+8);
  bg.drawString("AIRCRAFT",SC_X+SC_W/2,SC_Y+26);
  bg.setTextDatum(top_left);
  bg.setFont(&fonts::FreeSans9pt7b);
  bg.drawString("AIRCRAFT",OV_X+14,OV_Y+36);
  bg.drawString("UPDATED",OV_X+14,OV_Y+108);
  bg.drawString("COORDS",ST_X+14,ST_Y+36);
  bg.drawString("LABELS",ST_X+14,ST_Y+146);
  bg.drawFastHLine(OV_X+12,OV_Y+100,OV_W-24,colBorder);
  bg.drawFastHLine(ST_X+12,ST_Y+124,ST_W-24,colBorder);

  // arrow chevrons
  bg.fillTriangle(ARROW_L_X+7,ARROW_Y-10,ARROW_L_X+7,ARROW_Y+10,ARROW_L_X-8,ARROW_Y,colAcc);
  bg.fillTriangle(ARROW_R_X-7,ARROW_Y-10,ARROW_R_X-7,ARROW_Y+10,ARROW_R_X+8,ARROW_Y,colAcc);

  // gear icon
  bg.drawCircle(GEAR_X,GEAR_Y,7,colAcc);
  bg.drawCircle(GEAR_X,GEAR_Y,2,colAcc);
  for(int a=0;a<360;a+=60){
    float rad=deg2rad((float)a);
    bg.drawLine(GEAR_X+cosf(rad)*7,GEAR_Y+sinf(rad)*7,
                GEAR_X+cosf(rad)*11,GEAR_Y+sinf(rad)*11,colAcc);
  }
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
}

bool fetchParse(Stream &s, bool local){
  StaticJsonDocument<1024> filter;
  fillFilter(filter["ac"].createNestedObject());
  fillFilter(filter["aircraft"].createNestedObject());

  DynamicJsonDocument doc(49152);
  DeserializationError err=deserializeJson(doc,s,DeserializationOption::Filter(filter));
  if(err){ Serial.printf("JSON(%s): %s\n",local?"local":"cloud",err.c_str()); return false; }

  JsonArray arr=doc["aircraft"].as<JsonArray>();
  if(arr.isNull()) arr=doc["ac"].as<JsonArray>();
  if(arr.isNull()) return false;

  ApiPlane temp[MAX_TRACKS]; int n=0;
  for(JsonObject ac: arr){
    if(n>=MAX_TRACKS) break;
    if(!ac["lat"].is<float>()||!ac["lon"].is<float>()) continue;
    if(local){
      float sp=ac["seen_pos"]|999.0f;
      if(sp>15.0f) continue;
    }
    double lat=ac["lat"], lon=ac["lon"];
    if(haversineKm(homeLat,homeLon,lat,lon)>(float)rangeKm) continue;
    ApiPlane &p=temp[n];
    strlcpy(p.hex,ac["hex"]|"",sizeof(p.hex));
    strlcpy(p.flight,ac["flight"]|"",sizeof(p.flight));
    strlcpy(p.typeCode,ac["t"]|"",sizeof(p.typeCode));
    strlcpy(p.category,ac["category"]|"",sizeof(p.category));
    strlcpy(p.squawk,ac["squawk"]|"",sizeof(p.squawk));
    strlcpy(p.reg,ac["r"]|"",sizeof(p.reg));
    p.lat=lat; p.lon=lon;
    p.gsKt=ac["gs"]|0.0f; p.trackDeg=ac["track"]|0.0f;
    if(ac["alt_baro"].is<int>()) p.altFt=ac["alt_baro"];
    else if(ac["alt_baro"].is<const char*>()) p.altFt=0;
    else if(ac["alt_geom"].is<int>()) p.altFt=ac["alt_geom"];
    else p.altFt=-1;
    if(ac["baro_rate"].is<int>()) p.vRateFpm=ac["baro_rate"];
    else if(ac["geom_rate"].is<int>()) p.vRateFpm=ac["geom_rate"];
    else p.vRateFpm=0;
    n++;
  }

  portENTER_CRITICAL(&dataMux);
  pendingCount=n;
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
      if(ok) return;
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
      http.addHeader("User-Agent","ESP32-AirRadar/5.0");
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

bool applyPending(){
  ApiPlane local[MAX_TRACKS]; int n=0; bool ok=false, pl=true, ready;
  portENTER_CRITICAL(&dataMux);
  ready=pendingReady;
  if(ready){ ok=pendingOk; n=pendingCount; pl=pendingLocal;
    if(ok) memcpy(local,pendingPlanes,sizeof(ApiPlane)*n);
    pendingReady=false; }
  portEXIT_CRITICAL(&dataMux);
  if(!ready||!ok) return false;
  feedIsLocal=pl;

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
    strlcpy(t.flight,p.flight,sizeof(t.flight));
    strlcpy(t.typeCode,p.typeCode,sizeof(t.typeCode));
    strlcpy(t.category,p.category,sizeof(t.category));
    strlcpy(t.squawk,p.squawk,sizeof(t.squawk));
    strlcpy(t.reg,p.reg,sizeof(t.reg));
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
  if(emerg){r=255;g=93;bc=93;} else altRGB(t.altFt,r,g,bc);
  if(coasting){r=(uint8_t)(r*0.55f);g=(uint8_t)(g*0.55f);bc=(uint8_t)(bc*0.55f);}
  uint16_t col=plot.color565(r,g,bc);

  for(int i=0;i<t.trailCount;i++){
    int idx=(t.trailHead-1-i+TRAIL_LEN*2)%TRAIL_LEN;
    float f=0.65f-0.13f*i;
    plot.fillCircle((int)(t.trailX[idx]-PLOT_X),(int)(t.trailY[idx]-PLOT_Y),
                    (i==0)?2:1,plot.color565(r*f,g*f,bc*f));
  }

  float nx,ny,rx,ry,lx,ly,cx2,cy2;
  rotPt(0,-7,t.trackDeg,nx,ny);   rotPt(4.5,6,t.trackDeg,rx,ry);
  rotPt(-4.5,6,t.trackDeg,lx,ly); rotPt(0,3,t.trackDeg,cx2,cy2);
  if(coasting){
    plot.drawTriangle(sx+nx,sy+ny,sx+rx,sy+ry,sx+cx2,sy+cy2,col);
    plot.drawTriangle(sx+nx,sy+ny,sx+cx2,sy+cy2,sx+lx,sy+ly,col);
  }else{
    plot.fillTriangle(sx+nx,sy+ny,sx+rx,sy+ry,sx+cx2,sy+cy2,col);
    plot.fillTriangle(sx+nx,sy+ny,sx+cx2,sy+cy2,sx+lx,sy+ly,col);
  }
  if(sel){
    plot.drawEllipse(sx,sy,(int)(13*X_CORR)+1,13,colTxtHi);
    plot.drawEllipse(sx,sy,(int)(15*X_CORR)+1,15,colAccDim);
  }
  if(t.gsKt>20&&!coasting){
    float L=t.gsKt/16.0f; if(L>30)L=30;
    float ex,ey; rotPt(0,-7-L,t.trackDeg,ex,ey);
    plot.drawLine(sx+nx,sy+ny,sx+ex,sy+ey,col);
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
  restore(OV_X+8,OV_Y+52,OV_W-16,OV_H-58);
  bool link=(WiFi.status()==WL_CONNECTED);
  bool stale=(millis()-lastGoodApply>STALE_FEED_MS)||lastGoodApply==0;
  uint16_t st=!link?colBad:(stale?colLowA:colAcc);

  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(colAcc);
  lcd.drawString(String(visibleCount),OV_X+16,OV_Y+58);

  int px=OV_X+120, py=OV_Y+79;
  lcd.fillTriangle(px,py-8,px+6,py+7,px,py+3,st);
  lcd.fillTriangle(px,py-8,px,py+3,px-6,py+7,st);

  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(st);
  char buf[24];
  const char* lbl=!link?"NO LINK":(stale?"STALE":nullptr);
  if(lbl) snprintf(buf,sizeof(buf),"%s",lbl);
  else{ uint32_t age=(millis()-lastGoodApply)/1000;
        snprintf(buf,sizeof(buf),"%lus ago",(unsigned long)age); }
  lcd.drawString(buf,OV_X+14,OV_Y+128);

  // source on its own line
  if(!link){ lcd.setTextColor(colBad);  lcd.drawString("OFFLINE",OV_X+14,OV_Y+148); }
  else if(feedIsLocal){ lcd.setTextColor(colAcc); lcd.drawString(LOCAL_FEED_NAME,OV_X+14,OV_Y+148); }
  else{ lcd.setTextColor(colTxtMd); lcd.drawString("CLOUD",OV_X+14,OV_Y+148); }
}

void drawSettingsVals(){
  restore(ST_X+8,ST_Y+52,ST_W-16,64);
  lcd.setTextDatum(top_left);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtHi);
  char l1[20],l2[20];
  snprintf(l1,sizeof(l1),"%.4f %c",fabs(homeLat),homeLat>=0?'N':'S');
  snprintf(l2,sizeof(l2),"%.4f %c",fabs(homeLon),homeLon>=0?'E':'W');
  lcd.drawString(l1,ST_X+16,ST_Y+58);
  lcd.drawString(l2,ST_X+16,ST_Y+82);
  lcd.setFont(&fonts::Font0);
  lcd.setTextColor(colTxtLo);
  lcd.drawString("TAP TO EDIT",ST_X+16,ST_Y+104);

  // labels toggle pill
  restore(ST_X+ST_W-64,ST_Y+142,52,26);
  int tx=ST_X+ST_W-62, ty=ST_Y+144, tw=48, th=22;
  lcd.fillRoundRect(tx,ty,tw,th,11,showLabels?colAcc:lcd.color565(40,52,66));
  int kx=showLabels?tx+tw-11:tx+11;
  lcd.fillCircle(kx,ty+th/2,8,showLabels?lcd.color565(8,14,20):colTxtLo);
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
  restore(SC_X+6,SC_Y+46,SC_W-12,SC_H-52);
  Track* t=findByHex(selHex);
  lcd.setTextDatum(top_left);

  if(!t){
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(colTxtLo);
    lcd.setTextDatum(top_center);
    lcd.drawString("Tap a target or",SC_X+SC_W/2,SC_Y+150);
    lcd.drawString("use the arrows",SC_X+SC_W/2,SC_Y+174);
    lcd.setTextDatum(top_left);
    return;
  }

  String call(t->flight); call.trim(); if(!call.length()) call=String(t->hex);
  bool emerg=isEmergency(t->squawk);
  float d=haversineKm(homeLat,homeLon,t->lat,t->lon);

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(emerg?colBad:colTxtHi);
  lcd.drawString(call.substring(0,9),SC_X+14,SC_Y+48);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(colTxtMd);
  String typ(t->typeCode); typ.trim(); if(!typ.length()) typ=String(t->category);
  lcd.drawString(typ.substring(0,8),SC_X+14,SC_Y+76);
  if(emerg){
    lcd.setTextColor(colBad);
    char sq[10]; snprintf(sq,sizeof(sq),"SQ %s",t->squawk);
    lcd.setTextDatum(top_right);
    lcd.drawString(sq,SC_X+SC_W-12,SC_Y+76);
    lcd.setTextDatum(top_left);
  }

  struct Row{const char* k; char v[16]; uint16_t c;};
  Row rows[7];
  uint8_t rr,gg,bb; altRGB(t->altFt,rr,gg,bb);
  snprintf(rows[0].v,16,t->altFt>=0?"%d ft":"---",t->altFt);
  rows[0].k="ALT"; rows[0].c=lcd.color565(rr,gg,bb);
  snprintf(rows[1].v,16,"%d kt",(int)t->gsKt); rows[1].k="SPD"; rows[1].c=colTxtHi;
  snprintf(rows[2].v,16,"%d %s",(int)t->trackDeg,cardinal8(t->trackDeg));
  rows[2].k="HDG"; rows[2].c=colTxtHi;
  snprintf(rows[3].v,16,"%+d fpm",t->vRateFpm); rows[3].k="V/S";
  rows[3].c=(t->vRateFpm>300)?colAcc:(t->vRateFpm<-300?colLowA:colTxtHi);
  snprintf(rows[4].v,16,"%.1f km",d); rows[4].k="DIST"; rows[4].c=colTxtHi;
  snprintf(rows[5].v,16,"%s",regCountry(t->reg)); rows[5].k="CTRY"; rows[5].c=colAcc;
  snprintf(rows[6].v,16,"%s",categoryName(t->category)); rows[6].k="CAT"; rows[6].c=colTxtMd;

  lcd.setFont(&fonts::FreeSans9pt7b);
  for(int i=0;i<7;i++){
    int y=SC_Y+102+i*26;
    lcd.setTextDatum(top_left);
    lcd.setTextColor(colTxtLo);  lcd.drawString(rows[i].k,SC_X+14,y);
    lcd.setTextDatum(top_right);
    lcd.setTextColor(rows[i].c); lcd.drawString(rows[i].v,SC_X+SC_W-14,y);
  }
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
  drawSettingsVals();
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
  screen=SCR_MAIN; drawMainScreen(); startFetch();
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
  lcd.setTextColor(colTxtLo);
  String ipl=(WiFi.status()==WL_CONNECTED)?
    ("Browser config: http://"+WiFi.localIP().toString()+"/  (http://" MDNS_NAME ".local/)"):
    String("Not connected");
  lcd.drawString(ipl,400,404);
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
  startFetch();
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
}

void mainTouch(int tx,int ty){
  if(hitC(tx,ty,GEAR_X,GEAR_Y,GEAR_R+6)){ screen=SCR_MENU; drawMenuScreen(); return; }
  if(hitC(tx,ty,ARROW_L_X,ARROW_Y,ARROW_R+6)){
    selectByOrder(-1); drawSelectedCard(); renderPlot(); return; }
  if(hitC(tx,ty,ARROW_R_X,ARROW_Y,ARROW_R+6)){
    selectByOrder(+1); drawSelectedCard(); renderPlot(); return; }
  if(hit(tx,ty,340,440,120,36)){ cycleRange(); return; }
  if(hit(tx,ty,ST_X,ST_Y,ST_W,ST_H)){
    if(ty<ST_Y+124){
      coordLat=String(homeLat,6); coordLon=String(homeLon,6); coordField=0;
      screen=SCR_COORDS; drawCoordScreen();
    }else{
      showLabels=!showLabels; prefs.putBool("lbl",showLabels);
      drawSettingsVals(); renderPlot();
    }
    return;
  }
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

  if(screen!=SCR_MAIN) return;

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
