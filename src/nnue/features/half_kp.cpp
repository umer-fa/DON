// Definition of input features HalfKP of NNUE evaluation function

#include "../../position.h"
#include "half_kp.h"
#include "index_list.h"

namespace Evaluator::NNUE::Features {

    constexpr int32_t OrientSquare[COLORS]{
        SQ_A1, SQ_H8
    };

    // Orient a square according to perspective (rotates by 180 for black)
    inline Square orient(Color perspective, Square s) {
        return Square(int32_t(s) ^ OrientSquare[perspective]);
    }

    // Find the index of the feature quantity from the king position and PieceSquare
    template <Side AssociatedKing>
    inline IndexType HalfKP<AssociatedKing>::makeIndex(Color perspective, Square s, Piece pc, Square kSq) {
        return IndexType(orient(perspective, s) + PP_BoardIndex[pc][perspective] + PS_END * kSq);
    }

    // Get a list of indices for active features
    template<Side AssociatedKing>
    void HalfKP<AssociatedKing>::appendActiveIndices(Position const &pos, Color perspective, IndexList *active) {

        Square const kSq{ orient(perspective, pos.square(perspective|KING)) };
        Bitboard bb{ pos.pieces() & ~pos.pieces(KING) };
        while (bb != 0) {
            Square const s{ popLSq(bb) };
            active->push_back(makeIndex(perspective, s, pos[s], kSq));
        }
    }

    // Get a list of indices for recently changed features
    template<Side AssociatedKing>
    void HalfKP<AssociatedKing>::appendChangedIndices(Position const &pos, DirtyPiece const &dp, Color perspective, IndexList *removed, IndexList *added) {

        Square const kSq{ orient(perspective, pos.square(perspective|KING)) };
        for (int i = 0; i < dp.dirtyCount; ++i) {
            if (pType(dp.piece[i]) == KING) {
                continue;
            }
            if (dp.org[i] != SQ_NONE) {
                removed->push_back(makeIndex(perspective, dp.org[i], dp.piece[i], kSq));
            }
            if (dp.dst[i] != SQ_NONE) {
                added->push_back(makeIndex(perspective, dp.dst[i], dp.piece[i], kSq));
            }
        }
    }

    template class HalfKP<Side::FRIEND>;

}