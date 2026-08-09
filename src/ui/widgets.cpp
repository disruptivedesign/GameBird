#include "widgets.h"

void drawSelectorDots(Display& d, int row, int n, int sel,
                      const CRGB& on, const CRGB& off){
  if (n < 1) return;
  for (int i = 0; i < n; i++){
    int x = (2 * i + 1) * MATRIX_W / (2 * n);   // centered in its share of the row
    d.setPixel(x, row, i == sel ? on : off);
  }
}
