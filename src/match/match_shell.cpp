#include "match_shell.h"
#include "audio/sfx.h"

void drawMatchCountdown(Display& disp, Audio& audio, uint32_t elapsed) {
  // Step length is derived, not written down: retune MATCH_COUNTDOWN_MS and the
  // digits still land evenly instead of counting at the old rate.
  const uint32_t step = MATCH_COUNTDOWN_MS / MATCH_COUNTDOWN_STEPS;
  int n = MATCH_COUNTDOWN_STEPS - (int)(elapsed / step);
  if (n < 1) n = 1;                     // hold on 1 through any rounding tail

  // Beep on the digit change, not every frame. This is called at frame rate, so
  // without the edge it would be a continuous tone.
  //
  // The last-digit memory is a file static rather than caller state because the
  // firmware runs exactly one Game at a time (see main.cpp) -- two countdowns can
  // never be in flight at once. A new match restarts at elapsed 0, so `n` jumps
  // back up to MATCH_COUNTDOWN_STEPS, which is itself a change and ticks
  // correctly without needing an explicit reset.
  static int lastN = -1;
  if (n != lastN) {
    lastN = n;
    audio.play(SFX_COUNTDOWN);
  }

  disp.clear();
  disp.drawNumber((uint32_t)n, 3, 1, CRGB(255, 255, 255));
  disp.show();
}

void playMatchGo(Audio& audio) {
  audio.play(SFX_GO);
}

bool matchResultReleased(uint32_t elapsed, bool pressed) {
  if (elapsed > MATCH_RESULT_MS) return true;
  return pressed && elapsed > MATCH_RESULT_LOCKOUT_MS;
}
