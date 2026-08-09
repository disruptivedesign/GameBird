#pragma once
#include <Arduino.h>

// ============================================================================
//  Wire protocol shared by every layer of the wireless backend. Pure data +
//  constants, no logic. Transport-agnostic: nothing here mentions ESP-NOW, so a
//  different link (BLE, ...) could carry the same frames.
//
//  Frame layout on the wire:  [ NetHeader | payload ]
//  The payload is opaque to the backend for INPUT/STATE (the game owns it) and
//  a fixed struct for control messages (lobby / join / start).
// ============================================================================

#define NET_MAGIC        0x4247    // 'G','B' -- fences off stray / other-project traffic
// v4: Tron's input byte 0 became an absolute heading instead of a relative turn.
// Same two bytes, so a v3 unit would decode it happily and steer itself into a
// wall -- a size change fails loudly, a meaning change does not, which is the
// case this version byte exists for.
// v5: lockstep games. MSG_PRIVSTATE added, INPUT grew to carry a whole decision
// list, and NetHeader.tick became meaningful on INPUT (the tick the sender
// computed against). A v4 unit would read a v5 INPUT as a two-byte heading and
// act on garbage -- exactly the silent misread v4 was minted for.
// v6: the lobby beacon carries the whole ROSTER rather than just the host's
// colour, and JOIN carries a game-defined tag beside the colour. Before this a
// client could not see who else was in the lobby at all -- not even their
// colours -- so a co-op game had no way to show four people what loadouts they
// were about to take in. Both payloads changed size, so a mixed fleet fails on
// the version byte instead of memcpy-ing a 7-byte struct out of a 16-byte one.
#define NET_PROTO_VER    6
#define NET_CHANNEL      1         // fixed 2.4 GHz channel; all units must agree
#define NET_MAX_PLAYERS  5         // per match (host + up to 4 clients)
#define NET_MAX_PAYLOAD  250       // ESP-NOW v1 hard cap
// Max serialized per-tick input blob. Two bytes is all a joystick game needs,
// but a LOCKSTEP game sends a decision per owned cell instead of a control
// reading. The worst case is one player owning every cell of a full panel --
// 256, not the 224 of Virus's HUD-shortened arena, because the buffers are
// sized for the panel and the wire format is tested at 16x16 -- at five bits
// each, so 160 bytes. Frame is 10-byte header + 160 = 170 of the 250 ESP-NOW
// allows. virus.cpp static_asserts the derivation; it caught this being 144.
#define NET_INPUT_MAX    160
// Max per-player private state (MSG_PRIVSTATE). Virus needs 4 bits of banked
// energy per owned cell, plus one byte of what that player's virus just did --
// the events a client cannot recover by comparing one board against the next,
// which is what makes a networked match audible.
//
// Sized on the 256-cell panel rather than Virus's 224-cell arena, matching
// NET_INPUT_MAX above and the 16x16 the wire format is tested at: 128 + 1 =
// 129, rounded up. It was exactly 128 before this byte existed, so the worst
// case fit with nothing to spare and the addition overflowed it -- silently,
// since serializePrivate answers a frame it cannot fill with 0 and every cell
// then reads no energy and idles. test_worst_case_lists_fit_their_frames is
// what caught it.
#define NET_PRIV_MAX     136
// Largest control payload (lobby / join / joinack / start / scores). sendCtrl
// builds its frame on the stack against this, and net.cpp static_asserts every
// payload struct against it -- so adding a field, or a sixth player slot to
// StartPayload's colour table, fails the build instead of quietly running off
// the end of the buffer. Largest today is StartPayload at 23 bytes.
#define NET_CTRL_MAX     32
// Max serialized state snapshot. The frame is [NetHeader | payload] and the
// header is 10 bytes, so 240 is the most a single ESP-NOW frame can carry.
// Largest snapshot today is Virus at 16x16: 197 bytes.
#define NET_STATE_MAX    240

#define NET_PID_NONE     0xFF      // "no player id assigned yet"

// Message types (the `type` byte in NetHeader).
enum NetMsgType : uint8_t {
  MSG_LOBBY   = 1,   // host -> all : periodic "open lobby" announce (discovery)
  MSG_JOIN,          // client -> host : request to join
  MSG_JOINACK,       // host -> client : accepted, here is your playerId
  MSG_LEAVE,         // either -> : leaving the session
  MSG_START,         // host -> all : begin the match
  MSG_INPUT,         // client -> host : per-tick input (opaque game blob)
  MSG_STATE,         // host -> all : per-tick snapshot (opaque game blob)
  MSG_SCORES,        // host -> all : running scoreboard (tags + win counts)
  MSG_PRIVSTATE,     // host -> ONE client : that player's private state (opaque)
};

#pragma pack(push, 1)

// Fixed 10-byte header on every frame.
struct NetHeader {
  uint16_t magic;      // NET_MAGIC
  uint8_t  ver;        // NET_PROTO_VER
  uint8_t  type;       // NetMsgType
  uint8_t  gameId;     // which NetGame this frame concerns (0 = lobby-only)
  uint16_t session;    // per-match id, minted by the host
  uint8_t  sender;     // playerId, or NET_PID_NONE before assignment
  // STATE / PRIVSTATE: the host sim tick this frame describes.
  // INPUT: the tick the sender computed against -- which for a lockstep game is
  // load-bearing, not informational. Its decision list is positionally indexed
  // by the sender's owned cells in scan order, so a list applied to a board it
  // was not computed against is not stale, it is MISALIGNED: entry 7 would land
  // on a different cell. The host matches this tag before applying, and treats
  // any mismatch as "no decisions" (see virus-multiplayer-plan.md §1).
  // 0 otherwise.
  uint16_t tick;
};

// MSG_LOBBY: broadcast by a host so browsers can discover and join.
struct LobbyPayload {
  uint8_t gameId;
  // Self-describing, so a browser can tell whether it is able to play here.
  // Nothing in Net acts on it -- Net has no idea what board the game wants --
  // so the comparison lives in Multiplayer::arenaMatches, which refuses the join
  // and paints the lobby red. Said "fails loudly" here for a long time while
  // nothing actually compared them; it does now, but at the runner, not here.
  uint8_t arenaW, arenaH;
  uint8_t playerCount;
  uint8_t maxPlayers;
  uint8_t started;          // 0 = open to join, 1 = match in progress
  // Which playerIds are actually occupied, one bit each. playerCount is a
  // COUNT and the roster can have holes -- the player in slot 1 leaving while
  // slot 2 stays puts two players in slots {0,2} -- so anything drawing a
  // roster has to iterate this rather than 0..playerCount-1, or it paints an
  // empty slot and misses a real one.
  uint8_t slots;
  // The roster, indexed by playerId, so a CLIENT can draw the same lobby the
  // host does. It rides the beacon rather than a message of its own because the
  // beacon already goes out 4 Hz and already carries the player count -- the
  // roster is the answer to "who ARE those players", and splitting the two
  // across separate frames would only invent a way for them to disagree.
  // Slot 0 is the host, so colorIndex[0] is also the browse-list swatch.
  uint8_t colorIndex[NET_MAX_PLAYERS];
  // One byte per player that the backend carries and never interprets -- the
  // same opacity INPUT and STATE already have, applied to the lobby. The game
  // decides what it means; Swarm puts each player's chosen weapon here.
  uint8_t tag[NET_MAX_PLAYERS];
};

// MSG_JOIN: client asks a host to let it in (carries its MAC for back-addressing).
struct JoinPayload {
  uint8_t mac[6];
  uint8_t colorIndex;       // joiner's chosen color
  uint8_t tag;              // joiner's game-defined lobby tag (opaque here)
};

// MSG_SCORES: running standings so every unit shows the same scoreboard. Colors
// come from the resolved palette every unit already has from MSG_START.
struct ScoresPayload {
  uint8_t numPlayers;
  uint8_t score[NET_MAX_PLAYERS];
};

// MSG_JOINACK: host confirms membership and hands out identity + arena size.
struct JoinAckPayload {
  uint8_t playerId;
  uint8_t gameId;
  uint8_t arenaW, arenaH;
};

// MSG_START: host kicks off the match. Spawn slots are derived from playerId +
// seed on every device, so they are not sent explicitly. Carries the host-
// resolved player palette (r,g,b per player) so every screen renders alike.
struct StartPayload {
  uint8_t  gameId;
  uint8_t  arenaW, arenaH;
  uint8_t  numPlayers;
  uint32_t seed;
  uint8_t  colors[NET_MAX_PLAYERS * 3];   // resolved RGB per playerId

  // Matches in this series, as the HOST chose it. Both roles count matches
  // themselves and both decide independently when the series is over, so they
  // have to be counting to the same number -- and only the host has the screen
  // that picked it. 0 means "the game's own default", which is every game that
  // does not offer the choice.
  uint8_t  rounds;
};

#pragma pack(pop)

// Broadcast address for discovery / state fan-out.
static const uint8_t NET_BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
