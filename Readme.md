## Overview

[![Build Status](https://www.donchess.net)](https://www.donchess.net)

[DON] is a free, powerful UCI chess engine.
It features two evaluation functions, the classical evaluation based on handcrafted terms,
and the NNUE evaluation based on efficiently updateable neural networks.
The classical evaluation runs efficiently on most 64bit CPU architectures,
while the NNUE evaluation benefits strongly from the vector intrinsics
available on modern CPUs (avx2 or similar).

DON is a free UCI chess engine. It is not a complete chess program
and requires some UCI-compatible GUI (e.g. XBoard with Polyglot,
eboard, Arena, Sigma Chess, Shredder, Chess Partner or Fritz)
in order to be used comfortably. Read the documentation for GUI
of your choice for information about how to use engine with it.

## Features

DON uses bitboard representations, and is an alfa-beta searcher.

DON supports up to 512 cores. The engine defaults to one search thread,
so it is therefore recommended to inspect the value of the 'Threads'
UCI parameter, to make sure it equals the # of CPU cores on your computer.

DON has support for 32/64-bit CPUs, the hardware ABMI/BMI2 instruction,
big-endian machines such as Power PC, and other platforms.

DON has support for Polyglot book.
For information about how to create such books, consult the Polyglot documentation.
The book file can be selected by setting the *Book File* UCI parameter.

DON has support for Syzygybases.

## Files

This distribution of DON consists of the following files:

  * Readme.md, the file you are currently reading.

  * Copying.txt, a text file containing the GNU General Public License.

  * src, a subdirectory containing the full source code, including a Makefile
    that can be used to compile DON on Unix-like systems.

To use the NNUE evaluation an additional data file with neural network parameters
needs to be downloaded. The filename for the default set can be found as the default
value of the `EvalFile` UCI option, with the format
`nn-[SHA256 first 12 digits].nnue` (e.g. nn-c157e0a5755b.nnue).

## UCI options

Currently, DON has the following UCI options:

  * #### Hash
    The size of the hash table in MB.

  * #### Clear Hash
    Clear the hash table.

  * #### Retain Hash
    Retain the hash table.

  * #### Hash File
    Hash file name.
    
  * #### Threads
    The number of CPU threads used for searching a position. For best performance, set
    this equal to the number of CPU cores available.

  * #### Skill Level
    Lower the Skill Level in order to make DON play weaker (see also UCI_LimitStrength).
    Internally, MultiPV is enabled, and with a certain probability depending on the Skill Level a
    weaker move will be played.
    
  * #### MultiPV
    Output the N best lines (principal variations, PVs) when searching.
    Leave at 1 for best performance.
    
  * #### Contempt
    A positive value for contempt favors middle game positions and avoids draws.

  * #### Analysis Contempt
    By default, contempt is set to prefer the side to move. Set this option to "White"
    or "Black" to analyse with contempt for that side, or "Off" to disable contempt.

  * #### Use NNUE
    Toggle between the NNUE and classical evaluation functions. If set to "true",
    the network parameters must be available to load from file (see also EvalFile).

  * #### EvalFile
    The name of the file of the NNUE evaluation parameters. Depending on the GUI the
    filename should include the full path to the folder/directory that contains the file.
    
  * #### Overhead Move Time
    Assume a time delay of x ms due to network and GUI overheads. This is useful to
    avoid losses on time in those cases.

  * #### Move Slowness
    Lower values will make DON take less time in games, higher values will
    make it think longer.

  * #### Ponder
    Let DON ponder its next move while the opponent is thinking.
    
  * #### Nodes Time
    Tells the engine to use nodes searched instead of wall time to account for
    elapsed time. Useful for engine testing.

  * #### Debug File
    Write all communication to and from the engine into a text file.

  * #### SyzygyPath
    Path to the folders/directories storing the Syzygy tablebase files. Multiple
    directories are to be separated by ";" on Windows and by ":" on Unix-based
    operating systems. Do not use spaces around the ";" or ":".

    Example: `C:\tablebases\wdl345;C:\tablebases\wdl6;D:\tablebases\dtz345;D:\tablebases\dtz6`

    It is recommended to store .rtbw files on an SSD. There is no loss in storing
    the .rtbz files on a regular HD. It is recommended to verify all md5 checksums
    of the downloaded tablebase files (`md5sum -c checksum.md5`) as corruption will
    lead to engine crashes.

  * #### SyzygyDepthLimit
    Minimum remaining search depth for which a position is probed. Set this option
    to a higher value to probe less agressively if you experience too much slowdown
    (in terms of nps) due to TB probing.

  * #### SyzygyMove50Rule
    Count drawn by the 50-move rule as win or loss / draw
    'true' -> draw
    'false' -> win or lose.
    This is useful for ICCF correspondence games.

  * #### SyzygyPieceLimit
    Limit Syzygy tablebase probing to positions with at most this many pieces left
    (including kings and pawns).

  * #### UCI_Chess960
    An option handled by your GUI. If true, DON will play Chess960.

  * #### UCI_ShowWDL
    If enabled, show approximate WDL statistics as part of the engine output.
    These WDL numbers model expected game outcomes for a given evaluation and
    game ply for engine self-play at fishtest LTC conditions (60+0.6s per game).

  * #### UCI_LimitStrength
    Enable weaker play aiming for an Elo rating as set by UCI_Elo. This option overrides Skill Level.

  * #### UCI_Elo
    If enabled by UCI_LimitStrength, aim for an engine strength of the given Elo.
    This Elo rating has been calibrated at a time control of 60s+0.6s and anchored to CCRL 40/4.
    
## Classical and NNUE evaluation

Both approaches assign a value to a position that is used in alpha-beta (PVS) search
to find the best move. The classical evaluation computes this value as a function
of various chess concepts, handcrafted by experts, tested and tuned using fishtest.
The NNUE evaluation computes this value with a neural network based on basic
inputs (e.g. piece positions only). The network is optimized and trained
on the evalutions of millions of positions at moderate search depth.

The NNUE evaluation was first introduced in shogi, and ported to DON afterward.
It can be evaluated efficiently on CPUs, and exploits the fact that only parts
of the neural network need to be updated after a typical chess move.
Website provides additional tools to train and develop the NNUE networks.

On CPUs supporting modern vector instructions (avx2 and similar), the NNUE evaluation
results in stronger playing strength, even if the nodes per second computed by the engine
is somewhat lower (roughly 60% of nps is typical).

Note that the NNUE evaluation depends on the DON binary and the network parameter
file (see EvalFile). Not every parameter file is compatible with a given DON binary.
The default value of the EvalFile UCI option is the name of a network that is guaranteed
to be compatible with that binary.

## What to expect from Syzygybases?

If the engine is searching a position that is not in the tablebases (e.g.
a position with 7 pieces), it will access the tablebases during the search.
If the engine reports a very large score (typically 123.xx), this means
that it has found a winning line into a tablebase position.

If the engine is given a position to search that is in the tablebases, it
will use the tablebases at the beginning of the search to preselect all
good moves, i.e. all moves that preserve the win or preserve the draw while
taking into account the 50-move rule.
It will then perform a search only on those moves. **The engine will not move
immediately**, unless there is only a single good move. **The engine likely
will not report a mate score even if the position is known to be won.**

It is therefore clear that behaviour is not identical to what one might
be used to with Nalimov tablebases. There are technical reasons for this
difference, the main technical reason being that Nalimov tablebases use the
DTM metric (distance-to-mate), while Syzygybases use a variation of the
DTZ metric (distance-to-zero, zero meaning any move that resets the 50-move
counter). This special metric is one of the reasons that Syzygybases are
more compact than Nalimov tablebases, while still storing all information
needed for optimal play and in addition being able to take into account
the 50-move rule.

## Compiling it yourself

DON has support for 32 or 64-bit CPUs, certain hardware
instructions, big-endian machines such as Power PC, and other platforms.

On Unix-like systems, it should be easy to compile DON
directly from the source code with the included Makefile in the folder `src`.
In general it is recommended to run `make help` to see a list of make
targets with corresponding descriptions.

```
    cd src
    make help
    make build ARCH=x86-64-abm
```

When not using the Makefile to compile (for instance with Microsoft MSVC) you
need to manually set/unset some switches in the compiler command line; see
file *types.h* for a quick reference.

When reporting an issue or a bug, please tell us which version and
compiler you used to create your executable. These informations can
be found by typing the following commands in a console:

```
    ./DON
    compiler
```

## Terms of use

DON is free, and distributed under the **GNU General Public License** (GPL).
Essentially, this means that you are free to do almost exactly what
you want with the program, including distributing it among your friends,
making it available for download from your web site, selling it
(either by itself or as part of some bigger software package), or
using it as the starting point for a software project of your own.

The only real limitation is that whenever you distribute DON in some way,
you must always include the full source code, or a pointer to where the
source code can be found. If you make any changes to the source code,
these changes must also be made available under the GPL.

For full details, read the copy of the GPL found in the file named *Copying.txt*
