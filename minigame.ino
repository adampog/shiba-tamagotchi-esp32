// Treat Catch: Biscuit runs along the floor catching falling bones and dodging soap bubbles.
// Controller: D-pad / left stick to run, A to jump, START to quit.
// Touch fallback: hold the left / right half of the bottom bar to run, tap the room to jump.

#define G_ITEMS 12
#define G_ITEM_BONE 0
#define G_ITEM_GOLD 1
#define G_ITEM_SOAP 2

enum GameState { G_READY, G_PLAY, G_OVER };

struct GItem { float x, y, vy, phase; uint8_t type; bool on; };
struct GPopup { float x, y; uint32_t until; int pts; };

GameState gState = G_READY;
GItem gItems[G_ITEMS];
GPopup gPop[4];
float gX, gY, gVy;  // dog position on the floor and jump height
bool gFaceLeft = false;
int gScore = 0, gLives = 3, gBest = 0;
uint32_t gNextSpawn = 0, gHurtUntil = 0, gHappyUntil = 0, gStart = 0;

void gameEnter() {
  gameActive = true;
  gState = G_READY;
  gBest = prefs.getInt("best", 0);
  bubbleUntil = 0;
}

void gameExit() {
  // the run counts as play time for the pet
  if (gScore > 0) {
    pet.fun = clamp100(pet.fun + min(40, gScore * 2));
    pet.energy = clamp100(pet.energy - 8);
    pet.food = clamp100(pet.food - 4);
    say(gScore >= 20 ? "Best game ever!" : "That was fun!", 2000);
  }
  savePet();
  gameActive = false;
  sitDown(millis());
}

void gameReset(uint32_t now) {
  gState = G_PLAY;
  gScore = 0;
  gLives = 3;
  gX = W / 2;
  gY = 0;
  gVy = 0;
  gStart = now;
  gNextSpawn = now + 600;
  gHurtUntil = gHappyUntil = 0;
  for (auto &it : gItems) it.on = false;
  for (auto &p : gPop) p.until = 0;
}

void gameSpawn(uint32_t now) {
  for (auto &it : gItems) {
    if (it.on) continue;
    int r = random(100);
    int soapChance = min(40, 12 + gScore);  // more soap as you score
    it.type = r < 6 ? G_ITEM_GOLD : (r < 6 + soapChance ? G_ITEM_SOAP : G_ITEM_BONE);
    it.x = random(30, W - 30);
    it.y = SCENE_Y - 10;
    it.vy = 70 + min(gScore * 4, 170) + random(0, 40);
    if (it.type == G_ITEM_SOAP) it.vy *= 0.7f;  // bubbles drift down slowly
    it.phase = random(0, 628) / 100.0f;
    it.on = true;
    break;
  }
  int gap = max(350, 1100 - gScore * 25);
  gNextSpawn = now + random(gap / 2, gap);
}

void gamePopup(float x, float y, int pts, uint32_t now) {
  for (auto &p : gPop)
    if (now > p.until) { p = { x, y, now + 700, pts }; return; }
}

void drawBoneAt(int x, int y, float s, uint16_t c) {
  int hw = 9 * s, hh = 3 * s, r = 4 * s;
  gfx->fillRoundRect(x - hw, y - hh, hw * 2, hh * 2 + 1, hh, c);
  gfx->fillCircle(x - hw, y - hh, r, c); gfx->fillCircle(x - hw, y + hh, r, c);
  gfx->fillCircle(x + hw, y - hh, r, c); gfx->fillCircle(x + hw, y + hh, r, c);
}

void drawSoap(int x, int y) {
  gfx->fillCircle(x, y, 13, rgb(150, 210, 255));
  gfx->drawCircle(x, y, 13, C_TEXT);
  gfx->fillCircle(x - 4, y - 5, 4, C_TEXT);
  gfx->fillCircle(x + 9, y - 10, 5, rgb(200, 235, 255));
  gfx->drawCircle(x + 9, y - 10, 5, C_TEXT);
}

void drawHudHeart(int x, int y, bool full) {
  heart(x, y, 7, full ? C_PINK : C_BAR_BG);
}

void gameCenterText(const char *s, int y, int size, uint16_t c) {
  gfx->setTextSize(size);
  gfx->setTextColor(c);
  gfx->setCursor(W / 2 - strlen(s) * 3 * size, y);
  gfx->print(s);
}

// One frame of the game: input, update, draw. touchDown/tx/ty is the current touch.
void gameFrame(uint32_t now, float dt, const PadState &pad, const PadState &prev, bool touchDown, bool touchTap, int tx, int ty) {
  bool aPressed = (pad.a && !prev.a) || (pad.b && !prev.b) || (touchTap && ty < BTN_Y);
  bool startPressed = pad.start && !prev.start;
  bool quitTap = touchTap && ty < SCENE_Y;  // tap the top bar to quit

  if (startPressed && gState != G_PLAY) { gameExit(); return; }
  if (quitTap) { gameExit(); return; }

  float t = now / 1000.0f;
  if (gState == G_READY || gState == G_OVER) {
    if (aPressed) gameReset(now);
  } else {
    if (startPressed) { gState = G_OVER; }

    // --- movement ---
    float dir = 0;
    if (pad.left) dir = -1;
    if (pad.right) dir = 1;
    if (pad.lx < -40 || pad.lx > 40) dir = pad.lx / 127.0f;
    if (touchDown && ty >= BTN_Y) dir = tx < W / 2 ? -1 : 1;
    gX = constrain(gX + dir * 270 * dt, 50.0f, W - 50.0f);
    if (dir < 0) gFaceLeft = true;
    if (dir > 0) gFaceLeft = false;
    bool running = dir != 0;

    if (aPressed && gY == 0) gVy = 560;  // jump
    gVy -= 1500 * dt;
    gY = max(0.0f, gY + gVy * dt);
    if (gY == 0) gVy = 0;

    // --- items ---
    if (now > gNextSpawn) gameSpawn(now);
    int dogTop = FLOOR_Y - (int)gY - 150;
    for (auto &it : gItems) {
      if (!it.on) continue;
      it.y += it.vy * dt;
      if (it.type == G_ITEM_SOAP) it.x += sinf(t * 3 + it.phase) * 40 * dt;
      bool hit = fabsf(it.x - gX) < 62 && it.y > dogTop && it.y < FLOOR_Y - gY;
      if (hit) {
        it.on = false;
        if (it.type == G_ITEM_SOAP) {
          if (now > gHurtUntil) {
            gLives--;
            gHurtUntil = now + 1200;
            if (gLives <= 0) {
              gState = G_OVER;
              if (gScore > gBest) { gBest = gScore; prefs.putInt("best", gBest); }
            }
          }
        } else {
          int pts = it.type == G_ITEM_GOLD ? 5 : 1;
          gScore += pts;
          gHappyUntil = now + 400;
          gamePopup(it.x, it.y, pts, now);
        }
      } else if (it.y > FLOOR_Y + 10) it.on = false;  // missed, falls through the floor
    }

    // --- draw scene ---
    gfx->fillScreen(C_BG);
    blit(spr_room, SPR_ROOM_W, SPR_ROOM_H, 0, SCENE_Y, false, false);
    for (auto &it : gItems) {
      if (!it.on) continue;
      if (it.type == G_ITEM_SOAP) drawSoap(it.x, it.y);
      else {
        bool gold = it.type == G_ITEM_GOLD;
        drawBoneAt(it.x, it.y, gold ? 1.4f : 1.1f, gold ? C_YELLOW : C_CREAM);
        if (gold) sparkle(it.x + 14, it.y - 12, 6 + (int)(sinf(t * 10) * 2), C_TEXT);
      }
    }

    SprId spr;
    bool flip = false;
    int dy = FLOOR_Y - (int)gY;
    float sx = 1, sy = 1;
    if (now < gHurtUntil) {
      spr = S_BATH;  // got soaped!
      if (((now / 100) & 1) == 0) spr = S_SAD;
    } else if (gY > 0) {
      spr = S_RUN2;
      flip = gFaceLeft;
      sx = 0.96f; sy = 1.05f;
    } else if (running) {
      int f = (int)(t * 12) & 1;
      spr = f ? S_RUN2 : S_RUN1;
      dy -= f ? 8 : 0;
      flip = gFaceLeft;
    } else spr = now < gHappyUntil ? S_HAPPY : S_IDLE;
    // blink while invulnerable
    if (!(now < gHurtUntil && ((now / 80) & 1))) drawDog(spr, (int)gX, dy, sx, sy, flip, false);

    for (auto &p : gPop) {
      if (now > p.until) continue;
      float k = 1 - (p.until - now) / 700.0f;
      gfx->setTextSize(p.pts > 1 ? 3 : 2);
      gfx->setTextColor(p.pts > 1 ? C_YELLOW : C_TEXT);
      gfx->setCursor(p.x - 10, p.y - 20 - k * 40);
      gfx->printf("+%d", p.pts);
    }
  }

  if (gState != G_PLAY) {
    gfx->fillScreen(C_BG);
    blit(spr_room, SPR_ROOM_W, SPR_ROOM_H, 0, SCENE_Y, false, true);
    if (gState == G_READY) {
      drawDog(S_HAPPY, W / 2, FLOOR_Y - (int)(fabsf(sinf(t * 5)) * 14), 1, 1, false, false);
      gameCenterText("TREAT CATCH", SCENE_Y + 24, 3, C_ORANGE);
      gameCenterText("Catch bones, dodge soap!", SCENE_Y + 62, 2, C_TEXT);
    } else {
      drawDog(S_SAD, W / 2, FLOOR_Y, 1, 1, false, false);
      gameCenterText("GAME OVER", SCENE_Y + 24, 3, C_RED);
      char buf[24];
      snprintf(buf, sizeof(buf), "Score %d  Best %d", gScore, gBest);
      gameCenterText(buf, SCENE_Y + 62, 2, C_TEXT);
    }
    gameCenterText(pad.connected ? "A: play   START: back" : "Tap: play  Top: back", SCENE_Y + 90, 2, C_DIM);
  }

  // --- HUD (top bar) ---
  gfx->setTextSize(3);
  gfx->setTextColor(C_ORANGE);
  gfx->setCursor(28, 12);
  gfx->printf("%d", gScore);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(28, 46);
  gfx->printf("BEST %d", max(gBest, gScore));
  for (int i = 0; i < 3; i++) drawHudHeart(W - 40 - i * 30, 26, i < gLives);
  gfx->setCursor(W - 28 - 6 * 12, 46);
  gfx->print("< BACK");

  // --- bottom bar: controls hint / touch run zones ---
  gfx->fillRoundRect(6, BTN_Y, W / 2 - 12, 74, 18, touchDown && ty >= BTN_Y && tx < W / 2 ? C_BTN_HI : C_BTN);
  gfx->fillRoundRect(W / 2 + 6, BTN_Y, W / 2 - 12, 74, 18, touchDown && ty >= BTN_Y && tx >= W / 2 ? C_BTN_HI : C_BTN);
  gfx->fillTriangle(W / 4 - 20, BTN_Y + 37, W / 4 + 12, BTN_Y + 17, W / 4 + 12, BTN_Y + 57, C_TEXT);
  gfx->fillTriangle(3 * W / 4 + 20, BTN_Y + 37, 3 * W / 4 - 12, BTN_Y + 17, 3 * W / 4 - 12, BTN_Y + 57, C_TEXT);
  if (pad.connected) {
    gfx->fillCircle(W / 2, BTN_Y + 37, 5, C_GREEN);  // controller connected indicator
  }
  gfx->flush();
}
