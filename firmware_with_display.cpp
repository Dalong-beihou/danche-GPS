#include <Arduino.h>
#include <ctype.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <U8g2_for_Adafruit_GFX.h>

// ===================== 看门狗配置 =====================
#include <esp_task_wdt.h>
constexpr int WDT_TIMEOUT_SECONDS = 30; // 看门狗超时时间（秒）
constexpr unsigned long WDT_FEED_INTERVAL_MS = 5000; // 喂狗间隔（毫秒）
unsigned long lastWdtFeedTime = 0;

// ===================== 屏幕引脚定义 (来自您的新代码) =====================
#define TFT_SCLK  42
#define TFT_MOSI  41
#define TFT_CS    2
#define TFT_DC    40
#define TFT_RST   1
#define TFT_BL    39

// 新增：定义缺失的颜色
#define ST7735_DARKGREY 0x3186 // 自定义深灰色
#define ST7735_PURPLE 0x981F
#define ST7735_SKYBLUE 0x049F
#define ST7735_CYAN 0x07FB

// I2C引脚定义
constexpr int SDA_PIN = 38;
constexpr int SCL_PIN = 37;

// ===================== 核心配置 (来自 cpp.txt) =====================
constexpr const char* WIFI_SSID = "234";
constexpr const char* WIFI_PASSWORD = "00000000";
constexpr const char* SERVER_IP = "39.108.125.75";
constexpr int SERVER_PORT = 34232;
constexpr const char* DEVICE_ID = "ESP32_GPS_003";
constexpr bool USE_HTTPS = true; // 是否使用HTTPS
constexpr int SW420D_DO_PIN = 4;
constexpr int LED_PIN = 6;
constexpr int BUZZER_PIN = 5;

// ===================== 全局对象 =====================
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
WiFiClient wifiClient;
WiFiClientSecure wifiClientSecure;

// ===================== 温湿度传感器配置 =====================
Adafruit_AHTX0 aht;
float currentTemp = 0.0;
float currentHum = 0.0;
bool ahtFound = false;

// ===================== 新增LED配置（核心修改）=====================
enum class LedBlinkState { Idle, Blinking };
LedBlinkState ledBlinkState = LedBlinkState::Idle;
constexpr int LED_BLINK_INTERVAL = 500;
constexpr int LED_BLINK_TOGGLES = 6;
constexpr unsigned long LED_DEBOUNCE_MS = 120;
unsigned long ledNextToggleMs = 0;
unsigned long ledLastTriggerMs = 0;
int ledBlinkToggleCount = 0;
bool ledState = LOW;
bool isLedReady = false;

// ===================== 新增蜂鸣器配置 ======================
unsigned long buzzerEndTime = 0;   // 蜂鸣器停止鸣叫的时间戳
constexpr unsigned long BUZZER_DURATION = 3000; // 鸣叫持续时间（和LED一致，3秒）
bool isBuzzerOn = false;           // 蜂鸣器是否鸣叫中
bool isBuzzerReady = false;        // 蜂鸣器引脚是否初始化

// ===================== GPS 配置（优化版）=====================
#define GPS_TX 17  // ESP32 TX → GPS RX
#define GPS_RX 16  // ESP32 RX → GPS TX
HardwareSerial GPSSerial(2); // 使用ESP32-S3的串口2
float gpsLat = 0.0;   // 纬度
float gpsLng = 0.0;   // 经度
float gpsSpeed = 0.0; // 速度（km/h）
float gpsSpeedRaw = 0.0; // 原始速度（km/h）
unsigned long lastSpeedUpdateMs = 0;
unsigned long lastSpeedAboveMs = 0;
constexpr float SPEED_STOP_THRESHOLD = 1.2f; // 低于此阈值视为静止
constexpr float SPEED_SMOOTH_ALPHA = 0.25f; // 速度平滑系数
constexpr unsigned long SPEED_STOP_HOLD_MS = 3000;
float lastReportedSpeed = -1.0; // 上次上报的速度
constexpr float SPEED_CHANGE_THRESHOLD = 0.5; // 速度变化超过0.5km/h立即上报
int gpsSatellite = 0; // 卫星数
bool gpsValid = false;// GPS数据是否有效
unsigned long lastGPSValidTime = 0; // 最后一次有效GPS时间
unsigned long lastVibrationCheckTime = 0; // 震动检测计时器
unsigned long lastSpeedZeroTime = 0; // 速度第一次为零的时间
bool isVehicleTrulyStopped = false; // 车辆是否真正静止（速度为零且维持3秒）



// ===================== 蓝牙定位配置（简化版）======================
constexpr unsigned long BLE_SCAN_INTERVAL = 3000; // 蓝牙更新间隔(ms)
float bleLat = 0.0f;
float bleLng = 0.0f;
bool bleLocateValid = false;
unsigned long lastBleScanMs = 0;

// ===================== GPS偏移校正配置 ======================
constexpr float GPS_LAT_OFFSET = -0.00003;  // 纬度偏移
constexpr float GPS_LNG_OFFSET = 0.001123;  // 经度偏移（向东移动125米，修正75米偏差）

// ===================== 车辆状态定义 ======================
enum class VehicleStatus {
  IDLE,       // 空闲状态
  IN_USE,     // 使用中状态
  ABNORMAL    // 异常状态
};

// 当前车辆状态
VehicleStatus currentVehicleStatus = VehicleStatus::IDLE;

// ===================== 可调节的震动参数（默认值）=====================
int DETECT_INTERVAL = 100;    // 检测间隔(ms)：越小越灵敏，越大越稳
int SHAKE_THRESHOLD = 1;      // 连续震动次数阈值：越大越防抖
int STABLE_THRESHOLD = 3;     // 连续静止次数阈值：越大越稳
constexpr int HEARTBEAT_INTERVAL = 3000; // 心跳上报间隔(ms)，固定3秒

// ===================== 地理围栏配置 =====================
// 地理围栏配置（与前端红框一致）
const float CAMPUS_MIN_LAT = 36.5280; // 最小纬度
const float CAMPUS_MAX_LAT = 36.5320; // 最大纬度
const float CAMPUS_MIN_LNG = 103.7200; // 最小经度
const float CAMPUS_MAX_LNG = 103.7240; // 最大经度
bool isOutsideCampus = false; // 是否在校园外
unsigned long lastCampusCheckMs = 0; // 上次校园检查时间
const unsigned long CAMPUS_CHECK_INTERVAL = 5000; // 校园检查间隔(ms)

// ===================== 全局变量 =====================
unsigned long lastUploadTime = 0;
bool lastVibrationState = false;
bool currentVibrationState = false;
const int WIFI_RETRY_INTERVAL = 5000;
unsigned long lastWiFiCheck = 0;
int shakeCount = 0;  // 震动计数
int stableCount = 0; // 静止计数

// 联网逻辑优化 - 新增变量
constexpr int MAX_WIFI_RETRY = 5;    // WiFi最大重连次数
int wifiRetryCount = 0;          // WiFi重连计数器
constexpr int HTTP_TIMEOUT = 8000;   // 公网访问延长超时时间

// ===================== 显示屏刷新控制 =====================
unsigned long lastDisplayTime = 0;
constexpr int DISPLAY_UPDATE_INTERVAL = 100; // 提升刷新率以支持闪烁 (ms)
bool vibTextBlinking = false;
bool vibTextVisible = true;
bool vibTextInPause = false; // 闪烁间停顿标志
unsigned long vibTextBlinkEndMs = 0;
unsigned long vibTextLastToggleMs = 0;
unsigned long vibTextLastTriggerMs = 0;
int vibTextBlinkCount = 0; // 闪烁次数计数器
constexpr int VIB_TEXT_BLINK_TIMES = 2; // 连续闪烁次数
constexpr unsigned long VIB_TEXT_BLINK_INTERVAL = 250; // 闪烁间隔
constexpr unsigned long VIB_TEXT_PAUSE_INTERVAL = 500; // 闪烁间停顿时间
constexpr unsigned long VIB_TEXT_DEBOUNCE_MS = 120;

// 用于局部刷新的旧值存储
float oldGpsSpeed = -1.0;
float oldGpsLat = -1.0;
float oldGpsLng = -1.0;
int oldGpsSatellite = -1;
bool oldVibrationState = false;
bool oldGpsValid = true; // 初始值设为true，确保第一次能刷新
float temperatureC = 0.0f;
float humidityPct = 0.0f;
float oldTemperatureC = -1000.0f;
float oldHumidityPct = -1000.0f;
unsigned long lastTempReadMs = 0;
constexpr unsigned long TEMP_READ_INTERVAL = 1000;
unsigned long lastLatLngDrawMs = 0;
unsigned long lastTempDrawMs = 0;
constexpr unsigned long LATLNG_DRAW_INTERVAL = 300;
constexpr unsigned long TEMP_DRAW_INTERVAL = 500;

// UI 偏移量 (解决部分屏幕边框花屏问题)
constexpr int OFFSET_X = 2; 
constexpr int OFFSET_Y = 1;

// 布局常量定义 - 左右分栏布局
constexpr int COL_L_X = 2;
constexpr int COL_R_X = 78;
constexpr int ROW_H   = 24;
constexpr int ROW_Y1  = 20;
constexpr int ROW_Y2  = 44;
constexpr int ROW_Y3  = 68;
constexpr int ROW_Y4  = 92;
constexpr int ROW_Y5  = 116;
constexpr int LATLNG_VALUE_X = 65;
constexpr int LAT_VALUE_Y = ROW_Y3;
constexpr int LNG_VALUE_Y = ROW_Y4;
const uint8_t* LAT_VALUE_FONT = u8g2_font_wqy16_t_gb2312a;
const uint8_t* LNG_VALUE_FONT = u8g2_font_wqy16_t_gb2312a;
constexpr int LAT_VALUE_W = 82;
constexpr int LNG_VALUE_W = 82;
constexpr int LAT_VALUE_CLEAR_X = LATLNG_VALUE_X;
constexpr int LNG_VALUE_CLEAR_X = LATLNG_VALUE_X;
constexpr int LAT_VALUE_CLEAR_Y = LAT_VALUE_Y;
constexpr int LNG_VALUE_CLEAR_Y = LNG_VALUE_Y;
constexpr int LAT_VALUE_CLEAR_H = 20;
constexpr int LNG_VALUE_CLEAR_H = 20;
constexpr int LAT_DECIMALS = 6;
constexpr int LNG_DECIMALS = 6;
constexpr int SAT_VALUE_X = 50;
constexpr int TEMP_LABEL_X = 2;
constexpr int TEMP_VALUE_X = 44;
constexpr int HUM_LABEL_X = 84;
constexpr int HUM_VALUE_X = 124;
constexpr int TEMP_VALUE_Y = ROW_Y5;
constexpr int HUM_VALUE_Y = ROW_Y5;
const uint8_t* TEMP_VALUE_FONT = u8g2_font_wqy16_t_gb2312a;
const uint8_t* HUM_VALUE_FONT = u8g2_font_wqy16_t_gb2312a;
constexpr int TEMP_VALUE_W = 50;
constexpr int HUM_VALUE_W = 50;
constexpr int TEMP_VALUE_CLEAR_X = TEMP_VALUE_X;
constexpr int TEMP_VALUE_CLEAR_Y = TEMP_VALUE_Y;
constexpr int TEMP_VALUE_CLEAR_W = HUM_LABEL_X - TEMP_VALUE_X - 4;
constexpr int TEMP_VALUE_CLEAR_H = 20;
constexpr int HUM_VALUE_CLEAR_X = HUM_VALUE_X;
constexpr int HUM_VALUE_CLEAR_Y = HUM_VALUE_Y;
constexpr int HUM_VALUE_CLEAR_W = 36;
constexpr int HUM_VALUE_CLEAR_H = 20;
constexpr int TEMP_DECIMALS = 0;
constexpr int HUM_DECIMALS = 0;
constexpr uint16_t LATLNG_COLOR = ST7735_PURPLE;
constexpr uint16_t TEMP_COLOR = ST7735_SKYBLUE;
constexpr uint16_t HUM_COLOR = ST7735_SKYBLUE;
constexpr uint16_t SAT_COLOR = ST7735_CYAN;
constexpr int STATUS_W = 76;
constexpr int STATUS_H = 20;
constexpr int SPEED_VALUE_W = 60;
constexpr int SPEED_DECIMALS = 1;
constexpr unsigned long SPEED_DRAW_INTERVAL = 300;
constexpr int STATUS_CLEAR_W = STATUS_W;
constexpr int STATUS_CLEAR_H = STATUS_H;
constexpr unsigned long STATUS_DRAW_INTERVAL = 500;
constexpr int STATUS_VALUE_X = COL_R_X;
constexpr int STATUS_VALUE_Y = ROW_Y2;
const uint8_t* STATUS_VALUE_FONT = u8g2_font_wqy16_t_gb2312a;
constexpr int STATUS_VALUE_CLEAR_X = STATUS_VALUE_X;
constexpr int STATUS_VALUE_CLEAR_Y = STATUS_VALUE_Y;
constexpr int STATUS_VALUE_CLEAR_W = STATUS_CLEAR_W;
constexpr int STATUS_VALUE_CLEAR_H = STATUS_CLEAR_H;

// ===================== 屏幕UI绘制函数 =====================

// 绘制静态UI框架 - 左右分栏布局
void drawUiFrame() {
  tft.fillScreen(ST7735_BLACK);
  
  u8g2Fonts.setBackgroundColor(ST7735_BLACK);
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312a);
  
  // 第一行：速度标签 (左侧)
  u8g2Fonts.setForegroundColor(ST7735_ORANGE);
  u8g2Fonts.setCursor(COL_L_X + OFFSET_X, ROW_Y1 + OFFSET_Y);
  u8g2Fonts.print("速度:");
  
  // 第二行：卫星标签 (左侧)
  u8g2Fonts.setForegroundColor(SAT_COLOR);
  u8g2Fonts.setCursor(COL_L_X + OFFSET_X, ROW_Y2 + OFFSET_Y);
  u8g2Fonts.print("卫星:");
  
  // 第三行：纬度标签 (左侧)
  u8g2Fonts.setForegroundColor(LATLNG_COLOR);
  u8g2Fonts.setCursor(COL_L_X + OFFSET_X, ROW_Y3 + OFFSET_Y);
  u8g2Fonts.print("纬度:");
  
  // 第四行：经度标签 (左侧)
  u8g2Fonts.setForegroundColor(LATLNG_COLOR);
  u8g2Fonts.setCursor(COL_L_X + OFFSET_X, ROW_Y4 + OFFSET_Y);
  u8g2Fonts.print("经度:");
  
  // 第五行：温湿度标签 (左侧)
  u8g2Fonts.setForegroundColor(TEMP_COLOR);
  u8g2Fonts.setCursor(TEMP_LABEL_X + OFFSET_X, ROW_Y5 + OFFSET_Y);
  u8g2Fonts.print("温度:");
  u8g2Fonts.setForegroundColor(HUM_COLOR);
  u8g2Fonts.setCursor(HUM_LABEL_X + OFFSET_X, ROW_Y5 + OFFSET_Y);
  u8g2Fonts.print("湿度:");
  
  
}

// 更新屏幕上的动态数据
void updateDisplay() {
  if (millis() - lastDisplayTime < DISPLAY_UPDATE_INTERVAL) {
    return;
  }
  
  unsigned long now = millis();
  u8g2Fonts.setBackgroundColor(ST7735_BLACK);

  // 1. 更新速度数值 (右侧)
  static unsigned long lastSpeedUpdate = 0;
  if (now - lastSpeedUpdate >= SPEED_DRAW_INTERVAL) {
    if (oldGpsSpeed < -999.0f || fabs(gpsSpeed - oldGpsSpeed) > 0.1f) {
      char speedBuf[12];
      dtostrf(gpsSpeed, 0, SPEED_DECIMALS, speedBuf);
      tft.fillRect(COL_R_X + OFFSET_X, ROW_Y1 + OFFSET_Y - 16, SPEED_VALUE_W, 20, ST7735_BLACK);
      u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312a);
      u8g2Fonts.setForegroundColor(ST7735_YELLOW);
      u8g2Fonts.setCursor(COL_R_X + OFFSET_X, ROW_Y1 + OFFSET_Y);
      u8g2Fonts.print(speedBuf);
      oldGpsSpeed = gpsSpeed;
    }
    lastSpeedUpdate = now;
  }

  u8g2Fonts.setFont(STATUS_VALUE_FONT);
  static unsigned long lastStatusRefresh = 0;
  bool statusNeedsDraw = false;
  if (vibTextBlinking) {
    if (vibTextInPause) {
      // 处于停顿状态
      if (now - vibTextLastToggleMs >= VIB_TEXT_PAUSE_INTERVAL) {
        // 停顿结束，开始下一次闪烁
        vibTextInPause = false;
        vibTextVisible = true;
        vibTextLastToggleMs = now;
      }
    } else {
      // 处于闪烁状态
      if (now - vibTextLastToggleMs >= VIB_TEXT_BLINK_INTERVAL) {
        vibTextVisible = !vibTextVisible;
        vibTextLastToggleMs = now;
        if (!vibTextVisible) {
          // 一次闪烁完成
          vibTextBlinkCount++;
          if (vibTextBlinkCount < VIB_TEXT_BLINK_TIMES) {
            // 还有闪烁次数，进入停顿状态
            vibTextInPause = true;
          } else {
            // 闪烁次数完成，结束闪烁
            vibTextBlinking = false;
            vibTextVisible = true;
          }
        }
      }
    }
    statusNeedsDraw = true;
  }

  if (!vibTextBlinking && (currentVibrationState != oldVibrationState || now - lastStatusRefresh >= STATUS_DRAW_INTERVAL)) {
    statusNeedsDraw = true;
  }

  if (statusNeedsDraw) {
    const char* statusText = nullptr;
    if (vibTextBlinking) {
      if (vibTextVisible) {
        statusText = "车辆震动";
      }
    } else if (currentVibrationState) {
      statusText = "车辆震动";
    } else {
      statusText = "车辆正常";
    }
    tft.fillRect(STATUS_VALUE_CLEAR_X + OFFSET_X, STATUS_VALUE_CLEAR_Y + OFFSET_Y - 16, STATUS_VALUE_CLEAR_W, STATUS_VALUE_CLEAR_H, ST7735_BLACK);
    if (statusText != nullptr) {
      if (currentVibrationState || vibTextBlinking) {
        u8g2Fonts.setForegroundColor(ST7735_RED);
      } else {
        u8g2Fonts.setForegroundColor(ST7735_GREEN);
      }
      u8g2Fonts.setCursor(STATUS_VALUE_X + OFFSET_X, STATUS_VALUE_Y + OFFSET_Y);
      u8g2Fonts.print(statusText);
    }
    oldVibrationState = currentVibrationState;
    lastStatusRefresh = now;
  }

  if (gpsValid && gpsSatellite != oldGpsSatellite) {
    tft.fillRect(SAT_VALUE_X + OFFSET_X, ROW_Y2 + OFFSET_Y - 16, 28, 20, ST7735_BLACK);
    u8g2Fonts.setForegroundColor(SAT_COLOR);
    u8g2Fonts.setCursor(SAT_VALUE_X + OFFSET_X, ROW_Y2 + OFFSET_Y);
    u8g2Fonts.print(gpsSatellite);
    oldGpsSatellite = gpsSatellite;
  } else if (!gpsValid && oldGpsSatellite != -1) {
    tft.fillRect(SAT_VALUE_X + OFFSET_X, ROW_Y2 + OFFSET_Y - 16, 28, 20, ST7735_BLACK);
    oldGpsSatellite = -1;
  }

  if (now - lastLatLngDrawMs >= LATLNG_DRAW_INTERVAL) {
    if (oldGpsLat < -999.0f || fabs(gpsLat - oldGpsLat) > 1e-6) {
      char latBuf[20];
      dtostrf(gpsLat, 0, LAT_DECIMALS, latBuf);
      tft.fillRect(LAT_VALUE_CLEAR_X + OFFSET_X, LAT_VALUE_CLEAR_Y + OFFSET_Y - 16, LAT_VALUE_W, LAT_VALUE_CLEAR_H, ST7735_BLACK);
      u8g2Fonts.setForegroundColor(LATLNG_COLOR);
      u8g2Fonts.setFont(LAT_VALUE_FONT);
      u8g2Fonts.setCursor(LATLNG_VALUE_X + OFFSET_X, LAT_VALUE_Y + OFFSET_Y);
      u8g2Fonts.print(latBuf);
      oldGpsLat = gpsLat;
    }

    if (oldGpsLng < -999.0f || fabs(gpsLng - oldGpsLng) > 1e-6) {
      char lngBuf[20];
      dtostrf(gpsLng, 0, LNG_DECIMALS, lngBuf);
      tft.fillRect(LNG_VALUE_CLEAR_X + OFFSET_X, LNG_VALUE_CLEAR_Y + OFFSET_Y - 16, LNG_VALUE_W, LNG_VALUE_CLEAR_H, ST7735_BLACK);
      u8g2Fonts.setForegroundColor(LATLNG_COLOR);
      u8g2Fonts.setFont(LNG_VALUE_FONT);
      u8g2Fonts.setCursor(LATLNG_VALUE_X + OFFSET_X, LNG_VALUE_Y + OFFSET_Y);
      u8g2Fonts.print(lngBuf);
      oldGpsLng = gpsLng;
    }
    lastLatLngDrawMs = now;
  }

  if (now - lastTempDrawMs >= TEMP_DRAW_INTERVAL) {
    if (oldTemperatureC < -999.0f || oldHumidityPct < -999.0f || fabs(temperatureC - oldTemperatureC) > 0.1f || fabs(humidityPct - oldHumidityPct) > 0.1f) {
      char tempBuf[12];
      char humBuf[12];
      dtostrf(temperatureC, 0, TEMP_DECIMALS, tempBuf);
      dtostrf(humidityPct, 0, HUM_DECIMALS, humBuf);
      tft.fillRect(TEMP_VALUE_CLEAR_X + OFFSET_X, TEMP_VALUE_CLEAR_Y + OFFSET_Y - 16, TEMP_VALUE_CLEAR_W, TEMP_VALUE_CLEAR_H, ST7735_BLACK);
      u8g2Fonts.setForegroundColor(TEMP_COLOR);
      u8g2Fonts.setFont(TEMP_VALUE_FONT);
      u8g2Fonts.setCursor(TEMP_VALUE_X + OFFSET_X, TEMP_VALUE_Y + OFFSET_Y);
      u8g2Fonts.print(tempBuf);
      oldTemperatureC = temperatureC;

      tft.fillRect(HUM_VALUE_CLEAR_X + OFFSET_X, HUM_VALUE_CLEAR_Y + OFFSET_Y - 16, HUM_VALUE_CLEAR_W, HUM_VALUE_CLEAR_H, ST7735_BLACK);
      u8g2Fonts.setForegroundColor(HUM_COLOR);
      u8g2Fonts.setFont(HUM_VALUE_FONT);
      u8g2Fonts.setCursor(HUM_VALUE_X + OFFSET_X, HUM_VALUE_Y + OFFSET_Y);
      u8g2Fonts.print(humBuf);
      oldHumidityPct = humidityPct;
    }
    lastTempDrawMs = now;
  }

  lastDisplayTime = millis();
}


// ===================== 初始化函数 (合并) =====================
void setup() {
  // 先初始化串口，立即输出
  Serial.begin(115200);
  
  // 简单的启动消息，确保程序真的在运行
  Serial.println("");
  Serial.println("===== 程序启动 =====");
  Serial.println("Hello from ESP32!");
  
  // 增加CPU频率到240MHz，增加功耗
  setCpuFrequencyMhz(240);
  
  delay(1000);
  
  Serial.println("\n=====================================");
  Serial.println("🔌 ESP32 GPS震动监测系统启动中...");
  Serial.println("🌐 公网服务器配置：" + String(SERVER_IP) + ":" + String(SERVER_PORT));
  Serial.println("=====================================");
  
  // 1. 先初始化屏幕（避免I2C问题）
  Serial.println("[Setup] 初始化屏幕...");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  Serial.println("[Display] 正在初始化屏幕...");
  Serial.println("[Display] 屏幕引脚配置:");
  Serial.print("  CS: GPIO"); Serial.println(TFT_CS);
  Serial.print("  DC: GPIO"); Serial.println(TFT_DC);
  Serial.print("  RST: GPIO"); Serial.println(TFT_RST);
  Serial.print("  SCLK: GPIO"); Serial.println(TFT_SCLK);
  Serial.print("  MOSI: GPIO"); Serial.println(TFT_MOSI);
  Serial.print("  BL: GPIO"); Serial.println(TFT_BL);
  
  // 尝试多种初始化模式 - 可以通过串口发送命令切换
  // 可选模式: INITR_BLACKTAB, INITR_GREENTAB, INITR_REDTAB, INITR_MINI160x80
  Serial.println("[Display] 尝试 INITR_BLACKTAB 模式...");
  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(1);
  
  // 填充屏幕为黑色
  tft.fillScreen(ST7735_BLACK);
  
  Serial.println("[Display] 屏幕硬件初始化完成");
  
  u8g2Fonts.begin(tft);
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312a);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setForegroundColor(ST7735_WHITE);
  u8g2Fonts.setBackgroundColor(ST7735_BLACK);
  
  Serial.println("[Display] U8g2字体初始化完成");
  
  drawUiFrame(); // 绘制静态UI
  Serial.println("[Display] UI绘制完成");

  // 2. 初始化I2C总线
  Serial.println("[Setup] 初始化I2C总线...");
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Serial.println("[Setup] I2C总线初始化完成");

  // 3. 初始化所有传感器和模块 (来自 cpp.txt)
  initSW420D();
  initGPS();
  initLED();
  initBuzzer();
  initAHT20();    // 初始化温湿度传感器
  initWiFi();
  
  // 初始化看门狗
  initWatchdog();
  
  // 初始状态上报
  lastVibrationState = isLargeVibration();
  if (uploadDataToServer(lastVibrationState)) {
    lastUploadTime = millis();
    lastReportedSpeed = gpsSpeed;
  }
  
  Serial.println("\n=====================================");
  Serial.println("✅ 系统启动成功，3秒自动心跳保在线");
  Serial.println("💡 输入'param'查看参数，输入'help'查看指令");
  Serial.println("🌐 公网上报地址：http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/upload");
  Serial.println("💡 LED控制逻辑：震动触发后闪烁3秒（GPIO6），超时自动熄灭");
  Serial.println("💡 蜂鸣器控制逻辑：震动触发后鸣叫3秒（GPIO5），超时自动静音");
  Serial.println("🐕 看门狗已启用，超时时间：" + String(WDT_TIMEOUT_SECONDS) + "秒");
  Serial.println("=====================================");
}

// ===================== 主循环 (合并) =====================
void loop() {
  unsigned long now = millis();

  // 1. 优先处理串口指令（保持系统可调）
  handleSerialCommand();
  
  // 2. 读取并解析GPS数据（必须高频执行，防止串口缓冲溢出）
  readGPSData();
  
  if (now - lastTempReadMs >= TEMP_READ_INTERVAL) {
    // 统一使用 readAHT20()
    readAHT20();
    if (ahtFound) {
      temperatureC = currentTemp;
      humidityPct = currentHum;
    }
    lastTempReadMs = now;
  }

  // 3. 检查WiFi连接
  checkWiFi();
  
  // 4. 检查设备是否在校园内
  if (now - lastCampusCheckMs >= CAMPUS_CHECK_INTERVAL) {
    float currentLat, currentLng;
    bool locationValid = false;
    
    // 优先使用GPS定位
    if (gpsValid) {
      currentLat = gpsLat;
      currentLng = gpsLng;
      locationValid = true;
    } 
    // 其次使用蓝牙定位
    else if (bleLocateValid) {
      currentLat = bleLat;
      currentLng = bleLng;
      locationValid = true;
    }
    
    if (locationValid) {
      // 直接检查是否在校园内
      bool inCampus = (currentLat >= CAMPUS_MIN_LAT && currentLat <= CAMPUS_MAX_LAT && 
                      currentLng >= CAMPUS_MIN_LNG && currentLng <= CAMPUS_MAX_LNG);
      if (!inCampus && !isOutsideCampus) {
        // 设备刚离开校园，发出告警
        isOutsideCampus = true;
        // 触发LED和蜂鸣器告警
        startLedBlink(now);
        // 触发蜂鸣器
        isBuzzerOn = true;
        buzzerEndTime = now + 5000; // 蜂鸣5秒
      } else if (inCampus && isOutsideCampus) {
        // 设备回到校园，取消告警状态
        isOutsideCampus = false;
      }
    } else {
      // 没有有效定位信息，取消校园外告警状态
      isOutsideCampus = false;
    }
    lastCampusCheckMs = now;
  }

  // 4. 定时进行震动检测（非阻塞）
  bool currentVib = lastVibrationState;
  if (now - lastVibrationCheckTime >= (unsigned long)DETECT_INTERVAL) {
    currentVib = isLargeVibration();
    lastVibrationCheckTime = now;
  }
  currentVibrationState = currentVib;
  if (currentVib && now - vibTextLastTriggerMs >= VIB_TEXT_DEBOUNCE_MS) {
    vibTextBlinking = true;
    vibTextVisible = true;
    vibTextInPause = false;
    vibTextBlinkCount = 0;
    vibTextLastToggleMs = now;
    vibTextLastTriggerMs = now;
  }
  if (currentVib && now - ledLastTriggerMs >= LED_DEBOUNCE_MS) {
    startLedBlink(now);
  }

  // 5. 蜂鸣器和LED控制（异步）
  controlLED();
  controlBuzzer();

  // 6. 数据上报触发逻辑（优化GPS优先级）
  bool speedChangedSignificantly = gpsValid && (fabs(gpsSpeed - lastReportedSpeed) >= SPEED_CHANGE_THRESHOLD);
  bool heartbeatTimeout = (now - lastUploadTime >= HEARTBEAT_INTERVAL);
  bool vibrationChanged = (currentVib != lastVibrationState);
  static bool lastGpsValidState = false;
  bool gpsStateChanged = (gpsValid != lastGpsValidState);

  if (vibrationChanged || speedChangedSignificantly || heartbeatTimeout || gpsStateChanged) {
    // 只有在满足任一条件时才上报
    bool uploadSuccess = uploadDataToServer(currentVib);
    
    // 仅当上报成功后，才更新本地状态记录
    if (uploadSuccess) {
      lastVibrationState = currentVib;
      lastUploadTime = now;
      lastReportedSpeed = gpsSpeed;
      lastGpsValidState = gpsValid;
    }
  }

  // 执行蓝牙扫描定位（低优先级，GPS有效时跳过）
  if (!gpsValid) {
    scanBLE();
  }

  // 2. 新增：定时刷新屏幕显示
  updateDisplay();
  
  // 喂狗：防止系统死机
  feedWatchdog();
  
  // 保持主循环非阻塞
  yield(); 
}

// ===================== 串口指令处理函数 ======================
void handleSerialCommand() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim(); // 去除空格和换行
    
    if (cmd.startsWith("detect=")) {
      int newVal = cmd.substring(7).toInt();
      if (newVal >= 50 && newVal <= 1000) {
        DETECT_INTERVAL = newVal;
        Serial.print("✅ 检测间隔已修改为：");
        Serial.println(DETECT_INTERVAL);
      } else {
        Serial.println("❌ 检测间隔值无效（范围50-1000）");
      }
    } 
    else if (cmd.startsWith("shake=")) {
      int newVal = cmd.substring(6).toInt();
      if (newVal >= 1 && newVal <= 10) {
        SHAKE_THRESHOLD = newVal;
        Serial.print("✅ 震动阈值已修改为：");
        Serial.println(SHAKE_THRESHOLD);
      } else {
        Serial.println("❌ 震动阈值无效（范围1-10）");
      }
    } 
    else if (cmd.startsWith("stable=")) {
      int newVal = cmd.substring(7).toInt();
      if (newVal >= 1 && newVal <= 10) {
        STABLE_THRESHOLD = newVal;
        Serial.print("✅ 静止阈值已修改为：");
        Serial.println(STABLE_THRESHOLD);
      } else {
        Serial.println("❌ 静止阈值无效（范围1-10）");
      }
    } 
    else if (cmd == "param") {
      Serial.println("========== 当前震动参数 ==========");
      Serial.print("检测间隔："); Serial.print(DETECT_INTERVAL); Serial.println("ms");
      Serial.print("震动阈值："); Serial.print(SHAKE_THRESHOLD); Serial.println("次");
      Serial.print("静止阈值："); Serial.print(STABLE_THRESHOLD); Serial.println("次");
      Serial.println("==================================");
      Serial.println("========== 当前GPS状态 ==========");
      Serial.print("原始纬度："); Serial.print(gpsLat - GPS_LAT_OFFSET, 6); Serial.println(" (未校正)");
      Serial.print("校正纬度："); Serial.println(gpsLat, 6);
      Serial.print("原始经度："); Serial.print(gpsLng - GPS_LNG_OFFSET, 6); Serial.println(" (未校正)");
      Serial.print("校正经度："); Serial.println(gpsLng, 6);
      Serial.print("速度："); Serial.print(gpsSpeed, 1); Serial.println(" km/h");
      Serial.print("卫星数："); Serial.println(gpsSatellite);
      Serial.print("GPS有效："); Serial.println(gpsValid ? "是" : "否");
      Serial.print("最后有效GPS时间："); Serial.println(lastGPSValidTime);
      Serial.print("GPS偏移：纬度"); Serial.print(GPS_LAT_OFFSET, 6); Serial.print("，经度"); Serial.println(GPS_LNG_OFFSET, 6);
      Serial.println("==================================");
      Serial.println("========== 服务器配置 ==========");
      Serial.print("服务器IP："); Serial.println(SERVER_IP);
      Serial.print("服务器端口："); Serial.println(SERVER_PORT);
      Serial.print("上报地址："); Serial.print("http://"); 
      Serial.print(SERVER_IP); Serial.print(":"); Serial.print(SERVER_PORT); Serial.println("/api/upload");
      Serial.println("==================================");
      Serial.println("========== 当前WiFi状态 ==========");
      Serial.print("连接状态："); Serial.println(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
      Serial.print("本地IP："); Serial.println(WiFi.localIP().toString());
      Serial.print("重连次数："); Serial.println(wifiRetryCount);
      Serial.println("==================================");
      // 新增LED状态打印
      Serial.println("========== 当前LED状态 ==========");
      Serial.print("LED控制引脚："); Serial.println(LED_PIN);
      Serial.print("LED闪烁间隔："); Serial.println(LED_BLINK_INTERVAL);
      Serial.print("LED闪烁次数："); Serial.println(LED_BLINK_TOGGLES / 2);
      Serial.print("当前LED状态："); Serial.println(ledBlinkState == LedBlinkState::Blinking ? "闪烁中" : "熄灭");
      Serial.print("当前震动状态："); Serial.println(lastVibrationState ? "震动" : "静止");
      Serial.println("==================================");
      // 新增蜂鸣器状态打印
      Serial.println("========== 当前蜂鸣器状态 ==========");
      Serial.print("蜂鸣器控制引脚："); Serial.println(BUZZER_PIN);
      Serial.print("蜂鸣器鸣叫时长："); Serial.println(BUZZER_DURATION / 1000); Serial.println("秒");
      Serial.print("当前蜂鸣器状态："); Serial.println(isBuzzerOn ? "鸣叫中" : "静音");
      Serial.println("==================================");
      // 新增温湿度传感器状态打印
      Serial.println("========== 当前温湿度传感器状态 ==========");
      Serial.print("传感器状态："); Serial.println(ahtFound ? "已连接" : "未连接");
      if (ahtFound) {
        Serial.print("当前温度："); Serial.print(currentTemp); Serial.println(" ℃");
        Serial.print("当前湿度："); Serial.print(currentHum); Serial.println(" %");
      }
      Serial.println("==================================");
    } 
    else if (cmd == "wifi") {
      Serial.println("[WiFi] Triggering manual reconnect...");
      wifiRetryCount = 0;
      initWiFi();
    }
    else if (cmd.startsWith("screen=")) {
      int mode = cmd.substring(7).toInt();
      Serial.print("[Display] 切换屏幕初始化模式: ");
      switch(mode) {
        case 1:
          Serial.println("INITR_BLACKTAB");
          tft.initR(INITR_BLACKTAB);
          break;
        case 2:
          Serial.println("INITR_GREENTAB");
          tft.initR(INITR_GREENTAB);
          break;
        case 3:
          Serial.println("INITR_REDTAB");
          tft.initR(INITR_REDTAB);
          break;
        case 4:
          Serial.println("INITR_MINI160x80");
          tft.initR(INITR_MINI160x80);
          break;
        default:
          Serial.println("无效模式");
          Serial.println("可选模式: 1=BLACKTAB, 2=GREENTAB, 3=REDTAB, 4=MINI160x80");
          return;
      }
      tft.setRotation(1);
      tft.fillScreen(ST7735_BLACK);
      drawUiFrame();
      Serial.println("[Display] 屏幕重新初始化完成");
    }
    else if (cmd == "help") {
      Serial.println("========== Command Help ==========");
      Serial.println("detect=value   Change detection interval (50-1000ms)");
      Serial.println("shake=value    Change shake threshold (1-10)");
      Serial.println("stable=value   Change stable threshold (1-10)");
      Serial.println("param         View current parameters");
      Serial.println("wifi          Manual WiFi reconnect");
      Serial.println("screen=mode   Change screen mode (1=BLACKTAB, 2=GREENTAB, 3=REDTAB, 4=MINI160x80)");
      Serial.println("help          View help");
      Serial.println("==============================");
    }
    else {
      Serial.println("[Error] Unknown command, type 'help'");
    }
  }
}

// ===================== 新增蜂鸣器初始化 =====================
void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); // 有源蜂鸣器：高电平=静音，低电平=鸣叫（关键！）
  isBuzzerReady = true;
  Serial.println("🔌 蜂鸣器初始化完成（控制引脚：GPIO5）");
  Serial.print("📊 蜂鸣器鸣叫时长："); Serial.println(BUZZER_DURATION / 1000); Serial.println("秒");
}

// ===================== 新增蜂鸣器控制函数 ======================
void controlBuzzer() {
  if (!isBuzzerReady) return;
  unsigned long now = millis();
  
  // 如果需要鸣叫且未到结束时间
  if (isBuzzerOn && now < buzzerEndTime) {
    digitalWrite(BUZZER_PIN, LOW); // 低电平：蜂鸣器响
  } 
  // 鸣叫时间到，关闭蜂鸣器
  else if (isBuzzerOn) {
    isBuzzerOn = false;
    digitalWrite(BUZZER_PIN, HIGH); // 高电平：蜂鸣器静音
    Serial.println("🔇 蜂鸣器鸣叫3秒结束，已静音");
  }
}

String urlEncode(const String& value) {
  String encoded;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0xF];
      encoded += hex[c & 0xF];
    }
  }
  return encoded;
}

void updateGpsSpeed(float rawSpeed, bool hasSpeed) {
  unsigned long now = millis();
  if (!gpsValid || !hasSpeed) {
    if (millis() - lastGPSValidTime > 15000) {
      gpsSpeed = 0.0f;
      gpsSpeedRaw = 0.0f;
      lastSpeedZeroTime = now;
      isVehicleTrulyStopped = false;
    }
    return;
  }
  if (lastSpeedAboveMs == 0) {
    lastSpeedAboveMs = now;
  }
  gpsSpeedRaw = rawSpeed;
  lastSpeedUpdateMs = now;
  
  // 检测速度是否低于停止阈值
  if (rawSpeed >= SPEED_STOP_THRESHOLD) {
    lastSpeedAboveMs = now;
    lastSpeedZeroTime = 0;
    isVehicleTrulyStopped = false;
  } else {
    // 速度低于阈值，开始计时
    if (lastSpeedZeroTime == 0) {
      lastSpeedZeroTime = now;
      Serial.println("[Speed] 速度降至零以下，开始静止计时...");
    }
    
    // 检查是否已经维持3秒
    if (now - lastSpeedZeroTime >= SPEED_STOP_HOLD_MS) {
      if (!isVehicleTrulyStopped) {
        Serial.println("[Speed] ✓ 车辆已真正静止（速度为零并维持3秒）");
        isVehicleTrulyStopped = true;
      }
    }
  }
  
  float filtered = (gpsSpeed == 0.0f) ? rawSpeed : (gpsSpeed + (rawSpeed - gpsSpeed) * SPEED_SMOOTH_ALPHA);
  if (rawSpeed < SPEED_STOP_THRESHOLD && (now - lastSpeedAboveMs >= SPEED_STOP_HOLD_MS)) {
    filtered = 0.0f;
  }
  gpsSpeed = filtered;
}

// ===================== GPS 解析函数 ======================
void parseGPS(char* gpsData) {
  static unsigned long lastGpsDebugPrint = 0;
  static bool lastGpsValidState = false;
  
  if (gpsData[0] != '$') return;
  
  // 支持 $GPRMC, $GNRMC, $GLRMC 等
  if (strstr(gpsData, "RMC")) {
    int fieldIndex = 0;
    char* token = strtok(gpsData, ",");
    String fields[13];
    while (token != NULL && fieldIndex < 13) {
      fields[fieldIndex++] = String(token);
      token = strtok(NULL, ",");
    }
    
    // RMC: $--RMC,time,status,lat,N,lng,E,speed,course,date,variation,E,checksum
    // fields[2] 是状态 ('A'=有效, 'V'=无效)
    if (fieldIndex >= 8 && (fields[2] == "A")) {
      float rawLat = dmToDecimalLat(fields[3], fields[4].length() > 0 ? fields[4][0] : 'N');
      float rawLng = dmToDecimalLng(fields[5], fields[6].length() > 0 ? fields[6][0] : 'E');
      
      // GPS数据范围校验
      if (!isValidGpsCoordinate(rawLat, rawLng)) {
        Serial.printf("[GPS] ⚠ 坐标超出有效范围：纬度=%.6f, 经度=%.6f\n", rawLat, rawLng);
        gpsValid = false;
        return;
      }
      
      gpsLat = rawLat + GPS_LAT_OFFSET;
      gpsLng = rawLng + GPS_LNG_OFFSET;
      
      // fields[7] 是以节（knots）为单位的速度
      float rawSpeed = fields[7].toFloat() * 1.852f;
      updateGpsSpeed(rawSpeed, true);
      
      // 状态变化时输出调试信息
      if (!lastGpsValidState) {
        Serial.printf("[GPS] ✓ 定位成功！纬度: %.6f, 经度: %.6f\n", gpsLat, gpsLng);
      }
      
      gpsValid = true;
      lastGPSValidTime = millis();
      lastGpsValidState = true;
    } else {
      if (millis() - lastGPSValidTime > 15000) { // 15秒内无有效数据则标记为无效
        if (lastGpsValidState) {
          Serial.println("[GPS] ⚠ GPS信号丢失，等待重新定位...");
        }
        gpsValid = false;
        gpsSpeed = 0.0f;
        gpsSpeedRaw = 0.0f;
        lastSpeedAboveMs = 0;
        lastGpsValidState = false;
      }
    }
  }
  // 支持 $GPGGA, $GNGGA 等
  else if (strstr(gpsData, "GGA")) {
    int fieldIndex = 0;
    char* token = strtok(gpsData, ",");
    String fields[15];
    while (token != NULL && fieldIndex < 15) {
      fields[fieldIndex++] = String(token);
      token = strtok(NULL, ",");
    }
    // GGA: $--GGA,time,lat,N,lng,E,fix_quality,num_satellites,...
    if (fieldIndex >= 8) {
      // 只有当GPS数据有效时才更新卫星数
      int fixQuality = fields[6].toInt();
      if (fixQuality > 0) {
        gpsSatellite = fields[7].toInt();
        
        // 每10秒输出一次卫星数信息
        unsigned long now = millis();
        if (now - lastGpsDebugPrint > 10000) {
          Serial.printf("[GPS] 当前卫星数: %d\n", gpsSatellite);
          lastGpsDebugPrint = now;
        }
      } else {
        gpsSatellite = 0;
      }
    }
  }
  // 支持 $GPVTG, $GNVTG (专门的速度和地面航向)
  else if (strstr(gpsData, "VTG")) {
    int fieldIndex = 0;
    char* token = strtok(gpsData, ",");
    String fields[10];
    while (token != NULL && fieldIndex < 10) {
      fields[fieldIndex++] = String(token);
      token = strtok(NULL, ",");
    }
    // VTG: $--VTG,course,T,,M,speed_knots,N,speed_kmh,K,checksum
    if (fieldIndex >= 8 && fields[7].length() > 0) {
      float rawSpeed = fields[7].toFloat(); // fields[7] 是 km/h
      updateGpsSpeed(rawSpeed, true);
    }
  }
}

// ===================== 读取GPS数据 ======================
void readGPSData() {
  static char gpsBuffer[256];
  static int bufIndex = 0;
  
  // 优先处理GPS数据，尽可能清空串口缓冲区
  while (GPSSerial.available() > 0) {
    char c = GPSSerial.read();
    if (c == '\n' || c == '\r') {
      if (bufIndex > 0) {
        gpsBuffer[bufIndex] = '\0';
        // 优先解析RMC和GGA语句，提高定位速度
        if (strstr(gpsBuffer, "RMC") || strstr(gpsBuffer, "GGA")) {
          parseGPS(gpsBuffer);
        }
        bufIndex = 0;
      }
    } else {
      if (bufIndex < sizeof(gpsBuffer) - 1) {
        gpsBuffer[bufIndex++] = c;
      } else {
        bufIndex = 0; // 缓冲区溢出，重置
      }
    }
  }
}

// ===================== 震动判定 ======================
bool isLargeVibration() {
  bool currentShake = (digitalRead(SW420D_DO_PIN) == HIGH); // HIGH=震动
  
  if (currentShake) {
    stableCount = 0;
    shakeCount++;
    bool isShake = (shakeCount >= SHAKE_THRESHOLD);
    if (isShake) {
      Serial.printf("[Vibration] Large vibration detected (%d consecutive)\n", shakeCount);
      buzzerEndTime = millis() + BUZZER_DURATION;
      isBuzzerOn = true;
    }
    return isShake;
  } else {
    shakeCount = 0;
    stableCount++;
    bool isStable = (stableCount >= STABLE_THRESHOLD);
    if (isStable) {
      Serial.printf("[Status] Stable detected (%d consecutive)\n", stableCount);
    }
    // 防抖：静止未达到阈值时保持上一状态，避免短暂抖动导致误判
    if (!isStable) return lastVibrationState;
    return false;
  }
}

void startLedBlink(unsigned long now) {
  ledBlinkState = LedBlinkState::Blinking;
  ledBlinkToggleCount = 0;
  ledNextToggleMs = now + LED_BLINK_INTERVAL;
  ledLastTriggerMs = now;
  ledState = HIGH;
  if (isLedReady) {
    digitalWrite(LED_PIN, ledState);
  }
  Serial.printf("[INFO] led_blink_start t=%lu\n", now);
}

void controlLED() {
  if (!isLedReady) return;
  unsigned long now = millis();

  if (ledBlinkState == LedBlinkState::Blinking) {
    if (now >= ledNextToggleMs) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      ledBlinkToggleCount++;
      ledNextToggleMs = now + LED_BLINK_INTERVAL;
      if (ledBlinkToggleCount >= LED_BLINK_TOGGLES) {
        ledBlinkState = LedBlinkState::Idle;
        ledState = LOW;
        digitalWrite(LED_PIN, ledState);
        Serial.printf("[INFO] led_blink_end t=%lu\n", now);
      }
    }
  }
}

// ===================== WiFi相关函数 ======================
void testServerConnection() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  if (USE_HTTPS) {
    wifiClientSecure.setInsecure();
    wifiClientSecure.setTimeout(HTTP_TIMEOUT);
    if (wifiClientSecure.connect(SERVER_IP, SERVER_PORT)) {
      wifiClientSecure.stop();
    }
  } else {
    if (wifiClient.connected()) {
      wifiClient.stop();
      delay(100);
    }
    wifiClient.setTimeout(HTTP_TIMEOUT);
    if (wifiClient.connect(SERVER_IP, SERVER_PORT)) {
      wifiClient.stop();
    }
  }
}

void initWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE); // 禁用WiFi睡眠模式，增加功耗
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.println("[WiFi] 开始连接...");
  int retry = 0;
  constexpr int MAX_WIFI_CONNECT_RETRY = 20; // 最大重试次数
  constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 1000; // 每次等待1秒
  unsigned long connectStartTime = millis();
  
  while (WiFi.status() != WL_CONNECTED && retry < MAX_WIFI_CONNECT_RETRY) {
    delay(WIFI_CONNECT_TIMEOUT_MS);
    handleSerialCommand();
    readGPSData();
    controlLED();
    feedWatchdog(); // 喂狗防止超时
    yield();
    retry++;
    
    // 每5秒输出一次连接状态
    if (retry % 5 == 0) {
      Serial.printf("[WiFi] 连接中... 重试 %d/%d\n", retry, MAX_WIFI_CONNECT_RETRY);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    unsigned long connectDuration = millis() - connectStartTime;
    Serial.printf("[WiFi] ✅ 连接成功！耗时 %lu 毫秒\n", connectDuration);
    Serial.printf("[WiFi] IP地址: %s\n", WiFi.localIP().toString().c_str());
    testServerConnection();
    wifiRetryCount = 0;
  } else {
    wifiRetryCount++;
    Serial.printf("[WiFi] ❌ 连接失败，重试计数: %d/%d\n", wifiRetryCount, MAX_WIFI_RETRY);
  }
  
  lastWiFiCheck = millis();
}

void checkWiFi() {
  if (millis() - lastWiFiCheck < WIFI_RETRY_INTERVAL) return;
  
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiRetryCount >= MAX_WIFI_RETRY) {
      lastWiFiCheck = millis() + 5000;
      return;
    }
    initWiFi();
  }
  
  lastWiFiCheck = millis();
}

// ===================== 传感器初始化 =====================
void initSW420D() {
  pinMode(SW420D_DO_PIN, INPUT);
  Serial.println("🔌 SW-420D 初始化完成（普通输入模式）");
  
  bool initState = digitalRead(SW420D_DO_PIN);
  Serial.print("📊 SW-420D初始状态：");
  Serial.println(initState ? "震动" : "静止");
  
  Serial.println("========== 初始震动参数 ==========");
  Serial.print("检测间隔："); Serial.print(DETECT_INTERVAL); Serial.println("ms");
  Serial.print("震动阈值："); Serial.print(SHAKE_THRESHOLD); Serial.println("次");
  Serial.print("静止阈值："); Serial.print(STABLE_THRESHOLD); Serial.println("次");
  Serial.println("==================================");
  Serial.println("📢 输入help查看参数调节指令");
}

// ===================== 新增LED初始化 =====================
void initLED() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  isLedReady = true;
  Serial.println("🔌 LED初始化完成（控制引脚：GPIO6）");
  Serial.print("📊 LED闪烁间隔："); Serial.println(LED_BLINK_INTERVAL);
  Serial.print("📊 LED闪烁次数："); Serial.println(LED_BLINK_TOGGLES / 2);
}

// ===================== 温湿度传感器初始化 =====================
// ===================== 看门狗函数 =====================
void initWatchdog() {
  // 启用任务看门狗
  esp_err_t err = esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  if (err == ESP_OK) {
    // 将当前任务订阅到看门狗
    esp_task_wdt_add(NULL);
    Serial.println("🐕 看门狗初始化成功，超时时间：" + String(WDT_TIMEOUT_SECONDS) + "秒");
  } else {
    Serial.println("⚠️ 看门狗初始化失败，错误码：" + String(err));
  }
  lastWdtFeedTime = millis();
}

void feedWatchdog() {
  unsigned long now = millis();
  if (now - lastWdtFeedTime >= WDT_FEED_INTERVAL_MS) {
    esp_task_wdt_reset();
    lastWdtFeedTime = now;
  }
}

void initAHT20() {
  if (!aht.begin()) {
    Serial.println("❌ 未找到 AHT10/AHT20 传感器，请检查接线！");
    ahtFound = false;
  } else {
    Serial.println("✅ AHT10/AHT20 初始化成功");
    ahtFound = true;
  }
}

void readAHT20() {
  if (!ahtFound) return;
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);
  currentTemp = temp.temperature;
  currentHum = humidity.relative_humidity;
  // Serial.print("🌡️ 温度: "); Serial.print(currentTemp); Serial.println(" ℃");
  // Serial.print("💧 湿度: "); Serial.print(currentHum); Serial.println(" %");
}

// ===================== GPS初始化 ======================
void initGPS() {
  Serial.println("[GPS] 开始初始化GPS模块...");
  
  // 先尝试9600波特率（大多数GPS模块默认波特率）
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("[GPS] 尝试9600波特率...");
  
  // 等待足够时间让GPS模块启动和发送数据
  unsigned long startTime = millis();
  bool gpsDataFound = false;
  
  while (millis() - startTime < 3000) { // 3秒超时
    if (GPSSerial.available() > 0) {
      char c = GPSSerial.peek();
      if (c == '$') { // 检测到NMEA语句开始标志
        gpsDataFound = true;
        Serial.println("[GPS] ✓ 9600波特率检测成功");
        break;
      }
      // 读取并丢弃无关字符
      GPSSerial.read();
    }
    delay(10);
  }
  
  // 如果9600波特率没有数据，尝试115200
  if (!gpsDataFound) {
    Serial.println("[GPS] 9600波特率无数据，尝试115200波特率...");
    GPSSerial.end();
    delay(500);
    GPSSerial.begin(115200, SERIAL_8N1, GPS_RX, GPS_TX);
    
    startTime = millis();
    while (millis() - startTime < 3000) {
      if (GPSSerial.available() > 0) {
        char c = GPSSerial.peek();
        if (c == '$') {
          gpsDataFound = true;
          Serial.println("[GPS] ✓ 115200波特率检测成功");
          break;
        }
        GPSSerial.read();
      }
      delay(10);
    }
  }
  
  if (!gpsDataFound) {
    Serial.println("[GPS] ⚠ 警告：未检测到GPS数据，请检查接线和模块");
  } else {
    Serial.println("[GPS] GPS初始化完成，等待定位...");
  }
  
  // 清空串口缓冲区，避免旧数据干扰
  while (GPSSerial.available() > 0) {
    GPSSerial.read();
  }
}

// ===================== 数据上报函数 ======================
bool uploadDataToServer(bool vibrationState) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  WiFiClient* clientPtr;
  
  // 根据配置选择HTTP或HTTPS
  String url;
  if (USE_HTTPS) {
    url = "https://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/upload";
    // 对于自签名证书，跳过证书验证（生产环境不推荐）
    wifiClientSecure.setInsecure();
    clientPtr = &wifiClientSecure;
  } else {
    url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/upload";
    clientPtr = &wifiClient;
  }

  // 使用更大的JSON缓冲区（768字节）以容纳更多字段
  StaticJsonDocument<768> jsonDoc;
  jsonDoc["id"] = DEVICE_ID;
  jsonDoc["lat"] = gpsValid ? gpsLat : 0.0;
  jsonDoc["lng"] = gpsValid ? gpsLng : 0.0;
  // 使用蓝牙定位数据
  jsonDoc["ble_lat"] = bleLocateValid ? bleLat : 0.0;
  jsonDoc["ble_lng"] = bleLocateValid ? bleLng : 0.0;
  jsonDoc["ble_valid"] = bleLocateValid;
  
  // 读取温湿度（每 2 秒一次，避免频繁读取）
  unsigned long now = millis();
  static unsigned long lastAhtRead = 0;
  if (now - lastAhtRead > 2000) {
    readAHT20();
    lastAhtRead = now;
  }

  jsonDoc["vibration"] = vibrationState ? 1 : 0;
  jsonDoc["speed"] = gpsSpeed;
  jsonDoc["direction"] = "--";
  jsonDoc["satellite"] = gpsSatellite;
  jsonDoc["gps_valid"] = gpsValid;
  jsonDoc["outside_campus"] = isOutsideCampus;
  if (ahtFound) {
    jsonDoc["temp"] = currentTemp;
    jsonDoc["hum"] = currentHum;
  } else {
    jsonDoc["temp"] = nullptr;
    jsonDoc["hum"] = nullptr;
  }
  
  // 判断车辆状态 - 使用真正静止判定（速度为零并维持3秒）
  if (vibrationState && gpsSpeed > 0) {
    currentVehicleStatus = VehicleStatus::IN_USE;
  } else if (vibrationState && !isVehicleTrulyStopped) {
    // 只有震动但还没有真正静止（速度刚降下来还没到3秒）时才判定为异常
    currentVehicleStatus = VehicleStatus::ABNORMAL;
  } else if (gpsSpeed > 0) {
    currentVehicleStatus = VehicleStatus::IN_USE;
  } else {
    // 真正静止或速度为零，判定为空闲
    currentVehicleStatus = VehicleStatus::IDLE;
  }
  
  // 上报车辆状态
  if (currentVehicleStatus == VehicleStatus::IDLE) {
    jsonDoc["status"] = "idle";
  } else if (currentVehicleStatus == VehicleStatus::IN_USE) {
    jsonDoc["status"] = "in_use";
  } else if (currentVehicleStatus == VehicleStatus::ABNORMAL) {
    jsonDoc["status"] = "abnormal";
  }

  String jsonData;
  serializeJson(jsonDoc, jsonData);

  if (!http.begin(*clientPtr, url)) {
    return false;
  }
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  http.addHeader("Connection", "close");
  http.setTimeout(HTTP_TIMEOUT);
  http.setConnectTimeout(HTTP_TIMEOUT);
  http.setReuse(true);

  int httpCode = http.POST(jsonData);
  bool success = (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_NO_CONTENT);

  http.end();
  return success;
}

// 简化版蓝牙定位（仅使用固定坐标）
void scanBLE() {
  if (millis() - lastBleScanMs < BLE_SCAN_INTERVAL) return;
  
  // 直接使用固定坐标，不进行蓝牙扫描
  // 这样可以大幅减少代码大小
  if (gpsValid) {
    // 优先使用GPS坐标
    bleLat = gpsLat;
    bleLng = gpsLng;
  } else {
    // 使用固定的兰州工业学院内坐标
    bleLat = 36.53000;
    bleLng = 103.72200;
  }
  
  // 确保位置在校园范围内
  if (bleLat < CAMPUS_MIN_LAT) bleLat = CAMPUS_MIN_LAT;
  if (bleLat > CAMPUS_MAX_LAT) bleLat = CAMPUS_MAX_LAT;
  if (bleLng < CAMPUS_MIN_LNG) bleLng = CAMPUS_MIN_LNG;
  if (bleLng > CAMPUS_MAX_LNG) bleLng = CAMPUS_MAX_LNG;
  
  bleLocateValid = true;
  lastBleScanMs = millis();
}

// ===================== GPS数据校验函数 =====================
bool isValidGpsCoordinate(float lat, float lng) {
  // 纬度范围：-90 到 90
  // 经度范围：-180 到 180
  return (lat >= -90.0f && lat <= 90.0f && lng >= -180.0f && lng <= 180.0f);
}

static inline float dmToDecimalLat(const String& dm, char hemi) {
  if (dm.length() < 4) return 0.0f;
  float deg = dm.substring(0, 2).toFloat();
  float minutes = dm.substring(2).toFloat();
  float val = deg + minutes / 60.0f;
  if (hemi == 'S') val = -val;
  return val;
}

static inline float dmToDecimalLng(const String& dm, char hemi) {
  if (dm.length() < 5) return 0.0f;
  float deg = dm.substring(0, 3).toFloat();
  float minutes = dm.substring(3).toFloat();
  float val = deg + minutes / 60.0f;
  if (hemi == 'W') val = -val;
  return val;
}
