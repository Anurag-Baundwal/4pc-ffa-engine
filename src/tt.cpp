/*
  4PC FFA engine - transposition table.
*/

#include "tt.h"

#include <cstring>

namespace FFA {

TranspositionTable TT;

void TranspositionTable::resize(size_t mb) {
    size_t n = 1;
    while (n * 2 * sizeof(TTEntry) <= mb * 1024 * 1024) n *= 2;
    table.assign(n, TTEntry{});
    mask = n - 1;
    clear();
}

void TranspositionTable::clear() {
    if (!table.empty()) std::memset(static_cast<void*>(table.data()), 0, table.size() * sizeof(TTEntry));
    generation = 0;
}

TTEntry* TranspositionTable::probe(Key key, bool& found) {
    TTEntry* e = &table[key & mask];
    found      = e->key == key && e->bound != BOUND_NONE;
    return e;
}

void TranspositionTable::store(TTEntry* e, Key key, Value v, Bound b, Depth d, Move m) {
    // Replace unless the slot holds a deeper result for the same position from this search.
    if (e->key == key && e->generation == generation && d < e->depth && b != BOUND_EXACT) return;
    if (e->key != key || m) e->move = m.raw();
    e->key        = key;
    e->value      = v;
    e->depth      = std::clamp(d, -100, 127);
    e->bound      = b;
    e->generation = generation;
}

int TranspositionTable::hashfull() const {
    int n = 0;
    for (size_t i = 0; i < 1000 && i < table.size(); ++i) n += table[i].bound != BOUND_NONE && table[i].generation == generation;
    return n;
}

}  // namespace FFA
