/*
  4PC FFA engine - transposition table. Values are from the root player's point of view; the table is
  cleared whenever the engine searches for a different root player.
*/

#ifndef TT_H_INCLUDED
#define TT_H_INCLUDED

#include <cstddef>
#include <vector>

#include "types.h"

namespace FFA {

struct TTEntry {
    Key      key;
    uint32_t move;
    int32_t  value : 24;
    int32_t  depth : 8;
    uint8_t  bound;
    uint8_t  generation;
    uint16_t pad;
};

class TranspositionTable {
   public:
    void     resize(size_t mb);
    void     clear();
    void     new_search() { generation++; }
    TTEntry* probe(Key key, bool& found);
    void     store(TTEntry* e, Key key, Value v, Bound b, Depth d, Move m);
    int      hashfull() const;

   private:
    std::vector<TTEntry> table;
    size_t               mask = 0;
    uint8_t              generation = 0;
};

extern TranspositionTable TT;

}  // namespace FFA

#endif  // TT_H_INCLUDED
