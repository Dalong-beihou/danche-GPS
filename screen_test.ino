// ===================== 引脚定义 =====================
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_SCLK  12
#define TFT_MOSI  11
#define TFT_BL     7

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <U8g2_for_Adafruit_GFX.h>

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("===== 屏幕测试程序启动 =====");
  
  // 初始化背光
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  
  Serial.println("背光已打开");
  
  // 尝试多种初始化模式
  Serial.println("尝试 INITR_BLACKTAB 模式...");
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  
  Serial.println("填充红色...");
  tft.fillScreen(ST7735_RED);
  delay(1000);
  
  Serial.println("填充绿色...");
  tft.fillScreen(ST7735_GREEN);
  delay(1000);
  
  Serial.println("填充蓝色...");
  tft.fillScreen(ST7735_BLUE);
  delay(1000);
  
  Serial.println("填充黑色...");
  tft.fillScreen(ST7735_BLACK);
  
  Serial.println("显示测试文字...");
  tft.setCursor(10, 10);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(2);
  tft.println("Screen Test!");
  
  tft.setCursor(10, 40);
  tft.setTextSize(1);
  tft.println("If you see this,");
  tft.println("screen is working!");
  
  Serial.println("===== 测试完成 =====");
  Serial.println("现在尝试其他模式...");
  Serial.println("发送 'mode=1' 到串口切换模式");
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.startsWith("mode=")) {
      int mode = cmd.substring(5).toInt();
      Serial.print("切换到模式: ");
      Serial.println(mode);
      
      switch(mode) {
        case 1:
          tft.initR(INITR_BLACKTAB);
          Serial.println("INITR_BLACKTAB");
          break;
        case 2:
          tft.initR(INITR_GREENTAB);
          Serial.println("INITR_GREENTAB");
          break;
        case 3:
          tft.initR(INITR_REDTAB);
          Serial.println("INITR_REDTAB");
          break;
        case 4:
          tft.initR(INITR_MINI160x80);
          Serial.println("INITR_MINI160x80");
          break;
      }
      tft.setRotation(1);
      tft.fillScreen(ST7735_BLACK);
      tft.setCursor(10, 10);
      tft.setTextColor(ST7735_WHITE);
      tft.setTextSize(2);
      tft.print("Mode ");
      tft.println(mode);
    }
  }
}
