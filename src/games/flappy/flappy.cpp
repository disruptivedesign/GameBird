#include "flappy.h"
#include <esp_system.h> // esp_random()

#include "audio/sfx.h"
#include "audio/notes.h"
#include "ui/scoreboard.h"

#define NUM_MENU_PAGES 2 // 0 = normal, 1 = high score

#define PLAYER_X 32
#define PLAYER_START_HEIGHT 128
#define PLAYER_JUMP_SPEED 10
#define GRAVITY_AMOUNT 1
#define NUM_OBSTACLES_PER_LEVEL 7

#define OBSTACLE_START 255

// ---- world space ------------------------------------------------------------
// Flight happens in a fixed 256x256 world that is sampled down onto whatever
// panel is fitted, so the physics -- jump height, gravity, gap size, scroll
// speed -- are identical at 8x8 and at 16x16 and only the sampling changes.
// These two are the bridge: how many world units one panel column and one panel
// row are worth.
static constexpr int COL_UNITS = 256 / MATRIX_W;
static constexpr int ROW_UNITS = 256 / MATRIX_H;

// Half the collision band, in world units. DERIVED from the panel, because it
// has to agree with what is drawn: an obstacle is rendered one column wide, so
// the band either side of it must add up to one column. As a hardcoded 16 this
// happened to be right on the 8-wide panel and was twice the drawn width on the
// 16-wide one -- the bird died a full column before touching anything.
static constexpr int OBSTACLE_HALF_W = COL_UNITS / 2;

#define NS_FLAPPY "flappy" // storage namespace
#define KEY_HIGH "hi"      // high-score key


static const uint8_t FLAPPY_ICON[8][8] = {
  {0,0,0,0,0,0,0,0},
  {0,0,1,1,1,1,0,0},
  {0,1,1,1,1,0,1,0},
  {2,2,2,1,1,1,1,1},
  {2,2,2,1,1,3,3,3},
  {0,1,1,1,1,3,3,3},
  {0,0,0,1,1,1,0,0},
  {0,0,0,0,0,0,0,0},
};

static const uint8_t FLAPPY_ICON_GAME[8][8] = {
  {4,4,4,4,4,4,4,4},
  {4,4,1,1,1,1,4,4},
  {4,1,1,1,1,0,1,4},
  {2,2,2,1,1,1,1,1},
  {2,2,2,1,1,3,3,3},
  {4,1,1,1,1,3,3,3},
  {4,4,4,1,1,1,4,4},
  {4,4,4,4,4,4,4,4},
};

static const CRGB FLAPPY_ICON_PAL[] = {
  CRGB::Black, CRGB(255, 255, 0), CRGB(255, 255, 128), CRGB(255, 128, 0), CRGB(0, 128, 0)
};


// ============================================================================
//  Lifecycle
// ============================================================================
void Flappy::begin() {
  _highScore = _sys.storage.getU32(NS_FLAPPY, KEY_HIGH, 0);
  _state = T_MENU; // the boot splash already ran at system startup
  _menuPage = 0;
}

Icon Flappy::menuIcon() const { return { FLAPPY_ICON, FLAPPY_ICON_PAL }; }

GameStatus Flappy::service() {
  uint32_t now = millis();
  switch (_state) {
  case T_MENU:
    return updateMenu(now);
  case T_PLAYING:
    servicePlaying(now);
    return GAME_CONTINUE;
  case T_NEXT_LEVEL:
    updateNextLevel(now);
    return GAME_CONTINUE;
  case T_GAMEOVER:
    updateGameOver(now);
    return GAME_CONTINUE;
  }
  return GAME_CONTINUE;
}

bool Flappy::isColliding() {
  for (const auto &obstacle : _obstacles) {
    if (!obstacle.active) {
      continue;
    }

    if (std::abs(obstacle.x - PLAYER_X) < OBSTACLE_HALF_W) {
      if (_player.height < obstacle.min) {
        return true;
      }

      if (_player.height > obstacle.max) {
        return true;
      }
    }
  }

  return false;
}

void Flappy::updateObstacles() {
  for (auto &obstacle : _obstacles) {
    if (obstacle.active) {
      obstacle.x--;

      if (obstacle.x == 0) {
        obstacle.active = false;
        _score += _level;
      }
    }
  }
}

bool Flappy::tryAddingNewObstacle() {
  if (!_remainingObstacles) {
    return false;
  }

  for (auto &obstacle : _obstacles) {
    if (!obstacle.active) {
      obstacle.x = OBSTACLE_START;
      const uint32_t seed = esp_random();
      const uint8_t buffer = getObstacleWindowSize() / 2;
      uint8_t minTarget = 0 + buffer;
      uint8_t maxTarget = 255 - buffer;
      uint32_t scaled =
          (seed / (UINT32_MAX / (maxTarget - minTarget))) +
          minTarget;

      obstacle.min = scaled - buffer;
      obstacle.max = scaled + buffer;
      obstacle.active = true;

      return true;
    }
  }
  return false;
}

uint8_t Flappy::numActiveObstacles() {
  uint8_t result = 0;
  for (auto &obstacle : _obstacles) {
    if (obstacle.active) {
      result++;
    }
  }
  return result;
}

uint8_t Flappy::getObstacleWindowSize() {
  return std::max(static_cast<uint8_t>(160 - 3 * _level), static_cast<uint8_t>(32));
}

uint32_t Flappy::getObstacleRefreshPeriod() {
  return std::max(static_cast<int32_t>(25 - 2 * _level), static_cast<int32_t>(5));
}

uint32_t Flappy::getObstacleSpawnPeriod() {
  return std::max(static_cast<int32_t>(2500 - 130 * _level), static_cast<int32_t>(500));
}

void Flappy::checkPlayerInputs() {
  // Button A flaps, and so does a flick up, since the rev 2 board has a stick
  // and that is the gesture it asks for (Breakout serves the same way). stepY
  // is an edge, so leaning on it cannot hold the bird aloft. Button B is not a
  // flap button -- it exits to the submenu, handled in servicePlaying().
  if (_sys.buttonA.wasPressed() || _sys.joy.stepY() < 0) {
    _player.speed = PLAYER_JUMP_SPEED;
    _sys.audio.play(SFX_SELECT);
  }
}

void Flappy::updatePlayerPosition() {
  _player.height += _player.speed;
  _player.speed -= GRAVITY_AMOUNT;

  if ((_player.height > 255) || (_player.height < 0)) {
    _gameOver = true;
  }
}

void Flappy::resetLevel() {
  for (auto &obstacle : _obstacles) {
    obstacle.active = false;
  }

  _player.height = PLAYER_START_HEIGHT;
  _player.speed = PLAYER_JUMP_SPEED;

  _obstacleTime = 0;
  _obstacleSpawnTime = 0;
  _playerTime = 0;

  _remainingObstacles = NUM_OBSTACLES_PER_LEVEL;
}

void Flappy::resetGame() {
  resetLevel();

  _score = 0;
  _level = 1;
  _gameOver = false;
}

// ============================================================================
//  States
// ============================================================================
GameStatus Flappy::updateMenu(uint32_t now) {
  (void)now;
  // The rev 2 board replaced the slider with a joystick, so paging is a step
  // per flick and wraps -- which an absolute slider could not do. Same shape as
  // Menu, Tetris, Breakout and VirusApp.
  if (int8_t s = _sys.joy.stepX()) {
    _menuPage = (_menuPage + s + NUM_MENU_PAGES) % NUM_MENU_PAGES;
    _sys.audio.play(SFX_MOVE);
  }

  if (_sys.buttonB.wasPressed()) {
    _sys.audio.play(SFX_BACK);
    return GAME_EXIT;
  }

  if (_sys.buttonA.wasPressed() && _menuPage == 0) {
    _sys.audio.play(SFX_SELECT);
    resetGame();
    _state = T_PLAYING;
    return GAME_CONTINUE;
  }

  renderMenu();
  return GAME_CONTINUE;
}

void Flappy::updateGameOver(uint32_t now) {
  uint32_t t = now - _gameOverStart;
  const uint32_t FLASH_MS = 700;
  bool skip = _sys.buttonA.wasPressed() || _sys.buttonB.wasPressed();

  if (t < FLASH_MS) {
    if ((t / 150) & 1)
      disp().clear();
    else
      disp().fill(CRGB::Red);
    disp().show();
    return;
  }

  // Shared marquee: double-size digits, vertically centred, at the one scroll
  // rate every game uses. Flappy's own copy predated it and still drew the 3x5
  // font at 1x pinned to row 1, which on 16 rows is a smudge in the corner.
  disp().clear();
  bool done = drawScoreScroll(disp(), _score, t - FLASH_MS,
                              _newHigh ? CRGB(255, 180, 0) : CRGB(0, 150, 255));
  disp().show();

  if (done || skip) {
    resetGame();
    _state = T_MENU;
  }
}

void Flappy::servicePlaying(uint32_t now) {
  if (_sys.buttonB.wasPressed()) {      // exit to the submenu, mid-flight
    _sys.audio.play(SFX_BACK);
    resetGame();
    _state = T_MENU;
    return;
  }

  if (_gameOver){
    _newHigh = (_score > _highScore);
    if (_newHigh) {
      _highScore = _score;
      _sys.storage.putU32(NS_FLAPPY, KEY_HIGH, _highScore);
    }
    _sys.audio.play(SFX_LOSE);
    _gameOverStart = now;
    _state = T_GAMEOVER;
    return;
  }

  if (now - _obstacleTime >= getObstacleRefreshPeriod()) {
    updateObstacles();
    _obstacleTime = now;
  }

  if (now - _obstacleSpawnTime >= getObstacleSpawnPeriod()) {
    if (tryAddingNewObstacle()) {
      _obstacleSpawnTime = now;
      _remainingObstacles--;
    }
  }

  checkPlayerInputs();
  if (now - _playerTime >= 50) {
    updatePlayerPosition();
    _playerTime = now;
  }

  if (isColliding()) {
    _gameOver = true;
  }

  if (numActiveObstacles() == 0 && _remainingObstacles == 0) {
    _level++;
    _nextLevelStart = now;
    _state = T_NEXT_LEVEL;
    _sys.audio.play(SFX_WIN);
  }

  renderPlaying();
}

void Flappy::updateNextLevel(uint32_t now) {
  uint32_t t = now - _nextLevelStart;
  const uint32_t FLASH_MS = 400;

  if (t < FLASH_MS) {
    if ((t / 80) & 1)
      disp().clear();
    else
      disp().fill(CRGB::Green);
    disp().show();
    return;
  }

  disp().clear();
  bool done = drawScoreScroll(disp(), _level, t - FLASH_MS, CRGB(100, 180, 255));
  disp().show();

  if (done) {
    resetLevel();
    _state = T_PLAYING;
  }
}

// ============================================================================
//  Rendering
// ============================================================================
void Flappy::renderPlaying() {
  disp().clear();

  for (const auto &obstacle : _obstacles) {
    if (!obstacle.active) {
      continue;
    }

    uint8_t startCol = obstacle.x / COL_UNITS;
    uint8_t frac = (obstacle.x % COL_UNITS) * MATRIX_W;
    const auto lhs = CRGB(0, 255, 0).nscale8(255-frac);
    const auto rhs = CRGB(0, 255, 0).nscale8(frac);

    for (size_t row = 0; row < MATRIX_H; row++) {
      const bool isTopObstacle = (obstacle.max / ROW_UNITS) < row;
      const bool isBottomObstacle = (obstacle.min / ROW_UNITS) > row;

      if (isTopObstacle || isBottomObstacle) {
        disp().setPixel(startCol, (MATRIX_H-row - 1), lhs);
        disp().setPixel(startCol+1, (MATRIX_H-row - 1), rhs);
      }
    }
  }

  uint8_t startRow = _player.height / ROW_UNITS;
  uint8_t frac = (_player.height % ROW_UNITS) * MATRIX_H;
  const auto lhs = CRGB(255, 200, 0).nscale8(255-frac);
  const auto rhs = CRGB(255, 200, 0).nscale8(frac);
  disp().setPixel(PLAYER_X / COL_UNITS, MATRIX_H-(startRow) - 1, lhs);
  disp().setPixel(PLAYER_X / COL_UNITS, MATRIX_H-(startRow + 1) - 1, rhs);
  disp().show();
}

void Flappy::renderMenu() {
  disp().clear();
  if (_menuPage == 0) {
    disp().drawBitmap(FLAPPY_ICON_GAME, FLAPPY_ICON_PAL);
  } else {
    drawScoreScrollLoop(disp(), _highScore, millis(), CRGB(255, 180, 0));
  }
  disp().show();
}
