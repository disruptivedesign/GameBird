#include "tetris.h"
#include <esp_system.h>       // esp_random()
#include "pieces.h"           // COLORS, GHOST_COLOR, ICON_PAL, IMG_*, shapeCell
#include "ui/scoreboard.h"    // drawScoreScroll / drawScoreScrollLoop
#include "audio/sfx.h"
#include "audio/notes.h"

// ---- Presentation only. The feel constants moved into tetris_sim.cpp with the
// ---- rules they belong to; what is left here is animation length.
#define CLEAR_ANIM_MS    280     // line-clear fade duration
#define LEVEL_FLASH_MS   250
#define GAMEOVER_FLASH_MS 700
#define NUM_MENU_PAGES   3       // 0 = normal, 1 = ghost, 2 = high score
#define DROP_ARM_MS     90       // stick must rest near centre this long to
                                 // re-arm the hard drop (see servicePlaying)

// ---- Playfield / info panel geometry ----------------------------------------
// The well is 8 wide wherever there is room for a panel beside it, which is the
// real Tetris aspect ratio and something an 8x8 board could never be. On a panel
// too narrow to split, the well takes the whole screen exactly as before.
#define TET_WELL_W       8
#define TET_SPLIT_MIN_W  12      // narrower than this: no panel, well fills it

static inline int wellW()    { return MATRIX_W >= TET_SPLIT_MIN_W ? TET_WELL_W : MATRIX_W; }
static inline bool hasPanel(){ return MATRIX_W >= TET_SPLIT_MIN_W; }
static inline int panelX()   { return wellW() + 1; }        // past the divider
static inline int panelW()   { return MATRIX_W - panelX(); }

// Panel rows, top to bottom: next piece, level, lines-to-next-level.
#define PANEL_NEXT_ROW   1
#define PANEL_LEVEL_ROW  7
#define PANEL_BAR_ROW   13

#define NS_TETRIS  "tetris"   // storage namespace
#define KEY_HIGH   "hi"       // high-score key

// ---- Tetris's own voices ----------------------------------------------------
// The shared catalog in sfx.h covers what Tetris has in common with the rest of
// the device -- navigation, and SFX_LOSE on a top-out. These five describe
// things only Tetris does.
//
// Two constraints shaped them. Rotate and lock can fire several times a second,
// so they are single ticks with no tail: a rotate is a high click, a lock is a
// low chunk, and the wide interval between them is what makes a stack audibly
// settle rather than chatter. The clear stingers get to be longer because they
// play during T_CLEARING and own that whole window -- the four-row fanfare is
// deliberately sized to just fit inside CLEAR_ANIM_MS rather than get cut off.
static const Step S_ROTATE[] = { {NOTE_A6, 12} };
static const Step S_LOCK[]   = { {NOTE_C5, 22} };
// A hard drop is the same event as a lock but arrived at deliberately, so it
// gets the same shape an octave down and a touch longer -- audibly a thud
// rather than the click of a piece being set down.
static const Step S_SLAM[]   = { {NOTE_C4, 38} };
static const Step S_CLEAR[]  = { {NOTE_G6, 40}, {NOTE_C7, 70} };
static const Step S_TETRIS[] = { {NOTE_C6, 50}, {NOTE_E6, 50}, {NOTE_G6, 50}, {NOTE_C7, 120} };
static const Step S_LEVEL[]  = { {NOTE_C6, 45}, {NOTE_G6, 45}, {NOTE_C7, 110} };

static const Sfx SFX_TET_ROTATE = SFX_OF(S_ROTATE, PRIO_MINOR);
static const Sfx SFX_TET_LOCK   = SFX_OF(S_LOCK,   PRIO_MINOR);
static const Sfx SFX_TET_SLAM   = SFX_OF(S_SLAM,   PRIO_MINOR);
static const Sfx SFX_TET_CLEAR  = SFX_OF(S_CLEAR,  PRIO_MAJOR);
// PRIO_MATCH: clearing four at once is the best thing that happens in Tetris,
// and nothing should be able to talk over it.
static const Sfx SFX_TET_TETRIS = SFX_OF(S_TETRIS, PRIO_MATCH);
static const Sfx SFX_TET_LEVEL  = SFX_OF(S_LEVEL,  PRIO_MAJOR);

// ============================================================================
//  Lifecycle
// ============================================================================
void Tetris::begin(){
  _highScore = _sys.storage.getU32(NS_TETRIS, KEY_HIGH, 0);
  _state = T_MENU;         // the boot splash already ran at system startup
  _menuPage = 0;
  _ghostEnabled = false;
  _exitReq = false;
}

GameStatus Tetris::service(){
  uint32_t now = millis();
  switch (_state){
    case T_MENU:     updateMenu(now);     return _exitReq ? GAME_EXIT : GAME_CONTINUE;
    case T_CLEARING: updateClearing(now); return GAME_CONTINUE;
    case T_GAMEOVER: updateGameOver(now); return GAME_CONTINUE;
    case T_PLAYING:  servicePlaying(now); return GAME_CONTINUE;
  }
  return GAME_CONTINUE;
}

// Top-level menu identity: the purple T tetromino.
Icon Tetris::menuIcon() const { return { IMG_NORMAL, ICON_PAL }; }

void Tetris::startGame(){
  _sim.newGame((uint8_t)wellW(), MATRIX_H, esp_random());
  _newHigh = false;
  _levelFlashUntil = 0;
  _lastUpdate = millis();
  // Disarmed on entry: A launched the game, and a stick still travelling from
  // the menu must not spend the first piece.
  _dropArmed = false;
  _neutralAt = _lastUpdate;
  _state = T_PLAYING;
}

// One sound per update, biggest moment wins -- the same shape as Breakout's.
//
// TET_EV_MOVE is deliberately silent. DAS repeats every 50 ms, so a blip on
// every horizontal step would fire twenty times a second and smear into a
// drone: exactly the failure sfx.h describes for menu navigation, and sideways
// motion is already completely legible on the panel without it.
void Tetris::sound(uint8_t ev){
  if      (ev & TET_EV_LEVEL)    _sys.audio.play(SFX_TET_LEVEL);
  else if (ev & TET_EV_LINES)    _sys.audio.play(_sim.clearCount() >= 4 ? SFX_TET_TETRIS
                                                                       : SFX_TET_CLEAR);
  else if (ev & TET_EV_HARDDROP) _sys.audio.play(SFX_TET_SLAM);
  else if (ev & TET_EV_LOCK)     _sys.audio.play(SFX_TET_LOCK);
  else if (ev & TET_EV_ROTATE)   _sys.audio.play(SFX_TET_ROTATE);
}

// A spawn had nowhere to go. Reached from both update() and commitClear(), so
// the persist lives here rather than at either call site.
void Tetris::enterGameOver(uint32_t now){
  _sys.audio.play(SFX_LOSE);
  _newHigh = _sim.score() > _highScore;
  if (_newHigh){
    _highScore = _sim.score();
    _sys.storage.putU32(NS_TETRIS, KEY_HIGH, _highScore);
  }
  _gameOverStart = now;
  _state = T_GAMEOVER;
}

// ============================================================================
//  States
// ============================================================================
void Tetris::updateMenu(uint32_t now){
  (void)now;
  if (int8_t s = _sys.joy.stepX()){
    _menuPage = (_menuPage + s + NUM_MENU_PAGES) % NUM_MENU_PAGES;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()){                              // back to main menu
    _sys.audio.play(SFX_BACK);
    _exitReq = true;
    return;
  }
  if (_sys.buttonA.wasPressed() && _menuPage < 2){
    _sys.audio.play(SFX_SELECT);
    _ghostEnabled = (_menuPage == 1);
    startGame();
    return;
  }
  renderMenu();
}

void Tetris::updateClearing(uint32_t now){
  uint32_t t = now - _clearStart;
  if (t >= CLEAR_ANIM_MS){
    uint8_t ev = _sim.commitClear();
    _lastUpdate = now;                 // the sim was frozen for the animation
    if (ev & TET_EV_GAMEOVER){ enterGameOver(now); return; }
    sound(ev);
    if (ev & TET_EV_LEVEL) _levelFlashUntil = now + LEVEL_FLASH_MS;
    _state = T_PLAYING;
    return;
  }
  renderClearing(t);
}

void Tetris::updateGameOver(uint32_t now){
  uint32_t t = now - _gameOverStart;
  bool skip = _sys.buttonA.wasPressed() || _sys.buttonB.wasPressed();

  if (t < GAMEOVER_FLASH_MS){
    if ((t / 150) & 1) disp().clear();
    else               disp().fill(CRGB::Red);
    disp().show();
    return;
  }

  disp().clear();
  bool done = drawScoreScroll(disp(), _sim.score(), t - GAMEOVER_FLASH_MS,
                              _newHigh ? CRGB(255, 180, 0) : CRGB(0, 150, 255));
  disp().show();

  if (done || skip) startGame();
}

// The rev 2 control map:
//
//   left / right   move, with DAS in the sim
//   down           soft drop  (was: hold B)
//   up             hard drop  (an edge, so leaning up cannot repeat it)
//   A              rotate clockwise
//   joystick button  rotate counter-clockwise
//   B              exit to the submenu -- checked first, below, so it can
//                  never also be read as a rotate that tick
//
// Moving soft drop onto the stick is what pays for the second rotation
// direction, which on a 16-tall well matters more than it did on 8: pieces now
// travel far enough that turning the long way round costs real time. B used to
// be that second direction; it moved to the joystick button so B could join
// every other game's exit-to-submenu binding instead.
//
// ---- Why the hard drop has to be armed --------------------------------------
// Up and down are the two most destructive bindings in the game sitting on
// opposite ends of one spring, and a spring-return stick does not stop at
// centre when you let go of it -- it overshoots. An overshoot past JOY_STEP_ON
// is arithmetically indistinguishable from a deliberate up-flick, so releasing
// a soft drop would slam the piece down: the player asks the piece to hurry and
// watches it bury itself instead.
//
// So an up-flick only counts once the stick has genuinely come to rest near
// centre for DROP_ARM_MS. A bounce through the deadzone does not qualify, which
// is the whole point -- the settle time is what separates "let go" from "asked
// for it". Costs nothing in play: the stick is at rest between placements
// anyway, and the delay is over before the next piece has moved.
void Tetris::servicePlaying(uint32_t now){
  if (_sys.buttonB.wasPressed()) {      // exit to the submenu, mid-piece
    _sys.audio.play(SFX_BACK);
    _state = T_MENU;
    return;
  }

  uint32_t dt = now - _lastUpdate;
  _lastUpdate = now;

  const JoyDir d = _sys.joy.dir();

  if (d != DIR_NONE)                            _neutralAt = now;
  else if (now - _neutralAt >= DROP_ARM_MS)     _dropArmed = true;
  if (d == DIR_DOWN)                            _dropArmed = false;

  uint8_t ev = 0;
  _sim.setMove(d == DIR_LEFT ? -1 : d == DIR_RIGHT ? 1 : 0);
  if (_sys.buttonA.wasPressed())   ev |= _sim.rotate(+1);
  if (_sys.joyButton.wasPressed()) ev |= _sim.rotate(-1);
  if (_dropArmed && _sys.joy.stepY() < 0){
    _dropArmed = false;                         // one slam per return to centre
    ev |= _sim.hardDrop();
  }
  _sim.softDrop(_sys.joy.stepY() > 0, d == DIR_DOWN);
  ev |= _sim.update(dt);

  if (ev & TET_EV_GAMEOVER){ enterGameOver(now); return; }   // SFX_LOSE, not a lock thud
  sound(ev);

  if (ev & TET_EV_LEVEL) _levelFlashUntil = now + LEVEL_FLASH_MS;
  if (ev & TET_EV_LINES){ _clearStart = now; _state = T_CLEARING; return; }

  render();
}

// ============================================================================
//  Rendering
// ============================================================================
void Tetris::renderPiece(uint16_t shape, int px, int py, const CRGB& c){
  for (int r = 0; r < 4; r++)
    for (int col = 0; col < 4; col++)
      if (shapeCell(shape, col, r)){
        int by = py + r;
        if (by >= 0) disp().setPixel(px + col, by, c);
      }
}

// The next piece, centered in a 4x4 box. Centering matters because the shape
// table is not: an I lives on one row of its box and an O in one corner, so
// drawing the box raw makes the preview jump around between pieces.
void Tetris::renderNext(int x0, int y0){
  const int t = _sim.nextType();
  const uint16_t s = SHAPES[t][0];

  int minC = 4, maxC = -1, minR = 4, maxR = -1;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (shapeCell(s, c, r)){
        if (c < minC) minC = c;
        if (c > maxC) maxC = c;
        if (r < minR) minR = r;
        if (r > maxR) maxR = r;
      }
  if (maxC < 0) return;

  const int ox = x0 + (4 - (maxC - minC + 1)) / 2 - minC;
  const int oy = y0 + (4 - (maxR - minR + 1)) / 2 - minR;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (shapeCell(s, c, r)) disp().setPixel(ox + c, oy + r, COLORS[t]);
}

// Divider column plus the info panel. Seven columns is exactly two 3x5 digits
// with the one-pixel gap between them, which is what decided the contents: next
// piece, level, and how close the next level is. The score does not fit -- five
// digits is nineteen pixels -- so it stays on the game-over marquee rather than
// becoming a second thing crawling beside the well.
void Tetris::renderHud(){
  if (!hasPanel()) return;

  const int px = panelX();
  for (int r = 0; r < MATRIX_H; r++)
    disp().setPixel(wellW(), r, CRGB(40, 40, 40));

  renderNext(px + (panelW() - 4) / 2, PANEL_NEXT_ROW);

  const uint32_t lv = (uint32_t)_sim.level();
  disp().drawNumber(lv, px + (panelW() - disp().numberWidth(lv)) / 2,
                    PANEL_LEVEL_ROW, CRGB(120, 120, 160));

  const int filled = _sim.linesIntoLevel() * panelW() / TetrisSim::linesPerLevel();
  for (int i = 0; i < panelW(); i++)
    disp().setPixel(px + i, PANEL_BAR_ROW,
                    i < filled ? CRGB(0, 170, 90) : CRGB(12, 12, 12));
}

void Tetris::render(){
  disp().clear();
  for (int r = 0; r < _sim.h(); r++)
    for (int c = 0; c < _sim.w(); c++){
      int8_t v = _sim.cell(c, r);
      if (v >= 0) disp().setPixel(c, r, COLORS[v]);
    }

  uint16_t s = _sim.pieceShape();
  if (_ghostEnabled) renderPiece(s, _sim.pieceX(), _sim.ghostY(), GHOST_COLOR);
  renderPiece(s, _sim.pieceX(), _sim.pieceY(), COLORS[_sim.pieceType()]);

  renderHud();
  if (millis() < _levelFlashUntil) disp().border(CRGB::White);
  disp().show();
}

void Tetris::renderMenu(){
  disp().clear();
  if      (_menuPage == 0) disp().drawBitmap(IMG_TETRIS, ICON_PAL);
  else if (_menuPage == 1) disp().drawBitmap(IMG_GHOST, ICON_PAL);
  else if (_highScore == 0) disp().drawBitmap(IMG_SCORE, ICON_PAL);
  else drawScoreScrollLoop(disp(), _highScore, millis(), CRGB(255, 180, 0));
  disp().show();
}

void Tetris::renderClearing(uint32_t t){
  disp().clear();
  uint8_t v = (t >= CLEAR_ANIM_MS) ? 0 : (uint8_t)(255 - t * 255 / CLEAR_ANIM_MS);
  for (int r = 0; r < _sim.h(); r++){
    if (_sim.clearRow(r)){
      for (int c = 0; c < _sim.w(); c++) disp().setPixel(c, r, CRGB(v, v, v));
    } else {
      for (int c = 0; c < _sim.w(); c++){
        int8_t cv = _sim.cell(c, r);
        if (cv >= 0) disp().setPixel(c, r, COLORS[cv]);
      }
    }
  }
  renderHud();       // the panel does not blink out during a clear
  disp().show();
}
