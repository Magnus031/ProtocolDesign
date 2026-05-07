#include "plugins/app_2048/app_2048_logic.h"

#include <vector>

namespace app_2048 {

Board::Board(uint64_t seed) : rng_(seed) {}

void Board::reset(uint64_t seed) {
    tiles_.fill(0);
    merged_.fill(false);
    score_ = 0;
    state_ = GameState::Playing;
    rng_.seed(seed);
    // Standard 2048 starts with two tiles.
    spawn_random_tile();
    spawn_random_tile();
}

uint32_t Board::at(int x, int y) const {
    return tiles_[index_of(x, y)];
}

bool Board::merged_this_turn(int x, int y) const {
    return merged_[index_of(x, y)];
}

int Board::empty_cell_count() const {
    int n = 0;
    for (uint32_t v : tiles_)
        if (v == 0) ++n;
    return n;
}

uint32_t Board::compress_line(std::array<uint32_t, kBoardSize>& line,
                              std::array<bool, kBoardSize>& out_merged) {
    std::array<uint32_t, kBoardSize> packed{};
    int n = 0;
    for (uint32_t v : line)
        if (v != 0) packed[n++] = v;

    std::array<uint32_t, kBoardSize> result{};
    std::array<bool, kBoardSize>     merged{};
    uint32_t score_delta = 0;

    int write = 0;
    int i = 0;
    while (i < n) {
        if (i + 1 < n && packed[i] == packed[i + 1]) {
            // Merge two adjacent equal tiles into one with double value.
            // Each tile in the original line can participate in at most one
            // merge per move because i advances by 2.
            const uint32_t merged_val = packed[i] * 2;
            result[write] = merged_val;
            merged[write] = true;
            score_delta  += merged_val;
            ++write;
            i += 2;
        } else {
            result[write++] = packed[i++];
        }
    }

    line       = result;
    out_merged = merged;
    return score_delta;
}

MoveResult Board::move(Direction d) {
    if (state_ == GameState::GameOver)
        return MoveResult{};

    // Each input clears last turn's merge highlights so the renderer only
    // shows them for the frame produced by the move that created them.
    merged_.fill(false);

    // Build 4 logical "lines" pointing in the move direction, compress each
    // line as if moving toward index 0 of the line, then write back.
    std::array<int, kBoardSize * kBoardSize> indices{};
    std::array<uint32_t, kBoardSize> line{};
    std::array<bool,     kBoardSize> line_merged{};

    auto fill_indices = [&](int line_no, Direction dir) {
        for (int k = 0; k < kBoardSize; ++k) {
            int x = 0;
            int y = 0;
            switch (dir) {
                case Direction::Left:
                    x = k;             y = line_no;
                    break;
                case Direction::Right:
                    // Reverse so index 0 of the working line is the rightmost
                    // cell — i.e. the destination side under "right".
                    x = kBoardSize - 1 - k; y = line_no;
                    break;
                case Direction::Up:
                    x = line_no;       y = k;
                    break;
                case Direction::Down:
                    x = line_no;       y = kBoardSize - 1 - k;
                    break;
            }
            indices[k] = index_of(x, y);
        }
    };

    bool     changed     = false;
    bool     merged_any  = false;
    uint32_t score_delta = 0;

    for (int line_no = 0; line_no < kBoardSize; ++line_no) {
        fill_indices(line_no, d);
        for (int k = 0; k < kBoardSize; ++k)
            line[k] = tiles_[indices[k]];

        const auto before = line;
        const uint32_t gained = compress_line(line, line_merged);

        if (line != before) {
            changed = true;
            for (int k = 0; k < kBoardSize; ++k) {
                tiles_[indices[k]]  = line[k];
                if (line_merged[k]) merged_[indices[k]] = true;
            }
        }
        if (gained > 0) {
            merged_any   = true;
            score_delta += gained;
        }
    }

    if (changed) {
        score_ += score_delta;
        spawn_random_tile();
        if (!any_move_possible())
            state_ = GameState::GameOver;
    }

    MoveResult r;
    r.changed     = changed;
    r.merged      = merged_any;
    r.score_delta = score_delta;
    return r;
}

void Board::spawn_random_tile() {
    std::vector<int> empties;
    empties.reserve(kCellCount);
    for (int i = 0; i < kCellCount; ++i)
        if (tiles_[i] == 0) empties.push_back(i);
    if (empties.empty()) return;

    std::uniform_int_distribution<size_t> pick(0, empties.size() - 1);
    const int target = empties[pick(rng_)];
    // Standard 2048: 90% spawn 2, 10% spawn 4.
    std::uniform_int_distribution<int> roll(0, 9);
    tiles_[target] = (roll(rng_) == 0) ? 4u : 2u;
}

bool Board::any_move_possible() const {
    for (int i = 0; i < kCellCount; ++i)
        if (tiles_[i] == 0) return true;
    // Adjacency check: any two horizontally or vertically adjacent equal
    // tiles means a merge (and therefore a move) is still possible.
    for (int y = 0; y < kBoardSize; ++y) {
        for (int x = 0; x < kBoardSize; ++x) {
            const uint32_t v = tiles_[index_of(x, y)];
            if (x + 1 < kBoardSize && tiles_[index_of(x + 1, y)] == v) return true;
            if (y + 1 < kBoardSize && tiles_[index_of(x, y + 1)] == v) return true;
        }
    }
    return false;
}

void Board::set_for_test(const std::array<uint32_t, kCellCount>& tiles,
                         uint64_t seed) {
    tiles_  = tiles;
    merged_.fill(false);
    rng_.seed(seed);
    state_ = any_move_possible() ? GameState::Playing : GameState::GameOver;
}

}  // namespace app_2048
