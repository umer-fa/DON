#pragma once

#include <deque>
#include <memory> // For std::unique_ptr
#include <string>
#include <vector>

#include "Bitboard.h"
#include "Type.h"

/// StateInfo stores information needed to restore a Position object to its previous state when we retract a move.
///
///  - Hash key of the material situation
///  - Hash key of the pawn structure
///  - Hash key of the position
///  - Castling-rights information
///  - Enpassant square(SQ_NONE if no Enpassant capture is possible)
///  - Clock for detecting 50 move rule draws
///  - Piece type captured on last position
///  - Repetition info
///  - Bitboards of kingBlockers & kingCheckers
///  - Bitboards of all checking pieces
///  - Pointer to previous StateInfo
struct StateInfo {
    // ---Copied when making a move---
    Key         matlKey;        // Hash key of materials
    Key         pawnKey;        // Hash key of pawns
    CastleRight castleRights;   // Castling-rights information
    Square      epSquare;       // Enpassant -> "In passing"
    i16         clockPly;       // Number of half moves clock since the last pawn advance or any capture
    i16         nullPly;

    // ---Not copied when making a move---
    Key         posiKey;        // Hash key of position
    Bitboard    checkers;       // Checkers
    PieceType   captured;       // Piece type captured
    bool        promoted;
    i16         repetition;
    // Check info
    Array<Bitboard, COLORS> kingBlockers; // Absolute and Discover Blockers
    Array<Bitboard, COLORS> kingCheckers; // Absolute and Discover Checkers
    Array<Bitboard, PIECE_TYPES> checks;

    StateInfo *ptr;             // Previous StateInfo pointer

    void clear();
};

/// A list to keep track of the position states along the setup moves
/// (from the start position to the position just before the search starts).
/// Needed by 'draw by repetition' detection.
/// Use a std::deque because pointers to elements are not invalidated upon list resizing.
using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

class Thread;

extern Array<Score, PIECES, SQUARES> PSQ;

/// Position class stores information regarding the board representation:
///  - 64-entry array of pieces, indexed by the square.
///  - Bitboards of each piece type.
///  - Bitboards of each color
///  - Bitboard of occupied square.
///  - List of square for the pieces.
///  - Information about the castling rights.
///  - Initial files of both pairs of rooks, castle path and kings path, this is used to implement the Chess960 castling rules.
///  - Color of side on move.
///  - Ply of the game.
///  - StateInfo pointer for the current status.
class Position {

private:

    Array<Piece, SQUARES> board;
    Array<Bitboard, COLORS> colors;
    Array<Bitboard, PIECE_TYPES> types;
    Array<std::vector<Square>, PIECES> squareSet;

    Array<Value, COLORS> npMaterial;

    Table<Square, COLORS, CASTLE_SIDES> cslRookSq;
    Table<Bitboard, COLORS, CASTLE_SIDES> cslKingPath;
    Table<Bitboard, COLORS, CASTLE_SIDES> cslRookPath;
    Array<CastleRight, SQUARES> sqCastleRight;

    Color active;
    Score psq;
    i16   ply;
    Thread *_thread;

    StateInfo *_stateInfo;

    void placePiece(Square, Piece);
    void removePiece(Square);
    void movePiece(Square, Square);

    void setCastle(Color, Square);
    void setCheckInfo();

    bool canEnpassant(Color, Square, bool = true) const;

public:

    //static void initialize();

    Position() = default;
    Position(Position const&) = delete;
    Position(Position&&) = delete;
    Position& operator=(Position const&) = delete;
    Position& operator=(Position&&) = delete;

    Piece operator[](Square) const;
    bool empty(Square) const;

    //Bitboard pieces(Piece) const;
    Bitboard pieces(Color) const;
    Bitboard pieces(PieceType = NONE) const;
    template<typename... PieceTypes>
    Bitboard pieces(PieceType, PieceTypes...) const;
    template<typename... PieceTypes>
    Bitboard pieces(Color, PieceTypes...) const;

    i32 count(Piece) const;
    i32 count(PieceType) const;
    i32 count(Color) const;
    i32 count() const;
    std::vector<Square> const& squares(Piece) const;
    Square square(Piece, u08 = 0) const;

    Value nonPawnMaterial(Color) const;
    Value nonPawnMaterial() const;

    Square   castleRookSq(Color, CastleSide) const;
    Bitboard castleKingPath(Color, CastleSide) const;
    Bitboard castleRookPath(Color, CastleSide) const;
    //CastleRight castleRight(Square) const;

    CastleRight castleRights() const;
    bool canCastle(Color) const;
    bool canCastle(Color, CastleSide) const;
    Square epSquare() const;

    i16 clockPly() const;
    i16 nullPly() const;

    Key matlKey() const;
    Key pawnKey() const;
    Key posiKey() const;
    Bitboard checkers() const;
    PieceType captured() const;
    bool promoted() const;
    i16 repetition() const;

    Bitboard kingBlockers(Color) const;
    Bitboard kingCheckers(Color) const;
    Bitboard checks(PieceType) const;

    Color activeSide() const;
    Score psqScore() const;
    i16 gamePly() const;
    Thread* thread() const;
    void thread(Thread*);

    bool castleExpeded(Color, CastleSide) const;

    Key pgKey() const;
    Key movePosiKey(Move) const;

    i16  moveCount() const;
    bool draw(i16) const;
    bool repeated() const;
    bool cycled(i16) const;

    Bitboard attackersTo(Square, Bitboard) const;
    Bitboard attackersTo(Square) const;
    Bitboard attacksFrom(Square, Bitboard) const;
    Bitboard attacksFrom(Square) const;
    Bitboard attacksFrom(PieceType, Square) const;

    Bitboard sliderBlockersAt(Square, Bitboard, Bitboard&, Bitboard&) const;

    bool capture(Move) const;
    bool captureOrPromotion(Move) const;

    bool pseudoLegal(Move) const;
    bool legal(Move) const;
    bool giveCheck(Move) const;
    bool giveDblCheck(Move) const;

    PieceType captured(Move) const;

    bool see(Move, Value = VALUE_ZERO) const;

    bool pawnAdvanceAt(Color, Square) const;
    bool pawnPassedAt(Color, Square) const;
    Bitboard pawnsOnSqColor(Color, Color) const;

    bool bishopPaired(Color) const;
    bool bishopOpposed() const;
    bool semiopenFileOn(Color, Square) const;

    void clear();

    Position& setup(std::string const&, StateInfo&, Thread *const = nullptr);
    Position& setup(std::string const&, Color, StateInfo&);

    void doMove(Move, StateInfo&, bool);
    void doMove(Move, StateInfo&);
    void undoMove(Move);
    void doNullMove(StateInfo&);
    void undoNullMove();

    void flip();
    void mirror();

    std::string fen(bool full = true) const;

    std::string toString() const;

#if !defined(NDEBUG)
    bool ok() const;
#endif

};

extern std::ostream& operator<<(std::ostream&, Position const&);


inline Piece Position::operator[](Square s) const {
    assert(isOk(s));
    return board[s];
}
inline bool Position::empty(Square s) const {
    return operator[](s) == NO_PIECE;
}

//inline Bitboard Position::pieces(Piece p) const { return colors[pColor(p)] & types[pType(p)]; }
inline Bitboard Position::pieces(Color c) const {
    return colors[c];
}
inline Bitboard Position::pieces(PieceType pt) const {
    return types[pt];
}
template<typename... PieceTypes>
inline Bitboard Position::pieces(PieceType pt, PieceTypes... pts) const {
    return types[pt] | pieces(pts...);
}
template<typename... PieceTypes>
inline Bitboard Position::pieces(Color c, PieceTypes... pts) const {
    return colors[c] & pieces(pts...);
}

/// Position::count() counts specific piece
inline i32 Position::count(Piece p) const {
    return i32(squareSet[p].size());
}
/// Position::count() counts specific type
inline i32 Position::count(PieceType pt) const {
    return count(WHITE|pt) + count(BLACK|pt);
}
/// Position::count() counts specific color
inline i32 Position::count(Color c) const {
    return count(c|PAWN) + count(c|NIHT) + count(c|BSHP) + count(c|ROOK) + count(c|QUEN) + count(c|KING);
}
/// Position::count() counts all
inline i32 Position::count() const {
    return count(WHITE) + count(BLACK);
}


inline std::vector<Square> const& Position::squares(Piece p) const {
    return squareSet[p];
}
inline Square Position::square(Piece p, u08 index) const {
    assert(isOk(p));
    assert(count(p) > index);
    //return *(squares(p).begin() + index);
    return squares(p)[index];
}

//inline CastleRight Position::castleRight(Square s) const { return sqCastleRight[s]; }

inline Value Position::nonPawnMaterial(Color c) const {
    return npMaterial[c];
}
inline Value Position::nonPawnMaterial() const {
    return nonPawnMaterial(WHITE) + nonPawnMaterial(BLACK);
}

inline Square Position::castleRookSq(Color c, CastleSide cs) const {
    return cslRookSq[c][cs];
}
inline Bitboard Position::castleKingPath(Color c, CastleSide cs) const {
    return cslKingPath[c][cs];
}
inline Bitboard Position::castleRookPath(Color c, CastleSide cs) const {
    return cslRookPath[c][cs];
}

inline CastleRight Position::castleRights() const {
    return _stateInfo->castleRights;
}
inline bool Position::canCastle(Color c) const {
    return (castleRights() & makeCastleRight(c)) != CR_NONE;
}
inline bool Position::canCastle(Color c, CastleSide cs) const {
    return (castleRights() & makeCastleRight(c, cs)) != CR_NONE;
}
inline Square Position::epSquare() const {
    return _stateInfo->epSquare;
}

inline i16 Position::clockPly() const {
    return _stateInfo->clockPly;
}
inline i16 Position::nullPly() const {
    return _stateInfo->nullPly;
}

inline Key Position::matlKey() const {
    return _stateInfo->matlKey;
}
inline Key Position::pawnKey() const {
    return _stateInfo->pawnKey;
}
inline Key Position::posiKey() const {
    return _stateInfo->posiKey;
}
inline Bitboard Position::checkers() const {
    return _stateInfo->checkers;
}
inline PieceType Position::captured() const {
    return _stateInfo->captured;
}
inline bool Position::promoted() const {
    return _stateInfo->promoted;
}
inline i16 Position::repetition() const {
    return _stateInfo->repetition;
}

inline Bitboard Position::kingBlockers(Color c) const {
    return _stateInfo->kingBlockers[c];
}
inline Bitboard Position::kingCheckers(Color c) const {
    return _stateInfo->kingCheckers[c];
}
inline Bitboard Position::checks(PieceType pt) const {
    return _stateInfo->checks[pt];
}

inline Color Position::activeSide() const {
    return active;
}
inline Score Position::psqScore() const {
    return psq;
}
inline i16 Position::gamePly() const {
    return ply;
}

inline Thread* Position::thread() const {
    return _thread;
}
inline void Position::thread(Thread *th) {
    _thread = th;
}

inline bool Position::castleExpeded(Color c, CastleSide cs) const {
    return (castleRookPath(c, cs) & pieces()) == 0;
}
/// Position::moveCount() starts at 1, and is incremented after BLACK's move.
inline i16 Position::moveCount() const {
    return i16(std::max((ply - active) / 2, 0) + 1);
}

inline void Position::placePiece(Square s, Piece p) {
    //assert(isOk(p));
    //assert(std::count(squareSet[p].begin(), squareSet[p].end(), s) == 0);
    colors[pColor(p)] |= s;
    types[pType(p)] |= s;
    types[NONE] |= s;
    psq += PSQ[p][s];
    squareSet[p].push_back(s);
    board[s] = p;
}
inline void Position::removePiece(Square s) {
    auto p{ board[s] };
    //assert(isOk(p));
    //assert(squareSet[p].size() != 0);
    //assert(std::count(squareSet[p].begin(), squareSet[p].end(), s) == 1);
    colors[pColor(p)] ^= s;
    types[pType(p)] ^= s;
    types[NONE] ^= s;
    psq -= PSQ[p][s];
    // Do keep order
    //squareSet[p].erase(std::find(squareSet[p].begin(), squareSet[p].end(), s));
    // Don't keep order
    if (squareSet[p].size() > 1) {
        *std::find(squareSet[p].begin(), squareSet[p].end(), s) = squareSet[p].back();
    }
    squareSet[p].pop_back();
    //board[s] = NO_PIECE; // Not needed, overwritten by the capturing one
}
inline void Position::movePiece(Square s1, Square s2) {
    auto p{ board[s1] };
    //assert(isOk(p));
    //assert(squareSet[p].size() != 0);
    //assert(std::count(squareSet[p].begin(), squareSet[p].end(), s1) == 1
    //    && std::count(squareSet[p].begin(), squareSet[p].end(), s2) == 0);
    Bitboard bb{ s1 | s2 };
    colors[pColor(p)] ^= bb;
    types[pType(p)] ^= bb;
    types[NONE] ^= bb;
    psq += PSQ[p][s2]
         - PSQ[p][s1];
    *std::find(squareSet[p].begin(), squareSet[p].end(), s1) = s2;
    board[s2] = p;
    board[s1] = NO_PIECE;
}

/// Position::attackersTo() finds attackers to the square on occupancy.
inline Bitboard Position::attackersTo(Square s, Bitboard occ) const {
    return (pieces(BLACK, PAWN) & PawnAttackBB[WHITE][s])
         | (pieces(WHITE, PAWN) & PawnAttackBB[BLACK][s])
         | (pieces(NIHT)        & attacksFrom(NIHT, s))
         | (pieces(BSHP, QUEN)  & attacksBB<BSHP>(s, occ))
         | (pieces(ROOK, QUEN)  & attacksBB<ROOK>(s, occ))
         | (pieces(KING)        & attacksFrom(KING, s));
}
/// Position::attackersTo() finds attackers to the square.
inline Bitboard Position::attackersTo(Square s) const {
    return attackersTo(s, pieces());
}

inline Bitboard Position::attacksFrom(Square s, Bitboard occ) const {
    return attacksBB(pType(board[s]), s, occ);
}
inline Bitboard Position::attacksFrom(Square s) const {
    return attacksFrom(s, pieces());
}

/// Position::attacksFrom() finds attacks of the piecetype from the square
inline Bitboard Position::attacksFrom(PieceType pt, Square s) const {
    return attacksBB(pt, s, pieces());
}

inline bool Position::capture(Move m) const {
    assert(isOk(m));
    return mType(m) != ENPASSANT ?
            contains(pieces(~active), dstSq(m)) : true;
}
inline bool Position::captureOrPromotion(Move m) const {
    assert(isOk(m));
    return mType(m) == SIMPLE ?
            contains(pieces(~active), dstSq(m)) : mType(m) != CASTLE;
}
inline PieceType Position::captured(Move m) const {
    assert(isOk(m));
    return mType(m) != ENPASSANT ? pType(operator[](dstSq(m))) : PAWN;
}
/// Position::pawnAdvanceAt() check if pawn is advanced at the given square
inline bool Position::pawnAdvanceAt(Color c, Square s) const {
    return contains(/*pieces(c, PAWN) & */PawnSideBB[~c], s);
}
/// Position::pawnPassedAt() check if pawn passed at the given square
inline bool Position::pawnPassedAt(Color c, Square s) const {
    return (pieces(~c, PAWN) & pawnPassSpan(c, s)) == 0;
}

inline Bitboard Position::pawnsOnSqColor(Color c, Color sqC) const {
    return pieces(c, PAWN) & ColorBB[sqC];
}

/// Position::bishopPaired() check the side has pair of opposite color bishops
inline bool Position::bishopPaired(Color c) const {
    Bitboard b{ pieces(c, BSHP) };
    return moreThanOne(b)
        && ((b & ColorBB[WHITE]) != 0) == ((b & ColorBB[BLACK]) != 0);
}
inline bool Position::bishopOpposed() const {
    return count(W_BSHP) == 1
        && count(B_BSHP) == 1
        && colorOpposed(square(W_BSHP), square(B_BSHP));
}

inline bool Position::semiopenFileOn(Color c, Square s) const {
    return (pieces(c, PAWN) & fileBB(s)) == 0;
}

inline void Position::doMove(Move m, StateInfo &si) {
    doMove(m, si, giveCheck(m));
}


#if !defined(NDEBUG)

extern bool isOk(std::string const&);

#endif
