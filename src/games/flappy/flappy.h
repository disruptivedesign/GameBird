#pragma once
#include "core/game.h"
#include "core/system.h"

#include <array>

class Flappy : public Game
{
public:
  Flappy(System &sys) : _sys(sys) {}
  void begin() override;
  GameStatus service() override;
  Icon menuIcon() const override;

private:
  System &_sys;
  Display &disp() { return _sys.display; }

  enum TState
  {
    T_MENU,
    T_PLAYING,
    T_NEXT_LEVEL,
    T_GAMEOVER
  };
  TState _state = T_MENU;

  static constexpr size_t MAX_OBSTACLES = 4; 

  struct Obstacle {
    uint8_t min;
    uint8_t max;
    uint8_t x;
    bool active;
  };

  struct Player {
    int32_t height;
    int8_t speed;
  };

  std::array<Obstacle, MAX_OBSTACLES> _obstacles;
  Player _player;

  // Menu / mode
  int _menuPage = 0;

  // Scoring / levels
  bool _gameOver = false;
  bool _newHigh = false;

  uint32_t _remainingObstacles = 0;
  uint32_t _score = 0;
  uint32_t _highScore = 0;
  uint32_t _level = 0;

  // timers
  uint32_t _nextLevelStart = 0;
  uint32_t _gameOverStart = 0;
  uint32_t _obstacleTime = 0;
  uint32_t _obstacleSpawnTime = 0;
  uint32_t _playerTime = 0;


  bool isColliding();
  void updateObstacles();
  bool tryAddingNewObstacle();
  uint8_t numActiveObstacles();
  uint8_t getObstacleWindowSize();
  uint32_t getObstacleRefreshPeriod();
  uint32_t getObstacleSpawnPeriod();
  void updatePlayerPosition();
  void checkPlayerInputs();

  void onGameOver();

  void resetLevel();
  void resetGame();

  // States
  GameStatus updateMenu(uint32_t now);
  void servicePlaying(uint32_t now);
  void updateNextLevel(uint32_t now);
  void updateGameOver(uint32_t now);

  // Rendering
  void renderMenu();
  void renderPlaying();
};

