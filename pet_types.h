#pragma once
// Kept in a header so Arduino's auto-generated prototypes can see these types.
enum SprId {
  S_IDLE, S_BLINK, S_HAPPY, S_BARK, S_SAD, S_DIZZY, S_BATH, S_SLEEP,
  S_EAT, S_EAT2, S_WALK1, S_WALK2, S_WALK3, S_RUN1, S_RUN2
};
enum Act { A_IDLE, A_EAT, A_PLAY, A_PET, A_BATH, A_NO, A_DIZZY, A_BARK };
// What the dog does on its own while no activity is running.
enum Mode { M_SIT, M_WALK, M_ZOOM };
