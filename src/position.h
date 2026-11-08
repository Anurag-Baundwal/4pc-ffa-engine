/*
  4PC FFA engine - board state, rules and move generation.

  Positions are copy-make: `Position child = pos; child.do_move(m);`. A Position holds the complete game
  state (pieces, side to move, castling, en passant, player states, points), so a move can apply every
  FFA consequence (points, check bonuses, immediate checkmates, eliminations, stalemate, game end) without
  an undo path.
*/

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <string>
#include <vector>

#include "bitboard.h"
#include "types.h"

namespace FFA {

// Rule switches. The defaults are the chess.com standard FFA rules ("DeadKingWalking EnPassant
// PromoteTo=D") as established by `ffa_engine audit` against human games; see 4PC_FFA_RULES.md.
struct RuleConfig {
    int  pieceValue[PIECE_TYPE_NB] = {0, 1, 3, 5, 5, 9, 20, 0};  // capture points by type (K = king capture)
    int  promotedValue             = 1;    // a promoted queen ("D")
    int  grayValue                 = 0;    // any piece of a non-alive player
    int  grayKingValue             = 0;    // the king of a dead player
    int  zombieKingValue           = 20;   // the king of a zombie (resigned/flagged) player
    int  zombieStalemateSplit      = 20;   // a stalemated zombie: shared (rounded up) by the alive players
    int  mateBonus                 = 20;   // per player checkmated by a move
    int  stalemateBonus            = 20;   // to the stalemated player
    int  checkBonus2               = 1;    // a move that newly checks 2 kings
    int  checkBonus3               = 5;    // ... 3 kings
    bool zombiesCountForChecks     = false; // zombie kings count for the check bonus
    bool zombieKingAttacks         = true;  // a zombie king attacks the squares around it
    bool zombieScores              = false; // a zombie earns points for its king's captures
    int  endBonusPerKing           = 20;   // at game end, the winner gets this per non-alive king left
    bool endBonusCountsDeadKings   = false; // ... counting kings of dead (mated/stalemated) players too
    int  drawBonus                 = 20;   // drawn ending: each of n alive players gets drawBonus*(n-1)/n
    int  rule50Moves               = 50;   // moves per moving player without a capture of an alive
                                           // player's piece or a pawn move that end the game
    int  promotionRelRank          = 7;    // relative rank (0-based) where pawns promote: the 8th rank
};

extern RuleConfig Rules;

// Sets a rule switch by name ("zombieKingAttacks", "checkBonus2", ...). False if the name is unknown.
bool set_rule(const std::string& name, int value);
std::string rules_string();

enum EndReason : uint8_t {
    END_NONE, END_LAST_PLAYER, END_INSUFFICIENT, END_RULE50, END_REPETITION, END_ADJUDICATED
};

const char* end_reason_name(EndReason r);

// Things that happened during the last do_move/resign, for the audit and for move notation.
struct MoveEvents {
    uint8_t checkedMask   = 0;  // kings newly checked by the move
    uint8_t matedMask     = 0;  // players checkmated by the move (immediate)
    uint8_t turnMateMask  = 0;  // players found mated at the start of their turn (no mating move)
    uint8_t stalemateMask = 0;  // players stalemated at the start of their turn
    uint8_t kingCapMask   = 0;  // players whose king was captured
    int     checkBonus    = 0;
    int     captureValue  = 0;
};

class Position {
   public:
    static void init();

    void        set_startpos();
    bool        set(const std::string& fen4, std::string* error = nullptr);
    std::string fen() const;
    std::string pretty() const;

    // --- State ---------------------------------------------------------------------------------
    Color        side_to_move() const { return sideToMove; }
    Piece        piece_on(Square s) const { return Piece(board[s]); }
    bool         empty(Square s) const { return board[s] == NO_PIECE; }
    Bitboard     pieces() const { return byTypeBB[ALL_PIECES]; }
    Bitboard     pieces(Color c) const { return byColorBB[c]; }
    Bitboard     pieces(PieceType pt) const { return byTypeBB[pt]; }
    Bitboard     pieces(Color c, PieceType pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard     promoted() const { return promotedBB; }
    Square       king_square(Color c) const { return kingSq[c]; }
    PlayerStatus status(Color c) const { return PlayerStatus(playerStatus[c]); }
    bool         is_alive(Color c) const { return playerStatus[c] == ALIVE; }
    int          points(Color c) const { return pts[c]; }
    int          castling_rights() const { return castling; }
    Square       ep_square(Color c) const { return epSquares[c]; }
    int          rule50_count() const { return rule50; }
    int          game_ply() const { return gamePly; }
    Key          key() const { return posKey ^ pointsKey(); }  // TT key: board + points
    Key          board_key() const { return posKey; }          // repetition key: board only
    int          alive_count() const;
    bool         game_over() const { return endReason != END_NONE; }
    EndReason    end_reason() const { return EndReason(endReason); }
    const MoveEvents& events() const { return lastEvents; }
    int          end_bonus(Color c) const { return endBonus[c]; }  // points added by the game end

    // Pieces of players whose pieces attack (alive players, plus zombie kings if the rule says so).
    Bitboard attacking_pieces() const;
    Bitboard attackers_to(Square s, const Bitboard& occ) const;  // all colors, before status filtering
    Bitboard checkers(Color c) const { return checkersBB[c]; }
    bool     in_check(Color c) const { return bool(checkersBB[c]); }
    bool     attacked_by_others(Square s, Color us, const Bitboard& occ) const;

    // --- Moves ---------------------------------------------------------------------------------
    // Legal moves of the side to move (`c` for another player, as if it were their turn).
    int  generate_legal(Move* list) const { return generate_legal_for(sideToMove, list, false); }
    int  generate_captures(Move* list) const { return generate_legal_for(sideToMove, list, true); }
    int  generate_legal_for(Color c, Move* list, bool capturesOnly) const;
    bool has_legal_move(Color c) const;
    bool is_legal_move(Move m) const;   // m is one of generate_legal()
    bool is_capture(Move m) const { return !empty(m.to_sq()) || m.type_of() == EN_PASSANT; }
    int  capture_value(Square s) const;  // points for capturing the piece on s
    Move parse_move(const std::string& s) const;  // "d2d4", "g7g8d"/"g7g8q"; Move::none() if not legal

    // Applies a legal move of the side to move and all its consequences.
    void do_move(Move m);
    // The side to move makes no move (search: zombie turns and null moves). Turn passes on.
    void do_pass();
    // Player c resigns or flags (becomes a zombie). If c is the side to move and passTurn is set, this
    // uses c's turn; otherwise c's zombie king makes the move of this turn ("Ka7-b7R" on chess.com).
    void resign(Color c, bool passTurn = true);
    // Ends the game now: alive players share the draw bonus, per the drawn-ending rules.
    void end_drawn(EndReason reason);
    // True if c resigning now ends the game with c strictly first: chess.com's "claim win" (see .cpp).
    bool resign_wins(Color c) const;
    // Undoes the game end (and its bonuses): for playing on when chess.com doesn't see the game as over.
    void reopen();

    bool insufficient_material() const;

    // Debug: recomputes the hash and compares with the incremental one.
    bool hash_ok() const;

   private:
    void put_piece(Piece pc, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);
    void set_castling_right(Color c, Square rfrom);
    void compute_checkers();
    void eliminate(Color c, PlayerStatus newStatus);
    void advance_turn();
    void check_game_end();
    void finish_game(EndReason reason);
    void add_points(Color c, int n);
    Key  compute_key() const;
    Key  pointsKey() const;

    template<Color Us> Move* gen_pawn_moves(Move* list, const Bitboard& enemies, bool capturesOnly) const;
    Move* gen_piece_moves(Color us, Move* list, const Bitboard& targets) const;
    Move* gen_castling(Color us, Move* list) const;
    bool  legal_for(Color us, Move m) const;

    uint8_t  board[SQUARE_NB];
    Bitboard byTypeBB[PIECE_TYPE_NB];
    Bitboard byColorBB[COLOR_NB];
    Bitboard promotedBB;
    Bitboard checkersBB[COLOR_NB];
    Square   kingSq[COLOR_NB];
    Square   epSquares[COLOR_NB];
    Square   castlingRookSquare[8];  // by bit index of the right
    uint8_t  castlingRightsMask[SQUARE_NB];
    Key      posKey;
    int      pts[COLOR_NB];
    int      rule50;
    int      gamePly;
    int      castling;
    Color    sideToMove;
    uint8_t  playerStatus[COLOR_NB];
    uint8_t  paidKings;  // zombies whose king's 20 points were already awarded (resigned in check)
    uint8_t  endReason;
    Color    lastMover;
    int      endBonus[COLOR_NB];
    MoveEvents lastEvents;
};

// A game: a root position plus the moves played, with repetition detection. Used by the protocol and the
// audit; the search keeps its own key stack.
struct Game {
    Position              pos;
    std::vector<Key>      history;  // board keys of every position reached, including the current one
    void                  reset(const Position& p) { pos = p; history.assign(1, p.board_key()); }
    void                  play(Move m);     // also detects threefold repetition
    void                  pass_turn();      // resign handled by caller
    int                   repetitions() const;  // times the current board key occurred before
};

}  // namespace FFA

#endif  // POSITION_H_INCLUDED
