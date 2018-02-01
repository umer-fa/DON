#ifndef _NOTATION_H_INC_
#define _NOTATION_H_INC_

#include <iostream>
#include "Thread.h"
#include "Type.h"

class Position;

const std::string PieceChar ("PNBRQK  pnbrqk");
const std::string ColorChar ("wb-");

namespace Notation {

    inline char to_char (File f, bool lower = true)
    {
        return char((lower ? 'a' : 'A') + i08(f) - i08(F_A));
    }

    inline char to_char (Rank r)
    {
        return char('1' + i08(r) - i08(R_1));
    }

    inline std::string to_string (Square s)
    {
        return std::string{ to_char (_file (s)), to_char (_rank (s)) };
    }
    /// Converts a value to a string suitable for use with the UCI protocol specifications:
    ///
    /// cp   <x>   The score x from the engine's point of view in centipawns.
    /// mate <y>   Mate in y moves, not plies.
    ///            If the engine is getting mated use negative values for y.
    inline std::string to_string (Value v)
    {
        assert(-VALUE_MATE <= v && v <= +VALUE_MATE);
        
        std::ostringstream oss;

        if (abs (v) < +VALUE_MATE - i32(MaxPlies))
        {
            oss << "cp " << value_to_cp (v);
        }
        else
        {
            oss << "mate " << i32(v > VALUE_ZERO ?
                                +(VALUE_MATE - v + 1) :
                                -(VALUE_MATE + v + 0)) / 2;
        }
        return oss.str ();
    }

    extern std::string move_to_can (Move);
    extern Move move_from_can (const std::string&, const Position&);

    extern std::string move_to_san (Move, Position&);
    extern Move move_from_san (const std::string&, Position&);

    //extern std::string move_to_lan (Move, Position&);
    //extern Move move_from_lan (const std::string&, Position&);

    extern std::string pretty_pv_info (Thread *const&);
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, File f)
{
    os << Notation::to_char (f);
    return os;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, Rank r)
{
    os << Notation::to_char (r);
    return os;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, Square s)
{
    os << Notation::to_string (s);
    return os;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, Move m)
{
    os << Notation::move_to_can (m);
    return os;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, Color c)
{
    os << ColorChar[c];
    return os;
}

template<typename CharT, typename Traits>
inline std::basic_ostream<CharT, Traits>&
    operator<< (std::basic_ostream<CharT, Traits> &os, Piece p)
{
    os << PieceChar[p];
    return os;
}

#endif // _NOTATION_H_INC_
