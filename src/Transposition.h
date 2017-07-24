#ifndef _TRANSPOSITION_H_INC_
#define _TRANSPOSITION_H_INC_

#include <cstdlib>
#include <iostream>

#include "Type.h"
#include "Zobrist.h"
#include "MemoryHandler.h"
#include "Thread.h"

namespace Transposition {

    // Transposition::Entry needs 16 byte to be stored
    //
    //  Key--------- 16 bits
    //  Move-------- 16 bits
    //  Value------- 16 bits
    //  Evaluation-- 16 bits
    //  Depth------- 08 bits
    //  Generation-- 06 bits
    //  Bound------- 02 bits
    //  ====================
    //  Total------- 80 bits = 10 bytes
    struct Entry
    {
    private:
        u16 k16;
        u16 m16;
        i16 v16;
        i16 e16;
        i08 d08;
        u08 gb08;

        friend class Table;

    public:
        static const i08 Empty = DepthNone-1;

        // "Generation" variable distinguish transposition table entries from different searches.
        static u08 Generation;

        //u16   key16      () const { return u16  (k16); }
        Move  move       () const { return Move (m16); }
        Value value      () const { return Value(v16); }
        Value eval       () const { return Value(e16); }
        i16   depth      () const { return i16  (d08); }
        Bound bound      () const { return Bound(gb08 & 0x03); }
        u08   generation () const { return u08  (gb08 & 0xFC); }
        bool  empty      () const { return d08 == Empty; }

        // The worth of an entry is calculated as its depth minus 2 times its relative age.
        // Due to packed storage format for generation and its cyclic nature
        // add 0x103 (0x100 + 0x003 (BOUND_EXACT) to keep the lowest two bound bits from affecting the result)
        // to calculate the entry age correctly even after generation overflows into the next cycle.
        i16   worth      () const { return d08 - 2*((Generation + (0x103 - gb08)) & 0xFC); }

        void save (u64 k, Move m, Value v, Value e, i16 d, Bound b)
        {
            // Preserve more valuable entries
            if (   k16 != (k >> 0x30)
                || MOVE_NONE != m)
            {
                m16 = u16(m);
            }
            if (   k16 != (k >> 0x30)
                || d08 - 4 < d
                || BOUND_EXACT == b)
            {
                k16 = u16(k >> 0x30);
                gb08 = u08((   (   d08 - 4 < d
                                && !empty ())
                            || BOUND_EXACT == b ?
                                Generation :
                                generation ()) + b);
                d08 = i08(d);
                v16 = i16(v);
                e16 = i16(e);
            }
        }
    };

    const u08 CacheLineSize = 64;
    // Transposition::Cluster needs 32 bytes to be stored
    // 3 x 10 + 2
    struct Cluster
    {
    public:
        // Cluster entry count
        static const u08 EntryCount = 3;

        Entry entries[EntryCount];
        char padding[2]; // Align to a divisor of the cache line size
    };

    // Transposition::Table consists of a power of 2 number of clusters
    // and each cluster consists of Cluster::EntryCount number of entry.
    // Each non-empty entry contains information of exactly one position.
    // Size of a cluster should divide the size of a cache line size,
    // to ensure that clusters never cross cache lines.
    // In case it is less, it should be padded to guarantee always aligned accesses.
    // This ensures best cache performance, as the cacheline is prefetched.
    class Table
    {
    private:
        void alloc_aligned_memory (size_t mem_size, u32 alignment);

        void free_aligned_memory ();

    public:
        // Maximum bit of hash for cluster
        static const u08 MaxHashBit = 35;
        // Minimum size of Transposition::Table (4 MB)
        static const u32 MinHashSize = 4;
        // Maximum size of Transposition::Table (1048576 MB = 1048 GB = 1 TB)
        static const u32 MaxHashSize =
#       if defined(BIT64)
            (U64(1) << (MaxHashBit - 20)) * sizeof (Cluster);
#       else
            2048;
#       endif

        static const u32 BufferSize = 0x10000;

        void *mem;
        Cluster *clusters;
        size_t cluster_count;
        bool retain_hash;

        Table ()
            : mem (nullptr)
            , clusters (nullptr)
            , cluster_count (0)
            , retain_hash (false)
        {}

        Table (const Table&) = delete;
        Table& operator= (const Table&) = delete;
        
        ~Table ()
        {
            free_aligned_memory ();
        }

        size_t cluster_mask () const { return cluster_count - 1; }
        //size_t entry_count () const { return cluster_count * Cluster::EntryCount; }

        // Returns hash size in MB
        u32 size () const { return u32((cluster_count * sizeof (Cluster)) >> 20); }

        // Returns a pointer to the first entry of a cluster given a position.
        // The lower order bits of the key are used to get the index of the cluster inside the table.
        Entry* cluster_entry (const Key key) const { return clusters[size_t(key) & cluster_mask ()].entries; }

        u32 resize (u32 mem_size, bool force = false);
        u32 resize ();

        void auto_resize (u32 mem_size, bool force = false);

        void clear ();

        Entry* probe (const Key key, bool &tt_hit) const;

        u32 hash_full () const;

        void save (const std::string &hash_fn) const;
        void load (const std::string &hash_fn);

        template<typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits>&
            operator<< (std::basic_ostream<CharT, Traits> &os, const Table &tt)
        {
            u32 mem_size = tt.size ();
            u08 dummy = 0;
            os.write (reinterpret_cast<const CharT*> (&mem_size), sizeof (mem_size));
            os.write (reinterpret_cast<const CharT*> (&dummy), sizeof (dummy));
            os.write (reinterpret_cast<const CharT*> (&dummy), sizeof (dummy));
            os.write (reinterpret_cast<const CharT*> (&dummy), sizeof (dummy));
            os.write (reinterpret_cast<const CharT*> (&Entry::Generation), sizeof (Entry::Generation));
            os.write (reinterpret_cast<const CharT*> (&tt.cluster_count), sizeof (tt.cluster_count));
            for (u32 i = 0; i < tt.cluster_count / BufferSize; ++i)
            {
                os.write (reinterpret_cast<const CharT*> (tt.clusters+i*BufferSize), sizeof (Cluster)*BufferSize);
            }
            return os;
        }

        template<typename CharT, typename Traits>
        friend std::basic_istream<CharT, Traits>&
            operator>> (std::basic_istream<CharT, Traits> &is,       Table &tt)
        {
            u32 mem_size;
            u08 generation;
            u08 dummy;
            is.read (reinterpret_cast<CharT*> (&mem_size), sizeof (mem_size));
            is.read (reinterpret_cast<CharT*> (&dummy), sizeof (dummy));
            is.read (reinterpret_cast<CharT*> (&dummy), sizeof (dummy));
            is.read (reinterpret_cast<CharT*> (&dummy), sizeof (dummy));
            is.read (reinterpret_cast<CharT*> (&generation), sizeof (generation));
            is.read (reinterpret_cast<CharT*> (&tt.cluster_count), sizeof (tt.cluster_count));
            tt.resize (mem_size);
            Entry::Generation = generation;
            for (u32 i = 0; i < tt.cluster_count / BufferSize; ++i)
            {
                is.read (reinterpret_cast<CharT*> (tt.clusters+i*BufferSize), sizeof (Cluster)*BufferSize);
            }
            return is;
        }
    };
}

// Global Transposition Table
extern Transposition::Table TT;

#endif // _TRANSPOSITION_H_INC_
