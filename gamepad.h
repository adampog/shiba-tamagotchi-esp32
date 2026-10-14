#pragma once
#include <stdint.h>

// Controller state, filled from the USB host task and read from loop().
struct PadState {
  bool connected;
  bool left, right, up, down;
  bool a, b, x, y;
  bool start, select;
  int8_t lx;  // left stick X, -127..127
};

void padBegin();      // start the USB host (takes over the USB-C port)
PadState padRead();   // snapshot of the current state
