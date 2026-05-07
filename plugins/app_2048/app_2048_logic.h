#pragma once

#include <array>
#include <cstdint>
#include <random>

// Pure 2048 board logic, independent of GKC and the network layer.
// All state is owned here; the GKC plugin only consults this object during
// DoDraw / DoKeyboard.  Tests link this header without pulling GKC in.
namespace app_2048 {

// Fixed 4x4 board for the MVP.
constexpr int kBoardSize  = 4;
constexpr int kCellCount  = kBoardSize * kBoardSize;

enum class Direction : uint8_t {
    Left,
    Right,
    Up,
    Down,
};

enum class GameState : uint8_t {
    Playing,
    GameOver,
};

// Result of a single move.  `changed` is true iff the board layout changed,
// in which case the caller (Board::move) has already spawned a new tile.
// `merged` is true iff at least one merge happened in the move.  `score_delta`
// is the sum of merged tile values produced by the move.
struct MoveResult {
    bool     changed     = false;
    bool     merged      = false;
    uint32_t score_delta = 0;
};

// Board is the authoritative game state.  It seeds an internal mt19937 from
// a caller-provided value so tests can reproduce specific sequences while
// the runtime can seed from std::random_device.
class Board {
public:
    // Build an empty board with the given seed.  The constructor does NOT
    // spawn starting tiles — call reset() to begin a real game.  This split
    // lets tests inject explicit layouts via set_for_test() without first
    // generating, and then having to ignore, two random tiles.
    explicit Board(uint64_t seed = 0);

    // Reseed and begin a fresh game: clear tiles, zero the score, set state
    // to Playing, then spawn two starting tiles.  F2 in the plugin maps to
    // this regardless of current state.
    void reset(uint64_t seed);

    // Tile value at (x, y).  0 means empty.  x is column (0..3, left→right),
    // y is row (0..3, top→bottom).
    uint32_t at(int x, int y) const;

    // Whether the tile at (x, y) was produced by a merge during the most
    // recent move().  Cleared at the start of the next move().  The renderer
    // reads this to apply the merge highlight palette for one frame.
    bool merged_this_turn(int x, int y) const;

    uint32_t  score() const { return score_; }
    GameState state() const { return state_; }

    // Apply a move in the given direction.  If the move changes the board,
    // a new tile is spawned and game-over is re-evaluated.  Calling move()
    // when state() == GameOver is a no-op (returns all-zero MoveResult);
    // the plugin must reset() to leave game-over.
    MoveResult move(Direction d);

    // Test-only: install a deterministic board snapshot and rng seed.  Score
    // is left unchanged.  Game state is recomputed from the new layout so
    // tests don't have to track Playing/GameOver separately.
    void set_for_test(const std::array<uint32_t, kCellCount>& tiles,
                      uint64_t seed);

    // Test-only: read the count of empty cells.  Useful for asserting that
    // no tile was spawned when a move did not change the board.
    int empty_cell_count() const;

private:
    // Compress a single 4-cell line toward index 0 ("left").  Sets the
    // matching entries of out_merged to true for cells that are merge
    // results.  Returns the score gained by this line's merges.
    static uint32_t compress_line(std::array<uint32_t, kBoardSize>& line,
                                  std::array<bool, kBoardSize>& out_merged);

    // Spawn a new tile (90% chance "2", 10% chance "4") in a random empty
    // cell.  Caller must check that an empty cell exists.
    void spawn_random_tile();

    // True iff at least one move would change the board.
    bool any_move_possible() const;

    int index_of(int x, int y) const { return y * kBoardSize + x; }

    std::array<uint32_t, kCellCount> tiles_{};
    std::array<bool,     kCellCount> merged_{};
    uint32_t  score_ = 0;
    GameState state_ = GameState::Playing;
    std::mt19937_64                  rng_;
};

}  // namespace app_2048
