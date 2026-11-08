/*
  4PC FFA engine - UCI-like protocol.
*/

#ifndef UCI_H_INCLUDED
#define UCI_H_INCLUDED

#include <cstdint>
#include <string>

#include "position.h"

namespace FFA {

void     uci_loop();
uint64_t perft(const Position& pos, int depth);
void     perft_divide(const Position& pos, int depth);
// Applies a move token ("d2d4", "R"/"resign", "T"/"timeout") to a game. False if illegal.
bool     apply_token(Game& game, const std::string& token);
std::string game_state_line(const Position& pos);

}  // namespace FFA

#endif  // UCI_H_INCLUDED
