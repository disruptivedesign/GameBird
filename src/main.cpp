#include <Arduino.h>
#include "core/defines.h"
#include "core/system.h"
#include "ui/splash.h"
#include "ui/menu.h"
#include "games/tetris/tetris.h"
#include "games/breakout/breakout.h"
#include "games/flappy/flappy.h"
#include "games/tron/tron.h"
#include "match/multiplayer.h"
#include "match/single_player.h"
#include "games/tron/tron_app.h"
#include "games/swarm/swarm.h"
#include "games/swarm/swarm_app.h"
#include "games/virus/virus.h"
#include "games/virus/virus_app.h"
#include "games/virus/virus_menu.h"
#include "ui/settings.h"
#include "audio/sfx.h"

// ============================================================================
//  Entry point: boot splash -> top-level game menu -> run the chosen game.
//  Games return GAME_EXIT to hand control back to the main menu, and the shell
//  can force the same exit at any time (see the quit gesture below). Add a game
//  by editing the MENU registry below.
// ============================================================================

// ---- Universal quit gesture -------------------------------------------------
// Hold A and B together to leave any game.
//
// It has to be both buttons: games own their inputs during play and the shell
// cannot claim either one alone -- Tetris soft-drops on a held B, and any game
// that wanted a held A would collide the same way. Nothing uses the pair.
//
// The border brightens as the hold charges, which is what makes the gesture
// safe rather than just long: an accidental hold announces itself before it
// fires instead of after, and a deliberate one has visible progress. Releasing
// early cancels with no side effect.
#define QUIT_HOLD_MS  1000
#define QUIT_HINT_MS   250   // charging border appears once past this
System sys;
Tetris       tetris(sys);
Breakout     breakout(sys);     // single-player; all its rules live in BreakoutSim
Flappy       flappy(sys);
Tron         tron;              // a NetGame, run by both controllers below
Multiplayer  multi(sys, &tron);
SinglePlayer solo(sys, &tron);  // local match vs AI (shares the one Tron)
TronApp      tronApp(sys, tron, multi, solo);   // Tron submenu (Single / Multiplayer)
// Co-op: everyone against the swarm. Takes the piezo directly because a
// NetGame gets no System and Swarm has its own things to say -- a kill, a
// charged laser, the boss arriving -- that no runner could know to play.
Swarm        swarm(&sys.audio);
Multiplayer  swarmMulti(sys, &swarm);
SinglePlayer swarmSolo(sys, &swarm);            // you plus one AI wingman
SwarmApp     swarmApp(sys, swarm, swarmMulti, swarmSolo);
Virus        virusGame;                         // programmable cellular game (NetGame)
VirusApp     virusApp(sys, virusGame);          // single-device: your four, against each other
// A second Multiplayer, bound to Virus. One runner per networked game rather
// than one shared: a runner is built around the game it drives, and only one
// can be on the radio at a time anyway -- the menu sees to that, and both call
// net().leave() on entry, so switching between them starts from a clean session.
Multiplayer  virusMulti(sys, &virusGame);
VirusMenu    virusMenu(sys, virusGame, virusApp, virusMulti);
Settings     settings(sys);

// ---- Game registry: games only; each shows its own submenu if it has one. ---
static MenuEntry MENU[] = {
  { &tetris,   {} },
  { &breakout, {} },
  { &flappy, {} },
  { &tronApp,  {} },
  { &swarmApp, {} },
  { &virusMenu, {} },
  { &settings, {} },
};
static const int MENU_COUNT = sizeof(MENU) / sizeof(MENU[0]);
Menu menu(sys, MENU, MENU_COUNT);

// Top-level flow: menu -> game -> back to the menu, either because the game
// asked (GAME_EXIT) or because the player held A+B.
enum SysState { SYS_MENU, SYS_GAME };
static SysState sysState = SYS_MENU;
static Game*    current  = nullptr;
static uint32_t quitHeldSince = 0;   // 0 = not charging

// Returns true on the tick the hold completes. Resets the moment either button
// comes up, so this is edge-free and safe to call every frame.
static bool quitGestureFired(uint32_t now){
  if (!(sys.buttonA.isHeld() && sys.buttonB.isHeld())){ quitHeldSince = 0; return false; }
  if (!quitHeldSince){ quitHeldSince = now; return false; }
  if (now - quitHeldSince < QUIT_HOLD_MS) return false;
  quitHeldSince = 0;
  return true;
}

// Drawn over whatever the game just rendered, so it needs its own show().
static void drawQuitCharge(uint32_t now){
  uint32_t held = now - quitHeldSince;
  if (held < QUIT_HINT_MS) return;
  uint8_t v = (uint8_t)(held * 255 / QUIT_HOLD_MS);
  sys.display.border(CRGB(v, v, v));
  sys.display.show();
}

static void leaveGame(){
  if (current) current->end();       // release anything the game was holding
  sysState = SYS_MENU;
  menu.begin();
}

// ---- CALIBRATION 3: input test ---------------------------------------------
// Lights the EDGE the stick reports, so polarity answers itself: push up, and
// if the bottom edge lights, JOY_INVERT_Y is wrong. The white dot tracks the
// proportional reading so the deadzone and centre calibration are visible too --
// it should sit dead centre with nothing touching the stick.
//
// Worth having as a mode rather than as reasoning: an inverted axis does not
// present as an input fault. It presents as whichever game binds the two
// directions to different actions behaving bizarrely -- Tetris slamming a piece
// when asked to accelerate it, in the case this was written for.
static void inputTest(){
  Display& d = sys.display;
  d.clear();

  switch (sys.joy.dir()){
    case DIR_UP:    for (int c = 0; c < MATRIX_W; c++) d.setPixel(c, 0,            CRGB(0, 160, 0));   break;
    case DIR_DOWN:  for (int c = 0; c < MATRIX_W; c++) d.setPixel(c, MATRIX_H - 1, CRGB(160, 70, 0));  break;
    case DIR_LEFT:  for (int r = 0; r < MATRIX_H; r++) d.setPixel(0,            r, CRGB(0, 60, 160));  break;
    case DIR_RIGHT: for (int r = 0; r < MATRIX_H; r++) d.setPixel(MATRIX_W - 1, r, CRGB(160, 0, 120)); break;
    default: break;
  }

  // y() is stick space (+ = up) and rows grow down, hence the subtraction.
  const int cx = MATRIX_W / 2, cy = MATRIX_H / 2;
  d.setPixel(cx, cy, CRGB(20, 20, 20));
  d.setPixel(cx + sys.joy.x() * (MATRIX_W / 2 - 1) / 100,
             cy - sys.joy.y() * (MATRIX_H / 2 - 1) / 100, CRGB(200, 200, 200));

  if (sys.buttonA.isHeld()) d.setPixel(0,            MATRIX_H - 1, CRGB(0, 200, 0));
  if (sys.buttonB.isHeld()) d.setPixel(MATRIX_W - 1, MATRIX_H - 1, CRGB(200, 0, 0));
  d.show();
}

void setup(){
  Serial.begin(115200);
  sys.begin();
  Splash(sys.display, sys.audio).play();
  menu.begin();
}

void loop(){
  sys.update();                    // poll inputs once per tick

  if (CALIBRATION){
    if      (CALIBRATION >= 3) inputTest();                 // check the stick
    else if (CALIBRATION == 2) sys.display.calibrateChain();// discover the wiring
    else                       sys.display.calibrate();     // confirm it
    delay(CALIBRATION >= 3 ? 5 : 200);   // the input test has to feel live
    return;
  }

  if (sysState == SYS_MENU){
    Game* sel = menu.service();
    if (sel){ current = sel; current->begin(); sysState = SYS_GAME; quitHeldSince = 0; }
  } else {
    uint32_t now = millis();
    GameStatus st = current->service();
    if (quitGestureFired(now)){ sys.audio.play(SFX_BACK); leaveGame(); }
    else if (st == GAME_EXIT)  { leaveGame(); }
    else if (quitHeldSince)    { drawQuitCharge(now); }
  }

  delay(5);
}
