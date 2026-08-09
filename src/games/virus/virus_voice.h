#pragma once
#include "audio/sound.h"

// ============================================================================
//  A virus's voice -- the second thing an author writes, beside their rule.
//
//  WHY YOU DO NOT PLAY SOUNDS FROM decide()
//
//  Your rule is PURE (see virus_api.h): it may not remember anything, and it
//  may not do anything except return a Decision. That is not a style rule --
//  the referee replays rules over a snapshot to keep the match deterministic,
//  and virus_rules.cpp is compiled off-hardware by the native test suite, where
//  there is no buzzer at all. A decide() that made noise would break the match
//  and the tests together.
//
//  So you do not play sounds. You DECLARE them, here, and the referee plays
//  them for you off events it already tracks. Your rule stays pure and you
//  still get a virus that sounds like yours.
//
//  Any field may be nullptr, which means "silent for that event".
//
//  There is ONE piezo, so at most one sound plays per tick across the whole
//  match (see virus_app.cpp). Loud, frequent events will drown quiet rare ones,
//  which is why onGrow -- the event that can fire dozens of times a tick -- is
//  the one to leave nullptr or keep to a 15 ms tick.
// ============================================================================

struct VirusVoice {
  // ---- per-tick events, played at most once each per tick ----
  const Sfx* onGrow;       // you claimed at least one new tile
  const Sfx* onAttack;     // you landed damage on an enemy
  const Sfx* onDamaged;    // one of your cells was hit but survived
  const Sfx* onCellLost;   // one of your cells was killed
  const Sfx* onMoved;      // one of your cells walked into a neighbouring tile
  const Sfx* onSpore;      // you leapt a blockade

  // ---- match moments ----
  const Sfx* signature;    // once, as the match begins -- your calling card
  const Sfx* victory;      // once, if you win
};

// Referee lookup, indexed exactly like VIRUS_RULES: [0] = virus A .. [3] = D.
// A null entry means that virus plays silently.
extern const VirusVoice* VIRUS_VOICES[4];
