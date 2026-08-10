// Open Media Player v1.2 - Waveshare ESP32-S3-LCD-1.3 Prism
// Board: ESP32S3 Dev Module; USB CDC On Boot: Disabled; PSRAM: Disabled.
// Validated library set: Arduino-ESP32 3.2.0, Arduino GFX 1.5.9,
// JPEGDEC 1.8.4, Dev Device Pins 0.0.3 (not required by this profile).

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <stdlib.h>
#include "MjpegClass.h"

// This board routes its Type-C serial interface through the CH343 USB-to-UART
// bridge. Use UART0 explicitly instead of relying on the selected USB mode.
#define Serial Serial0

// Hardware values are from Waveshare's supplied SD_Video_Prism example.
#define LCD_BACKLIGHT_PIN 20
#define LCD_DC_PIN 38
#define LCD_CS_PIN 39
#define LCD_SCK_PIN 40
#define LCD_MOSI_PIN 41
#define LCD_RST_PIN 42
#define SDMMC_CS_PIN 17
#define SDMMC_CMD_PIN 18
#define SDMMC_CLK_PIN 21
#define SDMMC_D0_PIN 16
#define DEFAULT_BUTTON_PIN 1 // External OMP playback button, active-low

#define MAX_FILES 20
#define CONFIG_FILE "/config.json"
#define PLAYLIST_FILE "/playlist.txt"
#define GPIO_FILE "/gpio.json"
#define JSON_FILE_MAX_BYTES 4096
#define MIN_BUFFER_KB 32U
#define MAX_BUFFER_KB 96U
#define FILL_SOURCE_MAX_WIDTH 320
#define FILL_SOURCE_MAX_HEIGHT 320
#define FILL_SOURCE_MAX_PIXELS (240 * 240)
#define FILL_OUTPUT_ROWS 16

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC_PIN, LCD_CS_PIN, LCD_SCK_PIN,
                                              LCD_MOSI_PIN, GFX_NOT_DEFINED, FSPI);
// The fitted panel matches Waveshare's zero-offset, rotation-1 profile.
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST_PIN, 1, true,
                                       240, 240, 0, 0, 0, 0);

enum class PlaybackMode : uint8_t { Sequential, Shuffle, RepeatOne };
enum class DisplayMode : uint8_t { Fit, Fill };
enum class ButtonAction : uint8_t { None, Skip, TogglePause };
struct PlayerConfig {
  uint8_t brightness = 255;
  uint16_t bufferKb = 64;
  uint8_t targetFps = 15;
  bool startupScan = true;
  PlaybackMode playbackMode = PlaybackMode::Sequential;
  DisplayMode displayMode = DisplayMode::Fit;
  uint16_t imageRotation = 0;
  String mjpegFolder = "/mjpeg";
  int buttonPin = DEFAULT_BUTTON_PIN;
  int backlightPin = LCD_BACKLIGHT_PIN;
  bool buttonActiveLow = true;
};
struct MjpegScanResult { uint32_t frameCount = 0, largestFrame = 0; bool incompleteFrame = false; };

PlayerConfig config;
String mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {};
uint16_t mjpegFileRotations[MAX_FILES] = {};
int mjpegCount = 0, currentMjpegIndex = 0, totalFrames = 0;
unsigned long totalReadVideo = 0, totalDecodeVideo = 0, totalShowVideo = 0, currentMs = 0;
size_t mjpegBufferSize = 0;
uint8_t *mjpegBuffer = nullptr;
uint16_t *fillFrameBuffer = nullptr;
uint16_t fillOutputBlock[240 * FILL_OUTPUT_ROWS];
bool fillCaptureFailed = false;
bool fillCaptureEnabled = false;
int fillCaptureStride = 0;
bool transformFallbackWarningShown = false;
MjpegClass mjpeg;
constexpr uint32_t LONG_PRESS_MS = 750, BUTTON_DEBOUNCE_MS = 40;

bool loadText(const char *filename, String &contents) {
  File file = SD_MMC.open(filename, "r");
  if (!file) return false;
  if (file.size() > JSON_FILE_MAX_BYTES) { Serial.printf("WARNING: %s is larger than %u bytes; ignored\n", filename, JSON_FILE_MAX_BYTES); file.close(); return false; }
  contents = file.readString(); file.close(); return true;
}
String uncommentJson(const String &text) {
  String result; bool inString = false;
  for (size_t i = 0; i < text.length(); ++i) {
    char c = text[i]; if (c == '"' && (i == 0 || text[i - 1] != '\\')) inString = !inString;
    if (!inString && c == '/' && i + 1 < text.length() && text[i + 1] == '/') { while (i < text.length() && text[i] != '\n') ++i; if (i < text.length()) result += '\n'; }
    else result += c;
  } return result;
}
bool jsonValue(const String &json, const char *key, String &value) {
  int p = json.indexOf(String("\"") + key + "\""); if (p < 0 || (p = json.indexOf(':', p)) < 0) return false;
  for (++p; p < (int)json.length() && isspace((unsigned char)json[p]); ++p) {}
  if (p >= (int)json.length()) return false;
  if (json[p] == '"') { int end = json.indexOf('"', p + 1); if (end < 0) return false; value = json.substring(p + 1, end); return true; }
  int end = p; while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && !isspace((unsigned char)json[end])) ++end;
  value = json.substring(p, end); value.trim(); return value.length() != 0;
}
bool jsonUInt(const String &json, const char *key, uint32_t &out) { String v; if (!jsonValue(json, key, v) || !v.length()) return false; for (size_t i=0;i<v.length();++i) if (!isdigit((unsigned char)v[i])) return false; out = strtoul(v.c_str(), nullptr, 10); return true; }
bool jsonBool(const String &json, const char *key, bool &out) { String v; if (!jsonValue(json,key,v)) return false; if(v=="true"){out=true;return true;} if(v=="false"){out=false;return true;} return false; }
bool safeFolder(const String &v) { return v.length() > 1 && v.length() < 64 && v[0] == '/' && v.indexOf("..") < 0; }
bool safeName(const String &v) { return v.length() && v.length() < 96 && v.indexOf('/') < 0 && v.indexOf('\\') < 0 && v.indexOf("..") < 0 && v.endsWith(".mjpeg"); }
bool validRotation(uint32_t value) { return value == 0 || value == 90 || value == 180 || value == 270; }
bool parseRotation(const String &text, uint16_t &rotation) {
  if (!text.length()) return false;
  for (size_t i = 0; i < text.length(); ++i) if (!isdigit((unsigned char)text[i])) return false;
  const uint32_t value = strtoul(text.c_str(), nullptr, 10);
  if (!validRotation(value)) return false;
  rotation = (uint16_t)value;
  return true;
}

void loadConfiguration() {
  String text; uint32_t n; bool b; String s;
  if (loadText(CONFIG_FILE, text)) {
    String json = uncommentJson(text);
    if (jsonUInt(json,"brightness",n) && n <= 255) config.brightness = n;
    if (jsonUInt(json,"buffer_kb",n) && n >= MIN_BUFFER_KB && n <= MAX_BUFFER_KB) config.bufferKb = n;
    if (jsonUInt(json,"target_fps",n) && n >= 1 && n <= 60) config.targetFps = n;
    if (jsonBool(json,"startup_scan",b)) config.startupScan = b;
    if (jsonValue(json,"mjpeg_folder",s) && safeFolder(s)) config.mjpegFolder = s;
    if (jsonValue(json,"playback_mode",s)) { if(s=="shuffle") config.playbackMode=PlaybackMode::Shuffle; else if(s=="repeat_one") config.playbackMode=PlaybackMode::RepeatOne; }
    if (jsonValue(json,"display_mode",s)) { if(s=="fill") config.displayMode=DisplayMode::Fill; else if(s=="fit") config.displayMode=DisplayMode::Fit; }
    if (jsonUInt(json,"image_rotation",n) && validRotation(n)) config.imageRotation = (uint16_t)n;
  } else Serial.println("config.json not found; using defaults");
  if (loadText(GPIO_FILE,text)) { String json=uncommentJson(text); if(jsonUInt(json,"button_pin",n) && n <= 48) config.buttonPin=n; if(jsonUInt(json,"backlight_pin",n) && n <= 48) config.backlightPin=n; if(jsonBool(json,"button_active_low",b)) config.buttonActiveLow=b; }
  const char *mode = config.playbackMode == PlaybackMode::Shuffle ? "shuffle" : config.playbackMode == PlaybackMode::RepeatOne ? "repeat_one" : "sequential";
  const char *display = config.displayMode == DisplayMode::Fill ? "fill" : "fit";
  Serial.printf("Configuration: brightness %u, buffer %u KB, target %u FPS, folder %s, mode %s, display %s, rotation %u\n",config.brightness,config.bufferKb,config.targetFps,config.mjpegFolder.c_str(),mode,display,config.imageRotation);
  Serial.printf("GPIO: button %d (%s), backlight %d\n",config.buttonPin,config.buttonActiveLow?"active-low":"active-high",config.backlightPin);
}
void setBrightness() { ledcAttachChannel(config.backlightPin, 1000, 8, 1); ledcWrite(config.backlightPin, config.brightness); }
ButtonAction pollButton() {
  static bool wasPressed=false; static uint32_t pressedAt=0;
  bool level = digitalRead(config.buttonPin) == HIGH, pressed = config.buttonActiveLow ? !level : level; uint32_t now=millis();
  if (pressed && !wasPressed) { wasPressed=true; pressedAt=now; }
  else if (!pressed && wasPressed) { wasPressed=false; uint32_t held=now-pressedAt; if(held < BUTTON_DEBOUNCE_MS) return ButtonAction::None; return held >= LONG_PRESS_MS ? ButtonAction::TogglePause : ButtonAction::Skip; }
  return ButtonAction::None;
}
int jpegDrawCallback(JPEGDRAW *p) {
  // Edge MCU blocks legitimately exceed the display boundary; Arduino_GFX clips them.
  if (!p || !p->pPixels || p->x < 0 || p->y < 0 || p->x >= gfx->width() || p->y >= gfx->height() || p->iWidth <= 0 || p->iHeight <= 0 || p->iWidth > gfx->width() || p->iHeight > gfx->height()) return 1;
  unsigned long started=millis(); gfx->draw16bitBeRGBBitmap(p->x,p->y,p->pPixels,p->iWidth,p->iHeight); totalShowVideo += millis()-started; return 1;
}
int jpegFillCaptureCallback(JPEGDRAW *p) {
  const int sourceWidth = mjpeg.getFrameWidth(), sourceHeight = mjpeg.getFrameHeight();
  if (!p || !p->pPixels || !fillFrameBuffer || sourceWidth <= 0 || sourceHeight <= 0 ||
      sourceWidth > FILL_SOURCE_MAX_WIDTH || sourceHeight > FILL_SOURCE_MAX_HEIGHT ||
      (size_t)sourceWidth * sourceHeight > FILL_SOURCE_MAX_PIXELS || p->x < 0 || p->y < 0 ||
      p->iWidth <= 0 || p->iHeight <= 0 || p->x >= sourceWidth || p->y >= sourceHeight) {
    fillCaptureFailed = true;
    return 1;
  }
  fillCaptureStride = sourceWidth;
  const int copyWidth = p->x + p->iWidth > sourceWidth ? sourceWidth - p->x : p->iWidth;
  const int copyHeight = p->y + p->iHeight > sourceHeight ? sourceHeight - p->y : p->iHeight;
  for (int row = 0; row < copyHeight; ++row) {
    memcpy(fillFrameBuffer + (p->y + row) * fillCaptureStride + p->x,
           p->pPixels + row * p->iWidth, (size_t)copyWidth * sizeof(uint16_t));
  }
  return 1;
}
int jpegOutputCallback(JPEGDRAW *p) {
  return fillCaptureEnabled ? jpegFillCaptureCallback(p) : jpegDrawCallback(p);
}
bool drawTransformedFrame(int sourceWidth, int sourceHeight, uint16_t rotation, bool fill) {
  const int targetWidth = gfx->width(), targetHeight = gfx->height();
  if (!fillFrameBuffer || sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > FILL_SOURCE_MAX_WIDTH ||
      sourceHeight > FILL_SOURCE_MAX_HEIGHT || (size_t)sourceWidth * sourceHeight > FILL_SOURCE_MAX_PIXELS ||
      fillCaptureStride != sourceWidth || !validRotation(rotation) || targetWidth != 240 || targetHeight != 240) return false;
  const int rotatedWidth = (rotation == 90 || rotation == 270) ? sourceHeight : sourceWidth;
  const int rotatedHeight = (rotation == 90 || rotation == 270) ? sourceWidth : sourceHeight;
  const float scaleX = (float)targetWidth / rotatedWidth, scaleY = (float)targetHeight / rotatedHeight;
  const float scale = fill ? (scaleX > scaleY ? scaleX : scaleY) : (scaleX < scaleY ? scaleX : scaleY);
  const int outputWidth = fill ? targetWidth : (int)(rotatedWidth * scale + 0.5f);
  const int outputHeight = fill ? targetHeight : (int)(rotatedHeight * scale + 0.5f);
  const int outputX = fill ? 0 : (targetWidth - outputWidth) / 2;
  const int outputY = fill ? 0 : (targetHeight - outputHeight) / 2;
  const float cropX = fill ? ((float)rotatedWidth - (float)targetWidth / scale) * 0.5f : 0.0f;
  const float cropY = fill ? ((float)rotatedHeight - (float)targetHeight / scale) * 0.5f : 0.0f;
  unsigned long started = millis();
  for (int y = 0; y < outputHeight; y += FILL_OUTPUT_ROWS) {
    const int rows = (outputHeight - y) < FILL_OUTPUT_ROWS ? (outputHeight - y) : FILL_OUTPUT_ROWS;
    for (int row = 0; row < rows; ++row) {
      int sourceY = (int)(((float)(y + row) / scale) + cropY);
      if (sourceY < 0) sourceY = 0; else if (sourceY >= rotatedHeight) sourceY = rotatedHeight - 1;
      uint16_t *destination = fillOutputBlock + row * outputWidth;
      for (int x = 0; x < outputWidth; ++x) {
        int sourceX = (int)(((float)x / scale) + cropX);
        if (sourceX < 0) sourceX = 0; else if (sourceX >= rotatedWidth) sourceX = rotatedWidth - 1;
        int originalX = sourceX, originalY = sourceY;
        if (rotation == 90) { originalX = sourceY; originalY = sourceHeight - 1 - sourceX; }
        else if (rotation == 180) { originalX = sourceWidth - 1 - sourceX; originalY = sourceHeight - 1 - sourceY; }
        else if (rotation == 270) { originalX = sourceWidth - 1 - sourceY; originalY = sourceX; }
        destination[x] = fillFrameBuffer[originalY * fillCaptureStride + originalX];
      }
    }
    gfx->draw16bitBeRGBBitmap(outputX, outputY + y, fillOutputBlock, outputWidth, rows);
  }
  totalShowVideo += millis() - started;
  return true;
}
MjpegScanResult scanFile(File &file) {
  MjpegScanResult r; uint8_t buf[READ_BUFFER_SIZE], previous=0; bool in=false, have=false; uint32_t size=0; file.seek(0);
  while(true) { size_t count=file.read(buf,sizeof(buf)); if(!count) break; for(size_t i=0;i<count;++i) { uint8_t v=buf[i]; if(!in && have && previous==0xFF && v==0xD8){in=true;size=2;} else if(in){if(size<UINT32_MAX)++size; if(previous==0xFF && v==0xD9){++r.frameCount;if(size>r.largestFrame)r.largestFrame=size;in=false;}} previous=v;have=true; } }
  r.incompleteFrame=in; file.seek(0); return r;
}
bool addFile(const String &name, uint16_t rotation) {
  if(mjpegCount >= MAX_FILES || !safeName(name)) return false; String path=config.mjpegFolder+"/"+name; File file=SD_MMC.open(path,"r");
  if(!file || file.isDirectory()){Serial.printf("WARNING: Playlist file missing: %s\n",name.c_str());return false;} mjpegFileList[mjpegCount]=name;mjpegFileSizes[mjpegCount]=file.size();mjpegFileRotations[mjpegCount]=rotation;
  if(config.startupScan){MjpegScanResult r=scanFile(file);bool ok=r.frameCount && !r.incompleteFrame && r.largestFrame<=mjpegBufferSize;Serial.printf("Check: %s - %lu frames, largest %lu bytes: %s%s\n",name.c_str(),(unsigned long)r.frameCount,(unsigned long)r.largestFrame,ok?"OK":"UNSUPPORTED",r.incompleteFrame?" (incomplete frame)":"");}
  file.close();++mjpegCount;return true;
}
void loadMjpegFiles() {
  mjpegCount=0; File list=SD_MMC.open(PLAYLIST_FILE,"r"); bool present=false; if(list){present=true;Serial.println("Loading playlist.txt");while(list.available()&&mjpegCount<MAX_FILES){String n=list.readStringUntil('\n');n.trim();if(n.length()&&!n.startsWith("#")){uint16_t rotation=config.imageRotation;int separator=n.indexOf('|');if(separator>=0){String rotationText=n.substring(separator+1);n=n.substring(0,separator);n.trim();rotationText.trim();if(!parseRotation(rotationText,rotation)){Serial.printf("WARNING: Invalid playlist rotation for %s\n",n.c_str());continue;}}addFile(n,rotation);}}list.close();}
  if(!present || !mjpegCount){if(present)Serial.println("Playlist has no valid entries; scanning MJPEG folder");File dir=SD_MMC.open(config.mjpegFolder);if(!dir){Serial.printf("Failed to open %s folder\n",config.mjpegFolder.c_str());return;}while(mjpegCount<MAX_FILES){File f=dir.openNextFile();if(!f)break;if(!f.isDirectory()){String n=f.name();if(n.endsWith(".mjpeg"))addFile(n,config.imageRotation);}f.close();}dir.close();}
  Serial.printf("%d mjpeg files read\n",mjpegCount);for(int i=0;i<mjpegCount;++i)Serial.printf("File %d: %s, Size: %lu bytes, rotation: %u\n",i,mjpegFileList[i].c_str(),(unsigned long)mjpegFileSizes[i],mjpegFileRotations[i]);
}
void playFile(const String &path, uint16_t rotation) {
  File file=SD_MMC.open(path,"r");
  if(!file||file.isDirectory()){Serial.printf("ERROR: Failed to open %s\n",path.c_str());return;}
  Serial.println("MJPEG start");gfx->fillScreen(RGB565_BLACK);
  unsigned long start=millis(), nextFrameAt=start;
  const uint32_t framePeriodMs=1000U/config.targetFps;
  currentMs=start;totalFrames=totalReadVideo=totalDecodeVideo=totalShowVideo=0;transformFallbackWarningShown=false;bool paused=false;
  if(!mjpeg.setup(&file,mjpegBuffer,mjpegBufferSize,jpegOutputCallback,true,0,0,gfx->width(),gfx->height())){Serial.println("ERROR: MJPEG parser setup failed");file.close();return;}
  while(file.available()){
    while(!paused && totalFrames && (int32_t)(nextFrameAt-millis())>0) delay(1);
    currentMs=millis();
    ButtonAction a=pollButton();
    if(a==ButtonAction::Skip){Serial.println("Playback skipped");break;}
    if(a==ButtonAction::TogglePause){paused=!paused;nextFrameAt=millis();Serial.println(paused?"Playback paused":"Playback resumed");}
    if(paused){delay(10);continue;}
    if(!mjpeg.readMjpegBuf())break;
    totalReadVideo+=millis()-currentMs;currentMs=millis();
    bool shown = false;
    if((config.displayMode==DisplayMode::Fill || rotation != 0) && fillFrameBuffer){
      fillCaptureEnabled=true;fillCaptureFailed=false;fillCaptureStride=0;
      mjpeg.setForceNative(true);
      if(mjpeg.drawJpg() && !fillCaptureFailed) shown=drawTransformedFrame(mjpeg.getFrameWidth(),mjpeg.getFrameHeight(),rotation,config.displayMode==DisplayMode::Fill);
      if(!shown && fillCaptureFailed){
        if(!transformFallbackWarningShown){Serial.printf("WARNING: Transform source %d x %d is too large; using unrotated fit mode for this file\n",mjpeg.getFrameWidth(),mjpeg.getFrameHeight());transformFallbackWarningShown=true;}
        fillCaptureEnabled=false;mjpeg.setForceNative(false);shown=mjpeg.drawJpg();
      }
      else if(!shown) Serial.println("WARNING: Rejected malformed JPEG frame");
    } else { fillCaptureEnabled=false;mjpeg.setForceNative(false); shown=mjpeg.drawJpg(); }
    if(shown){++totalFrames;nextFrameAt=currentMs+framePeriodMs;}
    else if((config.displayMode!=DisplayMode::Fill && rotation == 0) || !fillFrameBuffer) Serial.println("WARNING: Rejected malformed JPEG frame");
    totalDecodeVideo+=millis()-currentMs;currentMs=millis();
  }
  unsigned long used=millis()-start, decode=totalDecodeVideo>=totalShowVideo?totalDecodeVideo-totalShowVideo:0;file.close();Serial.println("MJPEG end");Serial.printf("Total frames: %d\n",totalFrames);if(mjpeg.getOversizeFrameCount())Serial.printf("Skipped oversized frames: %lu (buffer: %u bytes)\n",(unsigned long)mjpeg.getOversizeFrameCount(),(unsigned)mjpegBufferSize);Serial.printf("Time used: %lu ms\nAverage FPS: %0.1f\n",used,used?1000.0f*totalFrames/used:0.0f);if(used)Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\nDecode video: %lu ms (%0.1f %%)\nShow video: %lu ms (%0.1f %%)\n",totalReadVideo,100.0f*totalReadVideo/used,decode,100.0f*decode/used,totalShowVideo,100.0f*totalShowVideo/used);
}
void setup() {
  Serial.begin(115200, SERIAL_8N1, 44, 43); delay(500); Serial.println("Open Media Player S3 starting"); if(!gfx->begin(8000000)){Serial.println("Display initialization failed!");while(true)delay(1000);}gfx->fillScreen(RGB565_BLACK);
  pinMode(SDMMC_CS_PIN,OUTPUT);digitalWrite(SDMMC_CS_PIN,HIGH);SD_MMC.setPins(SDMMC_CLK_PIN,SDMMC_CMD_PIN,SDMMC_D0_PIN);
  if(!SD_MMC.begin("/sdcard",true,false,8000000)){Serial.println("ERROR: File system mount failed!");while(true)delay(1000);}
  loadConfiguration();setBrightness();pinMode(config.buttonPin,config.buttonActiveLow?INPUT_PULLUP:INPUT_PULLDOWN);mjpegBufferSize=(size_t)config.bufferKb*1024U;mjpegBuffer=(uint8_t*)heap_caps_malloc(mjpegBufferSize,MALLOC_CAP_8BIT);if(!mjpegBuffer){Serial.printf("MJPEG buffer allocation failed (%u KB)!\n",config.bufferKb);while(true)delay(1000);}randomSeed(micros());loadMjpegFiles();bool needsFrameBuffer=config.displayMode==DisplayMode::Fill;for(int i=0;i<mjpegCount;++i)if(mjpegFileRotations[i])needsFrameBuffer=true;if(needsFrameBuffer){fillFrameBuffer=(uint16_t*)heap_caps_malloc((size_t)FILL_SOURCE_MAX_PIXELS*sizeof(uint16_t),MALLOC_CAP_8BIT);if(!fillFrameBuffer){config.displayMode=DisplayMode::Fit;Serial.println("WARNING: Transform buffer unavailable; using unrotated fit mode");}}
}
void loop() {
  if(!mjpegCount){delay(1000);return;}String path=config.mjpegFolder+"/"+mjpegFileList[currentMjpegIndex];Serial.printf("Playing %s (rotation %u)\n",path.c_str(),mjpegFileRotations[currentMjpegIndex]);playFile(path,mjpegFileRotations[currentMjpegIndex]);
  if(config.playbackMode==PlaybackMode::Sequential)currentMjpegIndex=(currentMjpegIndex+1)%mjpegCount;else if(config.playbackMode==PlaybackMode::Shuffle&&mjpegCount>1){int next=currentMjpegIndex;while(next==currentMjpegIndex)next=random(mjpegCount);currentMjpegIndex=next;}
}
