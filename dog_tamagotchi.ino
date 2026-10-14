// Dog Tamagotchi for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 AMOLED, QMI8658 IMU)
// Handles both hardware revisions: V1 = SH8601 display + FT3168 touch (0x38),
// V2 = CO5300 display + CST820 touch (0x15). Detected at boot from the touch chip's I2C address.
// Sprites generated with Higgsfield, converted by tools/convert_sprites.py into sprites.h.
//
// Tap the buttons to Feed / Play / Wash / Sleep. Tap the dog to pet it, tap poop to clean it.
// Shake the board to make the dog dizzy. Hold the BOOT button 3 s to adopt a new puppy.

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Preferences.h>
#include "sprites.h"
#include "pet_types.h"

// ---------- hardware ----------
#define LCD_CS 12
#define LCD_SCK 11
#define LCD_D0 4
#define LCD_D1 5
#define LCD_D2 6
#define LCD_D3 7
#define I2C_SDA 15
#define I2C_SCL 14
#define BOOT_BTN 0
#define TOUCH_ADDR_V1 0x38  // FT3168
#define TOUCH_ADDR_V2 0x15  // CST820 (same register layout from 0x02)
#define IMU_ADDR 0x6B
#define EXPANDER_ADDR 0x20

#define W 368
#define H 448
#define SCENE_Y 76
#define SCENE_H 280
#define BTN_Y 362
#define FLOOR_Y (SCENE_Y + 255)  // where the dog's paws touch the floor

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_OLED *panel = nullptr;  // SH8601 or CO5300, picked in setup()
Arduino_Canvas *gfx = nullptr;
uint8_t touchAddr = TOUCH_ADDR_V2;
Preferences prefs;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t C_BG = rgb(0, 0, 0), C_TEXT = rgb(255, 255, 255), C_DIM = rgb(110, 110, 120);
const uint16_t C_ORANGE = rgb(245, 150, 60), C_CREAM = rgb(255, 232, 200), C_RED = rgb(230, 50, 50);
const uint16_t C_YELLOW = rgb(255, 210, 60), C_BLUE = rgb(110, 190, 255), C_PINK = rgb(255, 110, 150);
const uint16_t C_GREEN = rgb(80, 210, 110), C_BTN = rgb(32, 30, 38), C_BTN_HI = rgb(70, 62, 80);
const uint16_t C_POOP = rgb(120, 72, 30), C_POOP_HI = rgb(160, 104, 52), C_BAR_BG = rgb(45, 45, 52);

// ---------- sprites ----------
struct Sprite { const uint16_t *px; uint16_t w, h; };
#define SPR(n, N) { spr_##n, SPR_##N##_W, SPR_##N##_H }
const Sprite SPRITES[] = {  // order matches SprId
  SPR(idle, IDLE), SPR(blink, BLINK), SPR(happy, HAPPY), SPR(bark, BARK), SPR(sad, SAD),
  SPR(dizzy, DIZZY), SPR(bath, BATH), SPR(sleep, SLEEP), SPR(eat, EAT), SPR(eat2, EAT2),
  SPR(walk1, WALK1), SPR(walk2, WALK2), SPR(walk3, WALK3), SPR(run1, RUN1), SPR(run2, RUN2),
};
const SprId WALK_CYCLE[4] = { S_WALK1, S_WALK2, S_WALK3, S_WALK2 };

// Copy an RGB565 image straight into the canvas framebuffer, with clipping.
void blit(const uint16_t *src, int w, int h, int x, int y, bool keyed, bool dim) {
  uint16_t *fb = gfx->getFramebuffer();
  for (int j = 0; j < h; j++) {
    int dy = y + j;
    if (dy < 0 || dy >= H) continue;
    const uint16_t *s = src + j * w;
    uint16_t *d = fb + dy * W;
    for (int i = 0; i < w; i++) {
      int dx = x + i;
      if (dx < 0 || dx >= W) continue;
      uint16_t c = s[i];
      if (keyed && c == SPR_KEY) continue;
      d[dx] = dim ? (c >> 1) & 0x7BEF : c;
    }
  }
}

// Draw a dog sprite standing on footY, centred on cx, scaled by sx/sy (squash & stretch,
// nearest-neighbour) and optionally mirrored. Returns the y of the top of the sprite.
int drawDog(SprId id, int cx, int footY, float sx, float sy, bool flip, bool dim) {
  const Sprite &s = SPRITES[id];
  int dw = (int)(s.w * sx), dh = (int)(s.h * sy);
  int x0 = cx - dw / 2, y0 = footY - dh;
  uint32_t stepX = (s.w << 16) / dw, stepY = (s.h << 16) / dh;
  uint16_t *fb = gfx->getFramebuffer();
  for (int j = 0; j < dh; j++) {
    int y = y0 + j;
    if (y < 0 || y >= H) continue;
    const uint16_t *row = s.px + ((j * stepY) >> 16) * s.w;
    uint16_t *d = fb + y * W;
    for (int i = 0; i < dw; i++) {
      int x = x0 + i;
      if (x < 0 || x >= W) continue;
      int si = (i * stepX) >> 16;
      uint16_t c = row[flip ? s.w - 1 - si : si];
      if (c == SPR_KEY) continue;
      d[x] = dim ? (c >> 1) & 0x7BEF : c;
    }
  }
  return y0;
}

// ---------- pet state ----------
#define PET_MAGIC 0xD0601234
struct Pet {
  uint32_t magic;
  float food, fun, energy, clean;
  float ageSec;
  float poopTimer;  // seconds until next poop, <0 = none pending
  uint8_t poops;
  bool asleep;
} pet;

Act act = A_IDLE;
uint32_t actStart = 0, actLen = 0;
float dogX = W / 2, targetX = W / 2;
Mode mode = M_SIT;
uint32_t modeUntil = 0;     // when the current sit ends
int zoomPasses = 0;         // remaining dashes during zoomies
bool facingLeft = false;    // side-view sprites face right; mirror when going left
uint32_t landedAt = 0;      // last time the dog "landed" -> squash
float ballX = W / 2;        // play: ball position
uint32_t nextBlink = 0, blinkUntil = 0, nextChatter = 0, nextSave = 0;
char bubble[28] = "";
uint32_t bubbleUntil = 0;
int pressedBtn = -1;
uint32_t pressedUntil = 0, lastTouch = 0;
bool dimmed = false;
const int POOP_X[4] = { 64, 304, 118, 252 };

void resetPet() {
  pet = { PET_MAGIC, 80, 80, 90, 100, 0, -1, 0, false };
}

void savePet() { prefs.putBytes("pet", &pet, sizeof(pet)); }

void loadPet() {
  if (prefs.getBytesLength("pet") != sizeof(pet) || prefs.getBytes("pet", &pet, sizeof(pet)) != sizeof(pet) || pet.magic != PET_MAGIC)
    resetPet();
}

float clamp100(float v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

void say(const char *txt, uint32_t ms) {
  strncpy(bubble, txt, sizeof(bubble) - 1);
  bubble[sizeof(bubble) - 1] = 0;
  bubbleUntil = millis() + ms;
}

void startAct(Act a, uint32_t len) {
  act = a;
  actStart = millis();
  actLen = len;
  mode = M_SIT;
  if (a == A_PLAY) ballX = dogX < W / 2 ? W - 50 : 50;  // throw the ball to the far side
}

void sitDown(uint32_t now) {
  mode = M_SIT;
  modeUntil = now + random(2500, 6000);
  landedAt = now;
}

void finishAct() {
  switch (act) {
    case A_EAT:
      pet.food = clamp100(pet.food + 35);
      pet.fun = clamp100(pet.fun + 3);
      if (pet.poopTimer < 0) pet.poopTimer = random(60, 150);
      say("Yum!", 1500);
      break;
    case A_PLAY:
      pet.fun = clamp100(pet.fun + 30);
      pet.energy = clamp100(pet.energy - 12);
      pet.food = clamp100(pet.food - 5);
      break;
    case A_BATH:
      pet.clean = 100;
      pet.poops = 0;
      pet.fun = clamp100(pet.fun - 3);
      say("Brrr! So fresh!", 1800);
      break;
    case A_PET: pet.fun = clamp100(pet.fun + 8); break;
    default: break;
  }
  act = A_IDLE;
  sitDown(millis());
  savePet();
}

// ---------- buttons ----------
void onButton(int b) {
  pressedBtn = b;
  pressedUntil = millis() + 180;
  if (pet.asleep && b != 3) { say("Zzz...", 1200); return; }
  if (act != A_IDLE && act != A_BARK) return;
  switch (b) {
    case 0:
      if (pet.food > 90) { startAct(A_NO, 1200); say("I'm full!", 1500); }
      else startAct(A_EAT, 3500);
      break;
    case 1:
      if (pet.energy < 15) { startAct(A_NO, 1200); say("Too sleepy...", 1500); }
      else startAct(A_PLAY, 6000);
      break;
    case 2: startAct(A_BATH, 4300); break;  // 3.5 s soapy + 0.8 s shake-dry
    case 3:
      if (pet.asleep) { pet.asleep = false; say("Good morning!", 1800); }
      else if (pet.energy > 80) { startAct(A_NO, 1200); say("Not sleepy!", 1500); }
      else { pet.asleep = true; act = A_IDLE; bubbleUntil = 0; }
      savePet();
      break;
  }
}

void onTap(int x, int y) {
  if (y >= BTN_Y) { onButton(constrain(x / (W / 4), 0, 3)); return; }
  // poop?
  for (int i = 0; i < pet.poops; i++) {
    if (abs(x - POOP_X[i]) < 26 && abs(y - (FLOOR_Y + 8)) < 30) {
      pet.poops--;
      pet.clean = clamp100(pet.clean + 10);
      say("Thanks!", 1000);
      return;
    }
  }
  // dog?
  if (abs(x - (int)dogX) < 80 && y > FLOOR_Y - 210 && y < FLOOR_Y + 10) {
    if (pet.asleep) say("Zzz...", 1200);
    else if (act == A_IDLE || act == A_BARK) startAct(A_PET, 2200);
  }
}

// ---------- touch / IMU / expander ----------
bool i2cWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(addr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void expanderInit() {
  // TCA9554: pins 0-2 drive the LCD / touch reset lines. Pulse them low then high.
  if (!i2cWrite(EXPANDER_ADDR, 0x03, 0xF8)) return;
  i2cWrite(EXPANDER_ADDR, 0x01, 0x00);
  delay(20);
  i2cWrite(EXPANDER_ADDR, 0x01, 0x07);  // P0 LCD_RST, P1 display power, P2 TOUCH_RST all high
  delay(150);
}

bool readTouch(int &x, int &y) {
  uint8_t b[5];
  if (!i2cRead(touchAddr, 0x02, b, 5)) return false;
  uint8_t n = b[0] & 0x0F;
  if (n == 0 || n > 2) return false;
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
  return x < W && y < H;
}

bool imuOk = false;
float shakeEnergy = 0;
uint32_t shakeCooldown = 0;

void imuInit() {
  uint8_t id;
  if (!i2cRead(IMU_ADDR, 0x00, &id, 1) || id != 0x05) return;
  i2cWrite(IMU_ADDR, 0x02, 0x40);  // CTRL1: register auto-increment
  i2cWrite(IMU_ADDR, 0x03, 0x15);  // CTRL2: accel +-4g
  i2cWrite(IMU_ADDR, 0x08, 0x01);  // CTRL7: accel on
  imuOk = true;
}

bool checkShake() {
  if (!imuOk) return false;
  uint8_t b[6];
  if (!i2cRead(IMU_ADDR, 0x35, b, 6)) return false;
  float ax = (int16_t)(b[0] | b[1] << 8) / 8192.0f;
  float ay = (int16_t)(b[2] | b[3] << 8) / 8192.0f;
  float az = (int16_t)(b[4] | b[5] << 8) / 8192.0f;
  float g = sqrtf(ax * ax + ay * ay + az * az);
  shakeEnergy = shakeEnergy * 0.85f + fabsf(g - 1.0f);
  if (shakeEnergy > 3.0f && millis() > shakeCooldown) {
    shakeEnergy = 0;
    shakeCooldown = millis() + 3000;
    return true;
  }
  return false;
}

// ---------- little drawings ----------
void heart(int x, int y, int s, uint16_t c) {
  gfx->fillCircle(x - s / 2, y, s / 2 + 1, c);
  gfx->fillCircle(x + s / 2, y, s / 2 + 1, c);
  gfx->fillTriangle(x - s - 1, y + 1, x + s + 1, y + 1, x, y + s + 3, c);
}

void sparkle(int x, int y, int r, uint16_t c) {
  gfx->fillTriangle(x, y - r, x - r / 3, y, x + r / 3, y, c);
  gfx->fillTriangle(x, y + r, x - r / 3, y, x + r / 3, y, c);
  gfx->fillTriangle(x - r, y, x, y - r / 3, x, y + r / 3, c);
  gfx->fillTriangle(x + r, y, x, y - r / 3, x, y + r / 3, c);
}

void iconBone(int x, int y) {
  gfx->fillRoundRect(x - 8, y - 2, 16, 5, 2, C_CREAM);
  gfx->fillCircle(x - 8, y - 2, 3, C_CREAM); gfx->fillCircle(x - 8, y + 2, 3, C_CREAM);
  gfx->fillCircle(x + 8, y - 2, 3, C_CREAM); gfx->fillCircle(x + 8, y + 2, 3, C_CREAM);
}
void iconBall(int x, int y) {
  gfx->fillCircle(x, y, 8, C_RED);
  gfx->fillCircle(x - 3, y - 3, 2, C_TEXT);
}
void iconBolt(int x, int y) {
  gfx->fillTriangle(x + 3, y - 10, x - 6, y + 1, x + 1, y + 1, C_YELLOW);
  gfx->fillTriangle(x - 1, y - 1, x + 6, y - 1, x - 3, y + 10, C_YELLOW);
}
void iconDrop(int x, int y) {
  gfx->fillCircle(x, y + 3, 6, C_BLUE);
  gfx->fillTriangle(x, y - 10, x - 6, y + 2, x + 6, y + 2, C_BLUE);
}
void iconMoon(int x, int y, uint16_t bg) {
  gfx->fillCircle(x, y, 8, C_YELLOW);
  gfx->fillCircle(x + 5, y - 3, 7, bg);
}
void iconSun(int x, int y) {
  gfx->fillCircle(x, y, 6, C_YELLOW);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    gfx->drawLine(x + cosf(a) * 8, y + sinf(a) * 8, x + cosf(a) * 11, y + sinf(a) * 11, C_YELLOW);
  }
}

void poop(int x, int y, float t, int k) {
  gfx->fillEllipse(x, y, 16, 7, C_POOP);
  gfx->fillEllipse(x, y - 7, 12, 6, C_POOP);
  gfx->fillEllipse(x, y - 13, 7, 5, C_POOP);
  gfx->fillTriangle(x + 1, y - 22, x - 4, y - 15, x + 4, y - 15, C_POOP);
  gfx->fillCircle(x - 5, y - 9, 2, C_POOP_HI);
  for (int f = 0; f < 2; f++) {  // flies
    float a = t * (5 + f) + k + f * 3;
    gfx->fillCircle(x + cosf(a) * 18, y - 28 + sinf(a * 1.7f) * 6, 2, C_DIM);
  }
}

void drawBubble(int ax, int ay) {
  gfx->setTextSize(2);
  int len = strlen(bubble);
  int bw = len * 12 + 20, bh = 32;
  bool right = ax + 30 + bw < W - 8;
  int bx = right ? ax + 30 : ax - 30 - bw;
  if (bx < 8) bx = 8;
  int by = max(ay - bh, SCENE_Y + 6);
  gfx->fillRoundRect(bx, by, bw, bh, 14, C_TEXT);
  int tx = right ? bx + 14 : bx + bw - 14;
  gfx->fillTriangle(tx - 8, by + bh - 2, tx + 8, by + bh - 2, right ? ax + 10 : ax - 10, by + bh + 12, C_TEXT);
  gfx->setTextColor(C_BG);
  gfx->setCursor(bx + 10, by + 9);
  gfx->print(bubble);
}

void statBar(int x, int y, float v) {
  gfx->fillRoundRect(x, y, 54, 10, 5, C_BAR_BG);
  uint16_t c = v > 50 ? C_GREEN : (v > 25 ? C_YELLOW : C_RED);
  int w = (int)(54 * v / 100);
  if (w > 4) gfx->fillRoundRect(x, y, w, 10, 5, c);
}

void drawTopBar() {
  gfx->setTextSize(3);
  gfx->setTextColor(C_ORANGE);
  gfx->setCursor(28, 12);
  gfx->print("BISCUIT");
  int day = (int)(pet.ageSec / 3600) + 1;  // one dog day = one hour switched on
  const char *stage = day < 3 ? "PUPPY" : (day < 8 ? "JUNIOR" : "ADULT");
  char buf[24];
  snprintf(buf, sizeof(buf), "%s D%d", stage, day);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(W - 28 - strlen(buf) * 12, 18);
  gfx->print(buf);

  int y = 52;
  iconBone(34, y + 5);  statBar(48, y, pet.food);
  iconBall(118, y + 5); statBar(132, y, pet.fun);
  iconBolt(202, y + 5); statBar(216, y, pet.energy);
  iconDrop(286, y + 5); statBar(300, y, pet.clean);
}

void drawButtons() {
  const char *labels[4] = { "FEED", "PLAY", "WASH", pet.asleep ? "WAKE" : "SLEEP" };
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    int x0 = i * (W / 4);
    int cx = x0 + W / 8;
    bool hi = pressedBtn == i && now < pressedUntil;
    uint16_t bg = hi ? C_BTN_HI : C_BTN;
    gfx->fillRoundRect(x0 + 6, BTN_Y, W / 4 - 12, 74, 18, bg);
    switch (i) {
      case 0: iconBone(cx, BTN_Y + 24); break;
      case 1: iconBall(cx, BTN_Y + 24); break;
      case 2: iconDrop(cx, BTN_Y + 22); break;
      case 3: if (pet.asleep) iconSun(cx, BTN_Y + 24); else iconMoon(cx, BTN_Y + 24, bg); break;
    }
    gfx->setTextSize(2);
    gfx->setTextColor(pet.asleep && i != 3 ? C_DIM : C_TEXT);
    gfx->setCursor(cx - strlen(labels[i]) * 6, BTN_Y + 46);
    gfx->print(labels[i]);
  }
}

// ---------- simulation ----------
void updateStats(float dt) {
  pet.ageSec += dt;
  if (pet.asleep) {
    pet.food = clamp100(pet.food - dt / 90);
    pet.energy = clamp100(pet.energy + dt / 2.4f);
    if (pet.energy >= 100) { pet.asleep = false; say("Good morning!", 2000); savePet(); }
    return;
  }
  pet.food = clamp100(pet.food - dt / 40);
  pet.fun = clamp100(pet.fun - dt / 30);
  pet.energy = clamp100(pet.energy - dt / 75);
  pet.clean = clamp100(pet.clean - dt * (1 + pet.poops) / 100);
  if (pet.poopTimer >= 0) {
    pet.poopTimer -= dt;
    if (pet.poopTimer < 0 && pet.poops < 4) { pet.poops++; say("Oops!", 1200); }
  }
  if (pet.energy <= 0) { pet.asleep = true; act = A_IDLE; say("So tired...", 1500); }
}

// Move toward targetX at speed px/s. Returns true when arrived.
bool moveDog(float speed, float dt) {
  float d = targetX - dogX;
  if (fabsf(d) < 3) return true;
  facingLeft = d < 0;
  float step = speed * dt;
  dogX += fabsf(d) < step ? d : (d > 0 ? step : -step);
  return false;
}

void idleBehaviour(uint32_t now, float dt) {
  switch (mode) {
    case M_SIT:
      if (now > modeUntil) {
        bool hyper = pet.fun > 60 && pet.energy > 50 && pet.food > 40;
        if (hyper && random(5) == 0) {  // zoomies!
          mode = M_ZOOM;
          zoomPasses = random(3, 6);
          targetX = dogX < W / 2 ? W - 70 : 70;
        } else {                        // stroll somewhere new
          mode = M_WALK;
          do targetX = random(90, W - 90); while (fabsf(targetX - dogX) < 50);
        }
      }
      break;
    case M_WALK:
      if (moveDog(pet.energy < 25 ? 35 : 60, dt)) sitDown(now);
      break;
    case M_ZOOM:
      if (moveDog(240, dt)) {
        if (--zoomPasses <= 0) { sitDown(now); say("Huff huff!", 1500); pet.energy = clamp100(pet.energy - 2); }
        else targetX = targetX < W / 2 ? W - 70 : 70;
      }
      break;
  }
  if (mode != M_SIT) return;  // blinks and chatter only while sitting

  if (now > nextBlink) {
    blinkUntil = now + 140;
    nextBlink = now + random(2000, 5000);
  }
  if (now > nextChatter) {
    nextChatter = now + random(12000, 25000);
    float lowest = min(min(pet.food, pet.fun), min(pet.energy, pet.clean));
    if (lowest < 30) {
      if (lowest == pet.food) say("I'm hungry...", 2200);
      else if (lowest == pet.fun) say("Play with me!", 2200);
      else if (lowest == pet.energy) say("So sleepy...", 2200);
      else say("I feel itchy...", 2200);
    } else if (random(3) == 0) {
      startAct(A_BARK, 1200);
      say("Woof!", 1200);
    }
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  pinMode(BOOT_BTN, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  expanderInit();

  // V1 boards have an FT3168 at 0x38; otherwise assume V2 (CST820 at 0x15, CO5300 panel).
  Wire.beginTransmission(TOUCH_ADDR_V1);
  bool v1 = Wire.endTransmission() == 0;
  touchAddr = v1 ? TOUCH_ADDR_V1 : TOUCH_ADDR_V2;
  if (v1) panel = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, W, H);
  else panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, W, H, 16, 0, 0, 0);
  gfx = new Arduino_Canvas(W, H, panel);

  bool ok = gfx->begin(80000000);
  panel->setBrightness(220);
  imuInit();
  Serial.printf("board V%d display=%d psram=%u imu=%d\n", v1 ? 1 : 2, ok, ESP.getPsramSize(), imuOk);

  prefs.begin("dogtama", false);
  loadPet();
  dogX = targetX = W / 2;
  lastTouch = millis();
  say(pet.ageSec < 5 ? "Hi! I'm Biscuit!" : "Welcome back!", 2500);
}

void loop() {
  static uint32_t lastFrame = 0, bootHeld = 0;
  static bool wasDown = false;
  uint32_t now = millis();
  if (now - lastFrame < 33) return;  // ~30 fps
  float dt = (now - lastFrame) / 1000.0f;
  if (dt > 0.5f) dt = 0.5f;
  lastFrame = now;

  // BOOT button long-press = new puppy
  if (digitalRead(BOOT_BTN) == LOW) {
    if (!bootHeld) bootHeld = now;
    else if (now - bootHeld > 3000) { resetPet(); savePet(); act = A_IDLE; say("Hi! I'm Biscuit!", 2500); bootHeld = 0xFFFFFFFF; }
  } else bootHeld = 0;

  // touch
  int tx, ty;
  bool down = readTouch(tx, ty);
  if (down && !wasDown) {
    lastTouch = now;
    if (dimmed) { dimmed = false; panel->setBrightness(220); }
    else onTap(tx, ty);
  }
  wasDown = down;
  if (!dimmed && now - lastTouch > 45000) { dimmed = true; panel->setBrightness(40); }

  if (checkShake()) {
    lastTouch = now;
    if (dimmed) { dimmed = false; panel->setBrightness(220); }
    if (pet.asleep) { pet.asleep = false; say("Huh?!", 1500); }
    else { startAct(A_DIZZY, 2500); say("Whoa!", 1500); }
  }

  updateStats(dt);
  if (act != A_IDLE && now - actStart > actLen) finishAct();
  if (act == A_IDLE && !pet.asleep) idleBehaviour(now, dt);
  if (now > nextSave) { savePet(); nextSave = now + 30000; }

  // play: the ball rolls/bounces to a spot, the dog runs after it, then it gets thrown again
  if (act == A_PLAY) {
    targetX = ballX + (ballX > dogX ? -80 : 80);  // stop with the mouth at the ball
    if (moveDog(230, dt)) {
      ballX = dogX < W / 2 ? random(W / 2 + 60, W - 40) : random(40, W / 2 - 60);
      landedAt = now;
    }
  }

  // ---- draw ----
  float t = now / 1000.0f;
  float at = (now - actStart) / 1000.0f;
  bool night = pet.asleep;
  gfx->fillScreen(C_BG);
  blit(spr_room, SPR_ROOM_W, SPR_ROOM_H, 0, SCENE_Y, false, night);

  for (int i = 0; i < pet.poops; i++) poop(POOP_X[i], FLOOR_Y + 14, t, i);

  float lowest = min(min(pet.food, pet.fun), min(pet.energy, pet.clean));
  int dx = (int)dogX, dy = FLOOR_Y;
  bool flip = false;
  // breathing: gentle vertical stretch with matching horizontal squeeze
  float breath = sinf(t * 2.6f) * 0.018f;
  float sx = 1 - breath * 0.6f, sy = 1 + breath;
  SprId spr = lowest < 25 ? S_SAD : S_IDLE;
  if (spr == S_IDLE && now < blinkUntil) spr = S_BLINK;

  if (pet.asleep) {
    spr = S_SLEEP;
    float b = sinf(t * 1.4f) * 0.035f;  // slow sleepy breathing
    sx = 1 - b * 0.5f; sy = 1 + b;
  } else switch (act) {
    case A_IDLE:
      if (mode == M_WALK) {
        int f = (int)(t * 7) & 3;
        spr = WALK_CYCLE[f];
        dy -= (f & 1) ? 3 : 0;  // body rises on the passing frames
        flip = facingLeft; sx = sy = 1;
      } else if (mode == M_ZOOM) {
        int f = (int)(t * 12) & 1;
        spr = f ? S_RUN2 : S_RUN1;
        dy -= f ? 10 : 2;
        flip = facingLeft; sx = 1.05f; sy = 0.97f;
      }
      break;
    case A_EAT:
      spr = ((int)(at * 3) & 1) ? S_EAT2 : S_EAT;  // dip into the bowl, lift and chew
      break;
    case A_PLAY: {
      bool running = fabsf(targetX - dogX) > 3;
      if (running) {
        int f = (int)(t * 12) & 1;
        spr = f ? S_RUN2 : S_RUN1;
        dy -= f ? 12 : 2;
        flip = facingLeft; sx = sy = 1;
      } else spr = S_HAPPY;
      break;
    }
    case A_PET: {
      spr = S_HAPPY;
      float hop = fabsf(sinf(at * 7));  // happy little hops
      dy -= (int)(hop * 16);
      if (hop < 0.25f) { sx = 1.08f; sy = 0.93f; } else { sx = 0.96f; sy = 1.05f; }
      break;
    }
    case A_BATH:
      if (at < 3.5f) { spr = S_BATH; dx += (int)(sinf(at * 25) * 3); }
      else {  // shake dry: fast side-to-side wiggle
        spr = S_BLINK;
        dx += (int)(sinf(at * 60) * 9);
        sx = 1.04f; sy = 0.97f;
      }
      break;
    case A_NO: dx += (int)(sinf(at * 22) * 6); break;
    case A_DIZZY: {
      spr = S_DIZZY;
      float w = sinf(at * 6);
      dx += (int)(w * 10);
      sx = 1 + w * 0.05f; sy = 1 - w * 0.04f;
      break;
    }
    case A_BARK: {
      spr = S_BARK;
      float k = fabsf(sinf(at * 9));  // each "woof" stretches up
      sx = 1 - k * 0.04f; sy = 1 + k * 0.06f;
      break;
    }
    default: break;
  }
  // landing squash after sitting down / catching the ball
  if (now - landedAt < 220 && !pet.asleep && (act == A_IDLE || act == A_PLAY) && mode == M_SIT) {
    float k = 1 - (now - landedAt) / 220.0f;
    sx += 0.08f * k; sy -= 0.08f * k;
  }
  int headY = drawDog(spr, dx, dy, sx, sy, flip, night) + 40;

  // effects
  if (pet.asleep) {
    gfx->setTextColor(C_BLUE);
    for (int k = 0; k < 3; k++) {
      float p = fmodf(t * 0.4f + k / 3.0f, 1.0f);
      gfx->setTextSize(p < 0.5f ? 2 : 3);
      gfx->setCursor(dx + 50 + p * 40, FLOOR_Y - 110 - p * 80);
      gfx->print("Z");
    }
  } else switch (act) {
    case A_PET:
      for (int k = 0; k < 4; k++) {
        float p = fmodf(at * 0.9f + k * 0.25f, 1.0f);
        heart(dx + sinf(k * 2.1f + at * 3) * 50, headY - p * 70, 7, C_PINK);
      }
      break;
    case A_PLAY: {
      int bx = (int)ballX;
      int by = FLOOR_Y - 14 - (int)(fabsf(sinf(at * 5.5f)) * 70);
      gfx->fillCircle(bx, by, 13, C_RED);
      gfx->fillRect(bx - 13, by - 2, 27, 4, C_TEXT);
      gfx->fillCircle(bx - 5, by - 6, 3, C_TEXT);
      break;
    }
    case A_BATH:
      if (at < 3.5f) {
        for (int k = 0; k < 10; k++) {
          float p = fmodf(at * 0.7f + k * 0.1f, 1.0f);
          int bx = dx + ((k * 37) % 140 - 70) + (int)(sinf(at * 3 + k) * 6);
          int by = FLOOR_Y - 20 - p * 200;
          int r = 4 + (k % 3) * 3;
          gfx->drawCircle(bx, by, r, C_TEXT);
          gfx->fillCircle(bx - r / 3, by - r / 3, 1, C_TEXT);
        }
      } else {  // water droplets flung off while shaking dry
        float p = (at - 3.5f) / 0.8f;
        for (int k = 0; k < 14; k++) {
          float a = k * 0.45f - 0.4f + (k & 1) * PI;
          float r = 50 + p * 110 + (k % 3) * 12;
          int x = dx + cosf(a) * r, y = FLOOR_Y - 110 + sinf(a) * r * 0.5f + p * p * 60;
          gfx->fillCircle(x, y, 3, C_BLUE);
          gfx->fillCircle(x, y - 3, 2, C_BLUE);
        }
        for (int k = 0; k < 3; k++) sparkle(dx - 60 + k * 60, headY + 10 + (k % 2) * 40, 9, C_YELLOW);
      }
      break;
    case A_EAT:
      for (int k = 0; k < 3; k++) {
        float p = fmodf(at * 1.5f + k * 0.33f, 1.0f);
        gfx->fillCircle(dx - 30 + k * 30, FLOOR_Y - 50 - p * 25, 2, C_POOP_HI);
      }
      break;
    case A_DIZZY:
      for (int k = 0; k < 3; k++) {
        float a = at * 5 + k * 2.09f;
        sparkle(dx + cosf(a) * 60, headY - 10 + sinf(a) * 12, 8, C_YELLOW);
      }
      break;
    default: break;
  }

  if (now < bubbleUntil) drawBubble(dx, headY - 10);
  drawTopBar();
  drawButtons();
  gfx->flush();
}
