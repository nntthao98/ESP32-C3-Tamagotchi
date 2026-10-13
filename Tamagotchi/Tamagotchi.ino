#include <Arduino.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <LovyanGFX.hpp>
#include "assets.h"
#include "ui_icons.h"
#include "background.h"

// =====================================================
// DISPLAY
// =====================================================

class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX()
  {
    auto bus_cfg = _bus_instance.config();

    bus_cfg.spi_host = SPI2_HOST;
    bus_cfg.spi_mode = 0;
    bus_cfg.freq_write = 40000000;
    bus_cfg.freq_read = 16000000;

    bus_cfg.pin_sclk = 6;
    bus_cfg.pin_mosi = 7;
    bus_cfg.pin_miso = -1;
    bus_cfg.pin_dc   = 2;

    _bus_instance.config(bus_cfg);
    _panel_instance.setBus(&_bus_instance);

    auto panel_cfg = _panel_instance.config();

    panel_cfg.pin_cs = 10;
    panel_cfg.pin_rst = -1;
    panel_cfg.pin_busy = -1;

    panel_cfg.memory_width = 240;
    panel_cfg.memory_height = 240;
    panel_cfg.panel_width = 240;
    panel_cfg.panel_height = 240;

    panel_cfg.offset_x = 0;
    panel_cfg.offset_y = 0;

    panel_cfg.readable = false;
    panel_cfg.invert = true;
    panel_cfg.rgb_order = false;
    panel_cfg.dlen_16bit = false;
    panel_cfg.bus_shared = false;

    _panel_instance.config(panel_cfg);
    setPanel(&_panel_instance);
  }
};

LGFX lcd;

// Buffer riêng cho pet: giúp đổi frame bằng một lần push, tránh nháy.
LGFX_Sprite petBuffer(&lcd);
LGFX_Sprite petBottomBuffer(&lcd);

#define TFT_BL 3

// =====================================================
// AUTO DISPLAY SLEEP
// =====================================================

bool displaySleeping = false;
bool justWokeFromSleep = false;
unsigned long lastInteraction = 0;
const unsigned long DISPLAY_SLEEP_TIMEOUT = 60000UL;

// Pet ngủ tối đa 2 tiếng rồi tự thức.
const unsigned long PET_SLEEP_DURATION = 2UL * 60UL * 60UL * 1000UL;
unsigned long sleepStartedAt = 0;

// DATE / TIME - NTP (VIETNAM)
// Có thể lưu nhiều mạng Wi-Fi. Điền SSID/password ở WIFI_SSID_2, _3... nếu cần.
const char* WIFI_SSID_1 = "TEN_WIFI_1";
const char* WIFI_PASSWORD_1 = "PASS_WIFI_1";
const char* WIFI_SSID_2 = "TEN_WIFI_2";       // Ví dụ: "Ten_WiFi_nha"
const char* WIFI_PASSWORD_2 = "PASS_WIFI_2";   // Mật khẩu Wi-Fi nhà
const char* WIFI_SSID_3 = "TEN_WIFI_3";       // Có thể để trống nếu không dùng
const char* WIFI_PASSWORD_3 = "PASS_WIFI_3";

struct WifiNetwork
{
  const char* ssid;
  const char* password;
};

const WifiNetwork WIFI_NETWORKS[] = {
  { WIFI_SSID_1, WIFI_PASSWORD_1 },
  { WIFI_SSID_2, WIFI_PASSWORD_2 },
  { WIFI_SSID_3, WIFI_PASSWORD_3 }
};
const size_t WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

const unsigned long TIME_SYNC_INTERVAL = 60UL * 60UL * 1000UL; // Đồng bộ lại mỗi 1 giờ
const unsigned long WIFI_CONNECT_TIMEOUT = 10000UL;
const unsigned long NTP_SYNC_TIMEOUT = 20000UL;
const unsigned long WIFI_RETRY_INTERVAL = 5UL * 60UL * 1000UL; // Thử lại sau 5 phút nếu Wi-Fi/NTP thất bại

bool timeSynced = false;
unsigned long lastClockDraw = 0;
int lastClockMinute = -1;

enum TimeSyncState { TIME_SYNC_IDLE, TIME_SYNC_CONNECTING, TIME_SYNC_WAIT_NTP };
TimeSyncState timeSyncState = TIME_SYNC_IDLE;
unsigned long timeSyncStateStarted = 0;
unsigned long nextTimeSyncAt = 0;
size_t wifiNetworkIndex = 0;

// Background hiện tại: 0 = Windows 7, 1 = Bathroom, 2 = Living room.
int currentBackground = 0;

void wakeDisplay();
void enterDisplaySleep();
void drawUI();
void drawSleepClock(bool force = false);
void startTimeSyncCycle();
void serviceTimeSync();
void tryNextWifiNetwork();
void finishTimeSync(bool success);

// =====================================================
// TOUCH - CST816D/S
// =====================================================

#define TOUCH_SDA 4
#define TOUCH_SCL 5
#define TOUCH_RST 1
#define TOUCH_ADDR 0x15

// Phân biệt lỗi giao tiếp I2C với trạng thái bình thường không chạm.
bool touchWasDown = false; // Khai báo sớm để hàm phục hồi có thể xóa trạng thái chạm cũ.
bool touchI2CError = false;
uint8_t touchI2CErrorCount = 0;
const uint8_t TOUCH_ERRORS_BEFORE_RECOVERY = 3;
unsigned long lastTouchRecovery = 0;
const unsigned long TOUCH_RECOVERY_COOLDOWN = 1000UL;

bool readTouch(int &x, int &y)
{
  touchI2CError = false;

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0)
  {
    touchI2CError = true;
    return false;
  }

  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)1) != 1)
  {
    touchI2CError = true;
    while (Wire.available()) Wire.read();
    return false;
  }

  uint8_t points = Wire.read();
  if (points == 0)
    return false; // Không chạm là trạng thái bình thường, không phải lỗi I2C.

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x03);
  if (Wire.endTransmission(false) != 0)
  {
    touchI2CError = true;
    return false;
  }

  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)4) != 4)
  {
    touchI2CError = true;
    while (Wire.available()) Wire.read();
    return false;
  }

  uint8_t xHigh = Wire.read();
  uint8_t xLow  = Wire.read();
  uint8_t yHigh = Wire.read();
  uint8_t yLow  = Wire.read();

  int readX = ((xHigh & 0x0F) << 8) | xLow;
  int readY = ((yHigh & 0x0F) << 8) | yLow;

  // Không ép tọa độ lỗi về 239 vì có thể biến dữ liệu rác thành cú chạm ở mép màn hình.
  if (readX > 239 || readY > 239)
    return false;

  x = readX;
  y = readY;
  return true;
}

void recoverTouchController()
{
  unsigned long now = millis();
  if (now - lastTouchRecovery < TOUCH_RECOVERY_COOLDOWN)
    return;

  lastTouchRecovery = now;
  Serial.println("[TOUCH] I2C error repeated; restarting CST816 and I2C...");

  // Thả trạng thái chạm cũ để tránh nút bị kẹt sau khi bộ điều khiển khởi động lại.
  touchWasDown = false;

  Wire.end();
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(80);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(100000); // 100 kHz ổn định hơn 400 kHz với dây/mạch nhạy nhiễu.
  Wire.setTimeOut(50);
  touchI2CErrorCount = 0;
  Serial.println("[TOUCH] Controller restart complete.");
}

// =====================================================
// PET STATE
// =====================================================

enum PetMood
{
  MOOD_NORMAL,
  MOOD_HAPPY,
  MOOD_HUNGRY,
  MOOD_BORED,
  MOOD_SLEEP
};

struct PetState
{
  int hunger = 80;
  int happiness = 80;
  bool sleeping = false;
  int mood = MOOD_HAPPY;
};

PetState pet;

unsigned long lastHunger = 0;
unsigned long lastHappiness = 0;

void clampStats()
{
  pet.hunger = constrain(pet.hunger, 0, 100);
  pet.happiness = constrain(pet.happiness, 0, 100);
}

int calculateMood()
{
  if (pet.sleeping)
    return MOOD_SLEEP;

  if (pet.hunger < 30)
    return MOOD_HUNGRY;

  if (pet.happiness < 30)
    return MOOD_BORED;

  if (pet.happiness >= 80)
    return MOOD_HAPPY;

  return MOOD_NORMAL;
}

void updateMood()
{
  pet.mood = calculateMood();
}

// =====================================================
// PET ACTIONS
// =====================================================

void feedPet()
{
  if (pet.sleeping)
    return;

  pet.hunger += 20;
  pet.happiness += 3;

  clampStats();
  updateMood();

  Serial.println("FEED");
}

void playPet()
{
  if (pet.sleeping)
    return;

  pet.happiness += 20;
  pet.hunger -= 5;

  clampStats();
  updateMood();

  Serial.println("PLAY");
}

void sleepPet()
{
  if (pet.sleeping)
  {
    pet.sleeping = false;
    unsigned long now = millis();
    lastHunger = now;
    lastHappiness = now;
    updateMood();

    Serial.println("WAKE MANUAL");
    return;
  }

  pet.sleeping = true;
  sleepStartedAt = millis();
  updateMood();

  Serial.println("SLEEP");
}

// =====================================================
// PET UPDATE
// =====================================================

void updatePet()
{
  unsigned long now = millis();

  if (pet.sleeping)
  {
    // Sau đúng 2 tiếng kể từ lúc bắt đầu ngủ -> tự thức.
    if (now - sleepStartedAt >= PET_SLEEP_DURATION)
    {
      pet.sleeping = false;
      lastHunger = now;
      lastHappiness = now;
      updateMood();
      Serial.println("WAKE AFTER 2 HOURS");
      return;
    }

    // Khi đang ngủ, giữ nguyên hành vi: không giảm Hunger/Happiness.
    clampStats();
    updateMood();
    return;
  }

  // Pet đang thức: tính tất cả các chu kỳ đã trôi qua.
  while (now - lastHunger >= 60000UL)
  {
    lastHunger += 60000UL;
    pet.hunger--;
  }

  while (now - lastHappiness >= 90000UL)
  {
    lastHappiness += 90000UL;
    pet.happiness--;
  }

  clampStats();
  updateMood();
}

// =====================================================
// ANIMATION
// =====================================================

enum PetFrame
{
  FRAME_IDLE_01,
  FRAME_IDLE_MOVE,

  FRAME_HAPPY,
  FRAME_HAPPY_MOVE,

  FRAME_BORED,
  FRAME_BORED_MOVE,

  FRAME_HUNGRY,
  FRAME_HUNGRY_MOVE,

  FRAME_SLEEP,
  FRAME_SLEEP_MOVE
};

PetFrame currentFrame = FRAME_IDLE_01;
int previousMood = MOOD_HAPPY;

unsigned long lastAnimation = 0;
const unsigned long ANIMATION_INTERVAL = 500;

// Chạm vào pet: tạm chuyển sang HAPPY trong 1 giây, không đổi chỉ số.
bool petTapHappyActive = false;
unsigned long petTapHappyStarted = 0;
const unsigned long PET_TAP_HAPPY_DURATION = 1000UL;

const PetSprite& getCurrentSprite()
{
  switch (currentFrame)
  {
    case FRAME_IDLE_01:
      return PET_SPRITES[SPRITE_PET_IDLE_01];

    case FRAME_IDLE_MOVE:
      return PET_SPRITES[SPRITE_PET_IDLE_MOVE];

    case FRAME_HAPPY:
      return PET_SPRITES[SPRITE_PET_IDLE_HAPPY];

    case FRAME_HAPPY_MOVE:
      return PET_SPRITES[SPRITE_PET_IDLE_HAPPY_MOVE];

    case FRAME_BORED:
      return PET_SPRITES[SPRITE_PET_IDLE_BORED];

    case FRAME_BORED_MOVE:
      return PET_SPRITES[SPRITE_PET_IDLE_BORED_MOVE];

    case FRAME_HUNGRY:
      return PET_SPRITES[SPRITE_PET_IDLE_HUNGRY];

    case FRAME_HUNGRY_MOVE:
      return PET_SPRITES[SPRITE_PET_IDLE_HUNGRY_MOVE];

    case FRAME_SLEEP:
      return PET_SPRITES[SPRITE_PET_IDLE_SLEEP];

    case FRAME_SLEEP_MOVE:
      return PET_SPRITES[SPRITE_PET_IDLE_SLEEP_MOVE];
  }

  return PET_SPRITES[SPRITE_PET_IDLE_01];
}

void resetAnimationForMood()
{
  switch (pet.mood)
  {
    case MOOD_NORMAL:
      currentFrame = FRAME_IDLE_01;
      break;

    case MOOD_HAPPY:
      currentFrame = FRAME_HAPPY;
      break;

    case MOOD_BORED:
      currentFrame = FRAME_BORED;
      break;

    case MOOD_HUNGRY:
      currentFrame = FRAME_HUNGRY;
      break;

    case MOOD_SLEEP:
      currentFrame = FRAME_SLEEP;
      break;
  }

  lastAnimation = millis();
}

void updateAnimation()
{
  // Ưu tiên animation HAPPY tạm thời khi người dùng chạm vào pet.
  // Hết 2 giây sẽ trở về mood thực tế hiện tại.
  if (petTapHappyActive)
  {
    unsigned long now = millis();

    if (now - petTapHappyStarted >= PET_TAP_HAPPY_DURATION)
    {
      petTapHappyActive = false;
      previousMood = pet.mood;
      resetAnimationForMood();
      return;
    }

    if (now - lastAnimation >= ANIMATION_INTERVAL)
    {
      lastAnimation = now;
      currentFrame = (currentFrame == FRAME_HAPPY)
        ? FRAME_HAPPY_MOVE
        : FRAME_HAPPY;
    }
    return;
  }

  if (pet.mood != previousMood)
  {
    previousMood = pet.mood;
    resetAnimationForMood();
    return;
  }

  if (pet.mood == MOOD_SLEEP)
  {
    const unsigned long SLEEP_ANIMATION_INTERVAL = 1000UL;

    unsigned long now = millis();

    if (now - lastAnimation < SLEEP_ANIMATION_INTERVAL)
      return;

    lastAnimation = now;

    currentFrame =
      (currentFrame == FRAME_SLEEP)
      ? FRAME_SLEEP_MOVE
      : FRAME_SLEEP;

    return;
  }

  unsigned long now = millis();

  if (now - lastAnimation < ANIMATION_INTERVAL)
    return;

  lastAnimation = now;

  switch (pet.mood)
  {
    case MOOD_NORMAL:
      currentFrame =
        (currentFrame == FRAME_IDLE_01)
        ? FRAME_IDLE_MOVE
        : FRAME_IDLE_01;
      break;

    case MOOD_HAPPY:
      currentFrame =
        (currentFrame == FRAME_HAPPY)
        ? FRAME_HAPPY_MOVE
        : FRAME_HAPPY;
      break;

    case MOOD_BORED:
      currentFrame =
        (currentFrame == FRAME_BORED)
        ? FRAME_BORED_MOVE
        : FRAME_BORED;
      break;

    case MOOD_HUNGRY:
      currentFrame =
        (currentFrame == FRAME_HUNGRY)
        ? FRAME_HUNGRY_MOVE
        : FRAME_HUNGRY;
      break;

    case MOOD_SLEEP:
      currentFrame = FRAME_SLEEP;
      break;
  }
}

// =====================================================
// SLEEP DARKENING
// =====================================================

uint16_t darken565(uint16_t color)
{
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  // Giảm độ sáng còn khoảng 50%, vẫn giữ nguyên màu.
  r >>= 1;
  g >>= 1;
  b >>= 1;

  return (r << 11) | (g << 5) | b;
}

void drawBackground(bool dark)
{
  const uint16_t *bg = nullptr;

  switch (currentBackground)
  {
    case 0: bg = BG_ROOM_WIN7; break;
    case 1: bg = BG_ROOM_BATHROOM; break;
    default: bg = BG_ROOM_LIVINGROOM; break;
  }

  if (!dark)
  {
    lcd.pushImage(0, 0, BG_W, BG_H, bg);
    return;
  }

  // Khi pet ngủ: không dùng drawPixel() từng pixel nữa.
  // Cách cũ khiến màn hình bị vẽ từ trên xuống dưới rất rõ.
  // Dùng buffer 16 dòng và push theo từng khối lớn -> chuyển cảnh mượt hơn nhiều.
  const int BLOCK_H = 16;
  static uint16_t darkBuffer[BG_W * BLOCK_H];

  for (int y0 = 0; y0 < BG_H; y0 += BLOCK_H)
  {
    int h = min(BLOCK_H, BG_H - y0);

    for (int y = 0; y < h; y++)
    {
      for (int x = 0; x < BG_W; x++)
      {
        uint16_t color = pgm_read_word(&bg[(y0 + y) * BG_W + x]);
        darkBuffer[y * BG_W + x] = darken565(color);
      }
    }

    lcd.pushImage(0, y0, BG_W, h, darkBuffer);
  }
}

// =====================================================
// DRAW PET SPRITE
// =====================================================

void drawPet()
{
  const PetSprite &sprite = getCurrentSprite();

  // Sprite gốc 48x48 -> scale 3x = 144x144. Giữ drawPixel() để LovyanGFX xử lý màu đúng như bản gốc.
  // Chỉ cập nhật vùng của pet; không vẽ lại UI sau mỗi frame.
  const int scale = 3;
  const int x = (240 - 48 * scale) / 2;
  const int y = 48;

  const uint16_t *bg = nullptr;
  switch (currentBackground)
  {
    case 0: bg = BG_ROOM_WIN7; break;
    case 1: bg = BG_ROOM_BATHROOM; break;
    default: bg = BG_ROOM_LIVINGROOM; break;
  }

  // Phần trên: y=48..175, không chạm vào các icon phía dưới.
  for (int py = 0; py < 128; py++)
  {
    for (int px = 0; px < 144; px++)
    {
      int srcIndex = (py + 48) * BG_W + (px + 48);
      uint16_t bgColor = pgm_read_word(&bg[srcIndex]);

      if (pet.sleeping)
        bgColor = darken565(bgColor);

      petBuffer.drawPixel(px, py, bgColor);
    }
  }

  // Phần dưới của pet chỉ nằm ở vùng giữa x=72..167,
  // nên không đè lên FEED/SLEEP ở hai bên.
  for (int py = 0; py < 16; py++)
  {
    for (int px = 0; px < 96; px++)
    {
      int screenX = 72 + px;
      int screenY = 176 + py;

      uint16_t bgColor = pgm_read_word(
        &bg[screenY * BG_W + screenX]
      );

      if (pet.sleeping)
        bgColor = darken565(bgColor);

      petBottomBuffer.drawPixel(px, py, bgColor);
    }
  }

  // Vẽ frame pet vào 2 buffer, giữ nguyên màu và cách scale hiện tại.
  for (int sy = 0; sy < 48; sy++)
  {
    for (int sx = 0; sx < 48; sx++)
    {
      int index = sy * 48 + sx;

      uint8_t alpha = pgm_read_byte(&sprite.alpha[index]);

      if (alpha < 128)
        continue;

      uint16_t color = pgm_read_word(&sprite.pixels[index]);

      if (pet.sleeping)
        color = darken565(color);

      int dx = sx * scale;
      int dy = sy * scale;

      // Phần trên.
      for (int yy = 0; yy < scale; yy++)
      {
        for (int xx = 0; xx < scale; xx++)
        {
          int py = dy + yy;

          if (py < 128)
            petBuffer.drawPixel(dx + xx, py, color);
        }
      }

      // Phần dưới: chỉ vùng buffer x=24..119 tương ứng
      // với màn hình x=72..167.
      for (int yy = 0; yy < scale; yy++)
      {
        for (int xx = 0; xx < scale; xx++)
        {
          int bx = dx + xx;
          int by = dy + yy;

          if (bx >= 24 && bx < 120 && by >= 128 && by < 144)
            petBottomBuffer.drawPixel(bx - 24, by - 128, color);
        }
      }
    }
  }

  // Chỉ đẩy vùng pet lên LCD.
  // Không gọi drawUI() và không gọi drawSleepClock() ở đây.
  petBuffer.pushSprite(x, y);
  petBottomBuffer.pushSprite(72, 176);
}

// =====================================================
// UI ICONS
// =====================================================

// ui_icons.h chứa 3 icon PNG 20x20:
// FEED, PLAY, SLEEP.
// Icon được lưu RGB565 + alpha.

void drawUIIcon(const UIIcon &icon, int x, int y)
{
  // Icon gốc 20x20 -> phóng 1.5x = 30x30
  const int scale = 3;
  const int sourceStep = 2;

  for (int sy = 0; sy < 20; sy++)
  {
    for (int sx = 0; sx < 20; sx++)
    {
      int index = sy * 20 + sx;

      uint8_t alpha = pgm_read_byte(&icon.alpha[index]);

      if (alpha < 128)
        continue;

      uint16_t color = pgm_read_word(&icon.pixels[index]);

      // Mỗi pixel nguồn thành ô 1.5x1.5 bằng cách vẽ xen kẽ
      int dx = x + (sx * 3) / 2;
      int dy = y + (sy * 3) / 2;

      int nextX = x + ((sx + 1) * 3) / 2;
      int nextY = y + ((sy + 1) * 3) / 2;

      lcd.fillRect(dx, dy, nextX - dx, nextY - dy, color);
    }
  }
}

void drawHeartIcon(int cx, int cy)
{
  lcd.fillCircle(cx - 4, cy - 2, 5, TFT_RED);
  lcd.fillCircle(cx + 4, cy - 2, 5, TFT_RED);

  lcd.fillTriangle(
    cx - 9, cy,
    cx + 9, cy,
    cx, cy + 9,
    TFT_RED
  );
}

// =====================================================
// SMALL HUNGER ICON (10x10)
// =====================================================

void drawHungerIcon(const UISmallIcon &icon, int x, int y, float scale)
{
  // Icon gốc 10x10 -> 1.5x = 15x15
  for (int sy = 0; sy < 10; sy++)
  {
    for (int sx = 0; sx < 10; sx++)
    {
      int index = sy * 10 + sx;

      uint8_t alpha = pgm_read_byte(&icon.alpha[index]);
      if (alpha < 128)
        continue;

      uint16_t color = pgm_read_word(&icon.pixels[index]);

      int dx = x + (int)(sx * scale);
      int dy = y + (int)(sy * scale);
      int nextX = x + (int)((sx + 1) * scale);
      int nextY = y + (int)((sy + 1) * scale);

      lcd.fillRect(dx, dy, nextX - dx, nextY - dy, color);
    }
  }
}

// =====================================================
// DRAW UI
// =====================================================

// Vẽ lại một vùng nhỏ từ background gốc trước khi in chỉ số mới.
// Dùng đúng pixel màu gốc của background hiện tại; nếu pet đang ngủ thì
// áp dụng đúng phép làm tối vốn có. Không đổi palette, RGB565 hay byte order.
void redrawBackgroundPatch(int x, int y, int w, int h)
{
  const uint16_t *bg = nullptr;

  switch (currentBackground)
  {
    case 0: bg = BG_ROOM_WIN7; break;
    case 1: bg = BG_ROOM_BATHROOM; break;
    default: bg = BG_ROOM_LIVINGROOM; break;
  }

  static uint16_t patch[65 * 24];
  if (w > 65 || h > 24 || w <= 0 || h <= 0) return;

  for (int py = 0; py < h; py++)
  {
    for (int px = 0; px < w; px++)
    {
      uint16_t color = pgm_read_word(&bg[(y + py) * BG_W + (x + px)]);
      if (pet.sleeping) color = darken565(color);
      patch[py * w + px] = color;
    }
  }

  lcd.pushImage(x, y, w, h, patch);
}

void drawUI()
{
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);

  // Xóa chữ số cũ bằng cách khôi phục chính xác background bên dưới.
  // Nhờ vậy 80 -> 100 không bị chồng chữ, đồng thời giữ nguyên màu nền.
  redrawBackgroundPatch(68, 22, 48, 24);
  redrawBackgroundPatch(155, 22, 65, 24);

  // Top stats — giữ nguyên vị trí và màu icon/chữ của bản gốc.
  drawHungerIcon(UI_HUNGER_ICON, 50, 24, 1.5f);
  lcd.setCursor(70, 24);
  lcd.print(pet.hunger);

  drawHeartIcon(142, 31);
  lcd.setCursor(157, 24);
  lcd.print(pet.happiness);

  // Bottom action icons — giữ nguyên.
  drawUIIcon(UI_ICONS[UI_ICON_FEED], 42, 176);
  drawUIIcon(UI_ICONS[UI_ICON_PLAY], 105, 200);
  drawUIIcon(UI_ICONS[UI_ICON_SLEEP], 168, 176);
}

// =====================================================
// DATE / TIME - NTP (non-blocking; retries multiple Wi-Fi networks)
// =====================================================

void tryNextWifiNetwork()
{
  while (wifiNetworkIndex < WIFI_NETWORK_COUNT &&
         (WIFI_NETWORKS[wifiNetworkIndex].ssid == nullptr ||
          WIFI_NETWORKS[wifiNetworkIndex].ssid[0] == '\0'))
  {
    wifiNetworkIndex++;
  }

  if (wifiNetworkIndex >= WIFI_NETWORK_COUNT)
  {
    Serial.println("[TIME] All configured Wi-Fi networks failed");
    finishTimeSync(false);
    return;
  }

  const WifiNetwork &network = WIFI_NETWORKS[wifiNetworkIndex];
  Serial.print("[TIME] Trying Wi-Fi: ");
  Serial.println(network.ssid);
  WiFi.disconnect(false, false);
  WiFi.begin(network.ssid, network.password);
  timeSyncState = TIME_SYNC_CONNECTING;
  timeSyncStateStarted = millis();
}

void startTimeSyncCycle()
{
  if (timeSyncState != TIME_SYNC_IDLE)
    return;

  Serial.println("[TIME] Starting scheduled Wi-Fi/NTP sync...");
  wifiNetworkIndex = 0;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  tryNextWifiNetwork();
}

void finishTimeSync(bool success)
{
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  timeSyncState = TIME_SYNC_IDLE;

  if (success)
  {
    timeSynced = true;
    nextTimeSyncAt = millis() + TIME_SYNC_INTERVAL;
    Serial.println("[TIME] Sync successful; Wi-Fi OFF until next 1-hour sync");
  }
  else
  {
    // Nếu tất cả mạng đều tạm thời không dùng được, thử lại sau 5 phút.
    nextTimeSyncAt = millis() + WIFI_RETRY_INTERVAL;
    Serial.println("[TIME] Sync failed; Wi-Fi OFF, retry in 5 minutes");
  }
}

void serviceTimeSync()
{
  unsigned long now = millis();

  if (timeSyncState == TIME_SYNC_IDLE)
  {
    if (nextTimeSyncAt == 0 || (long)(now - nextTimeSyncAt) >= 0)
      startTimeSyncCycle();
    return;
  }

  if (timeSyncState == TIME_SYNC_CONNECTING)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("[TIME] Wi-Fi connected: ");
      Serial.println(WiFi.localIP());
      configTzTime("ICT-7", "pool.ntp.org", "time.nist.gov", "time.google.com");
      timeSyncState = TIME_SYNC_WAIT_NTP;
      timeSyncStateStarted = now;
      return;
    }

    if (now - timeSyncStateStarted >= WIFI_CONNECT_TIMEOUT)
    {
      Serial.println("[TIME] Wi-Fi timeout; trying next network");
      wifiNetworkIndex++;
      tryNextWifiNetwork();
    }
    return;
  }

  if (timeSyncState == TIME_SYNC_WAIT_NTP)
  {
    struct tm timeinfo;
    // configTzTime starts SNTP asynchronously; keep the UI/touch loop running.
    if (now - timeSyncStateStarted >= 5000UL &&
        getLocalTime(&timeinfo, 0) && timeinfo.tm_year >= (2025 - 1900))
    {
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
      Serial.print("[TIME] Current time: ");
      Serial.println(buffer);
      finishTimeSync(true);
      return;
    }

    if (now - timeSyncStateStarted >= NTP_SYNC_TIMEOUT)
    {
      Serial.println("[TIME] NTP timeout; trying next Wi-Fi network");
      wifiNetworkIndex++;
      WiFi.disconnect(false, false);
      tryNextWifiNetwork();
    }
  }
}

void drawSleepClock(bool force)
{
  if (!pet.sleeping || !timeSynced)
    return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0))
    return;

  if (!force && timeinfo.tm_min == lastClockMinute)
    return;

  lastClockMinute = timeinfo.tm_min;

  char timeText[8];
  char dateText[16];
  strftime(timeText, sizeof(timeText), "%H:%M", &timeinfo);
  strftime(dateText, sizeof(dateText), "%d/%m/%Y", &timeinfo);

  // Khung 132x38, căn giữa, nền đen khoảng 50% và bo góc.
  const int boxX = 54;
  const int boxY = 3;
  const int boxW = 132;
  const int boxH = 50;
  const int radius = 8;
  static uint16_t clockBuffer[132 * 50];

  const uint16_t *bg = nullptr;
  switch (currentBackground)
  {
    case 0: bg = BG_ROOM_WIN7; break;
    case 1: bg = BG_ROOM_BATHROOM; break;
    default: bg = BG_ROOM_LIVINGROOM; break;
  }

  for (int y = 0; y < boxH; y++)
  {
    for (int x = 0; x < boxW; x++)
    {
      uint16_t color = pgm_read_word(&bg[(boxY + y) * BG_W + (boxX + x)]);
      color = darken565(color);

      int dx = min(x, boxW - 1 - x);
      int dy = min(y, boxH - 1 - y);
      bool inside = (dx >= radius || dy >= radius ||
                     (dx - radius) * (dx - radius) +
                     (dy - radius) * (dy - radius) <= radius * radius);

      // Overlay đen thêm 50% trên nền đã tối của chế độ ngủ.
      if (inside)
        color = darken565(color);

      clockBuffer[y * boxW + x] = color;
    }
  }

  lcd.pushImage(boxX, boxY, boxW, boxH, clockBuffer);

  lcd.setTextColor(TFT_WHITE);
  // Tăng cỡ chữ khoảng 1.5x so với bản trước: 2 -> 3 cho giờ, 1 -> 2 cho ngày.
  lcd.setTextSize(3);
  int timeW = lcd.textWidth(timeText);
  lcd.setCursor((240 - timeW) / 2, 5);
  lcd.print(timeText);

  lcd.setTextSize(2);
  int dateW = lcd.textWidth(dateText);
  lcd.setCursor((240 - dateW) / 2, 32);
  lcd.print(dateText);
}

// =====================================================
// FULL SCREEN
// =====================================================

void drawScreen()
{
  drawBackground(pet.sleeping);
  drawPet();
  drawUI();

  if (pet.sleeping)
  {
    lastClockMinute = -1;
    drawSleepClock(true);
  }
}

// =====================================================
// AUTO DISPLAY SLEEP
// =====================================================

void wakeDisplay()
{
  if (displaySleeping)
  {
    digitalWrite(TFT_BL, HIGH);
    displaySleeping = false;
    drawScreen();
  }

  lastInteraction = millis();
}

void enterDisplaySleep()
{
  if (displaySleeping)
    return;

  // Đảm bảo GPIO0 đang ở trạng thái HIGH trước khi cấu hình wake.
  // Nếu INT của CST816 vẫn đang LOW, ESP32 sẽ vừa ngủ đã thức ngay lập tức.
  pinMode(0, INPUT_PULLUP);
  unsigned long waitStart = millis();
  while (digitalRead(0) == LOW && millis() - waitStart < 200)
    delay(1);

  displaySleeping = true;

  // Tắt backlight: đây là phần dễ nhận thấy nhất của Light Sleep.
  digitalWrite(TFT_BL, LOW);

  // CST816 INT = GPIO0, active LOW khi có touch.
  gpio_pullup_en(GPIO_NUM_0);
  gpio_pulldown_dis(GPIO_NUM_0);
  gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  // Timer đánh thức khi đến lịch đồng bộ NTP hoặc khi pet ngủ đủ 2 tiếng.
  // Dùng thời điểm gần nhất để Wi-Fi có thể đồng bộ ngay cả khi màn hình đang ngủ.
  uint64_t timerWakeMs = 0;
  unsigned long nowMs = millis();

  if (nextTimeSyncAt != 0)
  {
    long untilSync = (long)(nextTimeSyncAt - nowMs);
    if (untilSync > 0)
      timerWakeMs = (uint64_t)untilSync;
    else
      timerWakeMs = 1;
  }

  if (pet.sleeping)
  {
    unsigned long elapsed = nowMs - sleepStartedAt;
    if (elapsed < PET_SLEEP_DURATION)
    {
      uint64_t petWakeMs = (uint64_t)(PET_SLEEP_DURATION - elapsed);
      if (timerWakeMs == 0 || petWakeMs < timerWakeMs)
        timerWakeMs = petWakeMs;
    }
  }

  if (timerWakeMs > 0)
    esp_sleep_enable_timer_wakeup(timerWakeMs * 1000ULL);

  Serial.println("[POWER] Entering Light Sleep...");
  Serial.flush();

  esp_light_sleep_start();

  // Khi GPIO0 đánh thức, bật màn hình. Nếu timer đánh thức để đồng bộ giờ,
  // giữ màn hình tắt và xử lý Wi-Fi trong loop, tránh bật sáng không cần thiết.
  delay(20);
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  displaySleeping = false;

  if (wakeCause == ESP_SLEEP_WAKEUP_GPIO)
  {
    digitalWrite(TFT_BL, HIGH);
    justWokeFromSleep = true;
    lastInteraction = millis();
    Serial.println("[POWER] Woke from touch");
    updatePet();
    drawScreen();
  }
  else
  {
    digitalWrite(TFT_BL, LOW);
    justWokeFromSleep = false;
    // Cho loop tiếp tục tác vụ định kỳ rồi quay lại Light Sleep ngay sau đó.
    lastInteraction = millis() - DISPLAY_SLEEP_TIMEOUT;
    Serial.println("[POWER] Woke from timer");
    updatePet();
  }
}

// =====================================================
// TOUCH ACTION
// =====================================================

void updateTouch()
{
  int x, y;

  bool touched = readTouch(x, y);

  if (!touched)
  {
    if (touchI2CError)
    {
      if (touchI2CErrorCount < 255) touchI2CErrorCount++;
      if (touchI2CErrorCount >= TOUCH_ERRORS_BEFORE_RECOVERY)
        recoverTouchController();
      // Khi I2C lỗi, giữ nguyên trạng thái chạm; không giả lập thao tác nhấc tay.
      return;
    }

    touchI2CErrorCount = 0;
    touchWasDown = false;
    return;
  }

  touchI2CErrorCount = 0;

  // Có thao tác -> reset bộ đếm 1 phút.
  lastInteraction = millis();
  wakeDisplay();

  // Lần chạm đầu tiên sau Light Sleep chỉ để đánh thức màn hình.
  if (justWokeFromSleep)
  {
    justWokeFromSleep = false;
    touchWasDown = true;
    return;
  }

  // Chỉ xử lý một lần cho mỗi lần chạm
  if (touchWasDown)
    return;

  touchWasDown = true;

  Serial.print("Touch X=");
  Serial.print(x);
  Serial.print(" Y=");
  Serial.println(y);

  // Chạm vào thân pet -> HAPPY trong 1 giây, không đổi chỉ số.
  // Khu vực dưới được giới hạn để không chặn các nút FEED/PLAY/SLEEP.
  bool tappedPet =
    (x >= 48 && x <= 192 && y >= 48 && y < 176) ||
    (x >= 90 && x < 155 && y >= 176 && y < 195);

  if (tappedPet)
  {
    // Không kích hoạt vui vẻ khi pet đang ngủ hoặc đang đói.
    if (pet.sleeping || pet.hunger < 30)
    {
      Serial.println("PET TAPPED: ignored while sleeping or hungry");
      return;
    }

    petTapHappyActive = true;
    petTapHappyStarted = millis();
    lastAnimation = petTapHappyStarted;
    currentFrame = FRAME_HAPPY;
    Serial.println("PET TAPPED: HAPPY for 1 second");
    drawPet();
    return;
  }

  // Bottom buttons
  if (y >= 165 && y <= 239)
  {
    if (x >= 25 && x < 90)
    {
      feedPet();
      return;
    }

    if (x >= 90 && x < 155)
    {
      playPet();
      return;
    }

    if (x >= 155 && x <= 220)
    {
      sleepPet();
      return;
    }
  }

  // Chạm vào vùng background (ngoài 3 nút bên dưới) -> đổi background.
  // Chỉ đổi trạng thái ở đây; loop() sẽ vẽ lại đúng MỘT lần.
  // Tránh drawScreen() trực tiếp vì loop() cũng phát hiện backgroundChanged,
  // nếu không toàn màn hình sẽ bị vẽ hai lần liên tiếp gây khựng.
  currentBackground = (currentBackground + 1) % BG_COUNT;
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Touch reset
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  lcd.init();
  lcd.setRotation(0);

  // Background RGB565 được lưu theo byte-order của ảnh.
  // LovyanGFX cần swap byte khi pushImage để màu không bị ám xanh lá/vàng.
  lcd.setSwapBytes(true);

  // Buffer cho animation của pet. Tách phần dưới để không đè lên icon.
  petBuffer.setColorDepth(16);
  petBuffer.createSprite(144, 128);
  petBuffer.setSwapBytes(true);

  petBottomBuffer.setColorDepth(16);
  petBottomBuffer.createSprite(96, 16);
  petBottomBuffer.setSwapBytes(true);

  pet.hunger = 80;
  pet.happiness = 80;
  pet.sleeping = false;

  updateMood();

  lastHunger = millis();
  lastHappiness = millis();

  previousMood = pet.mood;
  resetAnimationForMood();

  drawScreen();
  lastInteraction = millis();

  Serial.println();
  Serial.println("======================");
  Serial.println("      CYBER PET");
  Serial.println("======================");
  Serial.println("Touch: FEED / PLAY / SLEEP");

  // Bắt đầu đồng bộ giờ nền sau khi UI đã hiển thị; không chặn khởi động.
  startTimeSyncCycle();
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
  serviceTimeSync();
  updatePet();
  updateAnimation();
  updateTouch();

  if (pet.sleeping && !displaySleeping && millis() - lastClockDraw >= 1000UL)
  {
    lastClockDraw = millis();
    drawSleepClock();
  }

  // Tự vào Light Sleep sau 1 phút không có thao tác.
  if (!displaySleeping && timeSyncState == TIME_SYNC_IDLE &&
      millis() - lastInteraction >= DISPLAY_SLEEP_TIMEOUT)
  {
    enterDisplaySleep();
    return;
  }

  // Chỉ vẽ lại đúng vùng cần thiết:
  // - Đổi background hoặc bắt đầu/kết thúc ngủ: vẽ toàn màn hình.
  // - Đổi chỉ số/mood: vẽ pet + UI, không nạp lại background.
  // - Đổi frame animation: chỉ vẽ vùng pet.
  static PetFrame lastDrawnFrame = FRAME_IDLE_01;
  static int lastDrawnMood = MOOD_HAPPY;
  static int lastHungerDrawn = -1;
  static int lastHappinessDrawn = -1;
  static int lastDrawnBackground = -1;
  static bool lastDrawnSleeping = false;

  bool frameChanged = (currentFrame != lastDrawnFrame);
  bool moodChanged = (pet.mood != lastDrawnMood);
  bool statsChanged = (pet.hunger != lastHungerDrawn ||
                       pet.happiness != lastHappinessDrawn);
  bool backgroundChanged = (currentBackground != lastDrawnBackground);
  bool sleepStateChanged = (pet.sleeping != lastDrawnSleeping);

  if (backgroundChanged || sleepStateChanged)
  {
    drawScreen();
    lastDrawnBackground = currentBackground;
    lastDrawnSleeping = pet.sleeping;
    lastDrawnFrame = currentFrame;
    lastDrawnMood = pet.mood;
    lastHungerDrawn = pet.hunger;
    lastHappinessDrawn = pet.happiness;
  }
  else if (moodChanged || statsChanged)
  {
    drawPet();
    drawUI();

    lastDrawnFrame = currentFrame;
    lastDrawnMood = pet.mood;
    lastHungerDrawn = pet.hunger;
    lastHappinessDrawn = pet.happiness;
  }
  else if (frameChanged)
  {
    drawPet();
    lastDrawnFrame = currentFrame;
  }
}
