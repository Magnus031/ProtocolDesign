#include "plugins/app_2048/app_2048_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using app_2048::Board;
using app_2048::Direction;
using app_2048::GameState;
using app_2048::kBoardSize;
using app_2048::kCellCount;
using app_2048::MoveResult;

// Helper: build a tile array from a row-major initializer list.
std::array<uint32_t, kCellCount> make_tiles(std::initializer_list<uint32_t> v) {
    std::array<uint32_t, kCellCount> a{};
    int i = 0;
    for (uint32_t x : v) {
        if (i < kCellCount) a[i++] = x;
    }
    return a;
}

// Helper: snapshot the board into a flat row-major array.
std::array<uint32_t, kCellCount> snapshot(const Board& b) {
    std::array<uint32_t, kCellCount> out{};
    for (int y = 0; y < kBoardSize; ++y)
        for (int x = 0; x < kBoardSize; ++x)
            out[y * kBoardSize + x] = b.at(x, y);
    return out;
}

// Helper: zero out tiles that the post-move spawn might have placed in any
// pre-existing empty cell.  We accept exactly one new 2 or 4 in cells that
// were empty BOTH before and after the in-place compression.  The expected
// layout passed in must be the post-compression layout WITHOUT the spawn.
void erase_spawn_diff(std::array<uint32_t, kCellCount>& observed,
                      const std::array<uint32_t, kCellCount>& pre_move,
                      const std::array<uint32_t, kCellCount>& expected_after) {
    int spawn_count = 0;
    for (int i = 0; i < kCellCount; ++i) {
        if (pre_move[i] == 0 && expected_after[i] == 0 && observed[i] != 0) {
            EXPECT_TRUE(observed[i] == 2u || observed[i] == 4u)
                << "spawned tile must be 2 or 4, got " << observed[i];
            observed[i] = 0;
            ++spawn_count;
        }
    }
    EXPECT_LE(spawn_count, 1) << "at most one new tile spawns per move";
}

}  // namespace

// Move left with no merge: tiles compress toward the left edge but no
// adjacent values are equal, so score does not change.
TEST(BoardMove, MoveLeftNoMerge) {
    Board b(0);
    const auto pre = make_tiles({
        0, 2, 0, 4,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    b.set_for_test(pre, /*seed=*/1);

    const MoveResult r = b.move(Direction::Left);
    EXPECT_TRUE(r.changed);
    EXPECT_FALSE(r.merged);
    EXPECT_EQ(r.score_delta, 0u);
    EXPECT_EQ(b.score(), 0u);

    auto after = snapshot(b);
    const auto expected = make_tiles({
        2, 4, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    erase_spawn_diff(after, pre, expected);
    EXPECT_EQ(after, expected);
}

// Move left with one merge: two adjacent 2s collapse into a single 4.
TEST(BoardMove, MoveLeftOneMerge) {
    Board b(0);
    const auto pre = make_tiles({
        2, 2, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    b.set_for_test(pre, /*seed=*/2);

    const MoveResult r = b.move(Direction::Left);
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(r.merged);
    EXPECT_EQ(r.score_delta, 4u);
    EXPECT_EQ(b.score(), 4u);
    EXPECT_TRUE(b.merged_this_turn(0, 0));

    auto after = snapshot(b);
    const auto expected = make_tiles({
        4, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    erase_spawn_diff(after, pre, expected);
    EXPECT_EQ(after, expected);
}

// A tile cannot merge twice in one move: [2,2,2,2] -> [4,4,0,0], not [8,0,0,0].
// This catches the classic off-by-one in the compress loop where freshly
// merged tiles get re-merged with the next equal value.
TEST(BoardMove, TileCannotMergeTwiceInOneMove) {
    Board b(0);
    const auto pre = make_tiles({
        2, 2, 2, 2,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    b.set_for_test(pre, /*seed=*/3);

    const MoveResult r = b.move(Direction::Left);
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(r.merged);
    EXPECT_EQ(r.score_delta, 8u);   // 4 + 4, not 8 + nothing-else
    EXPECT_EQ(b.score(), 8u);

    auto after = snapshot(b);
    const auto expected = make_tiles({
        4, 4, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    erase_spawn_diff(after, pre, expected);
    EXPECT_EQ(after, expected);
}

// Score must increase by the value of each merged tile (not the operands).
TEST(BoardMove, ScoreIncreasesByMergedTileValue) {
    Board b(0);
    const auto pre = make_tiles({
        4, 4, 0, 0,
        8, 8, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
    });
    b.set_for_test(pre, /*seed=*/4);

    const MoveResult r = b.move(Direction::Left);
    EXPECT_TRUE(r.changed);
    EXPECT_TRUE(r.merged);
    // 4+4 -> 8 (delta 8), 8+8 -> 16 (delta 16), total 24.
    EXPECT_EQ(r.score_delta, 24u);
    EXPECT_EQ(b.score(), 24u);
}

// If the move produces no change to the board, no new tile may spawn.
// This guards against the "every key press eventually fills the board"
// regression that hits if spawn_random_tile() runs unconditionally.
TEST(BoardMove, NoSpawnWhenBoardUnchanged) {
    Board b(0);
    const auto pre = make_tiles({
        2, 4, 8, 16,
        0, 0,  0,  0,
        0, 0,  0,  0,
        0, 0,  0,  0,
    });
    b.set_for_test(pre, /*seed=*/5);
    const int empties_before = b.empty_cell_count();

    const MoveResult r = b.move(Direction::Left);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.merged);
    EXPECT_EQ(r.score_delta, 0u);
    EXPECT_EQ(b.empty_cell_count(), empties_before);
    EXPECT_EQ(snapshot(b), pre);
}

// Game-over is reported when the board is full and no merges are possible.
// Constructed with a strict checkerboard of values that have no neighbours
// of equal value horizontally or vertically.
TEST(BoardMove, GameOverDetected) {
    Board b(0);
    const auto pre = make_tiles({
        2, 4, 2, 4,
        4, 2, 4, 2,
        2, 4, 2, 4,
        4, 2, 4, 2,
    });
    b.set_for_test(pre, /*seed=*/6);
    EXPECT_EQ(b.state(), GameState::GameOver);

    // Once GameOver, move() is a no-op: board, score, and state stay put.
    const MoveResult r = b.move(Direction::Left);
    EXPECT_FALSE(r.changed);
    EXPECT_FALSE(r.merged);
    EXPECT_EQ(r.score_delta, 0u);
    EXPECT_EQ(b.score(), 0u);
    EXPECT_EQ(b.state(), GameState::GameOver);
    EXPECT_EQ(snapshot(b), pre);
}

// reset() returns to a clean state: score zero, GameState::Playing, exactly
// two tiles on the board, every tile is 2 or 4.
TEST(BoardReset, ProducesCleanBoardWithTwoStartingTiles) {
    Board b(0);
    // Pollute the board to make the reset visible.
    b.set_for_test(make_tiles({
        2, 4, 2, 4,
        4, 2, 4, 2,
        2, 4, 2, 4,
        4, 2, 4, 2,
    }), /*seed=*/7);
    EXPECT_EQ(b.state(), GameState::GameOver);

    b.reset(/*seed=*/42);
    EXPECT_EQ(b.score(), 0u);
    EXPECT_EQ(b.state(), GameState::Playing);

    int non_empty = 0;
    for (int i = 0; i < kCellCount; ++i) {
        const uint32_t v = b.at(i % kBoardSize, i / kBoardSize);
        if (v != 0) {
            ++non_empty;
            EXPECT_TRUE(v == 2u || v == 4u) << "starting tile must be 2 or 4";
        }
    }
    EXPECT_EQ(non_empty, 2);
}

// Same seed must produce the same starting layout so manual demos and tests
// can be reproducible when needed.
TEST(BoardReset, IsDeterministicForSameSeed) {
    Board a(0);
    Board b(0);
    a.reset(123);
    b.reset(123);
    EXPECT_EQ(snapshot(a), snapshot(b));
}

// Merge highlight is per-turn: cleared at the start of the next move so the
// renderer only paints the highlight palette for one frame.
TEST(BoardMergeHighlight, ClearedAtStartOfNextMove) {
    Board b(0);
    b.set_for_test(make_tiles({
        2, 2, 0, 0,
        0, 0, 0, 4,
        0, 0, 0, 4,
        0, 0, 0, 0,
    }), /*seed=*/8);

    b.move(Direction::Left);
    EXPECT_TRUE(b.merged_this_turn(0, 0));

    // A subsequent move that produces no merge must clear all highlight bits,
    // even on cells that are not affected by this move's compression.
    b.move(Direction::Right);
    for (int y = 0; y < kBoardSize; ++y)
        for (int x = 0; x < kBoardSize; ++x)
            EXPECT_FALSE(b.merged_this_turn(x, y))
                << "highlight should be cleared at (" << x << "," << y << ")";
}
