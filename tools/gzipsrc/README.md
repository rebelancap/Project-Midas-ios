# gzipsrc

This directory is [gzip](https://www.gnu.org/software/gzip/) 1.2.4 by
Jean-loup Gailly. It is only used to build the ROM compression tool and is
not part of the shipped game.

gzip is licensed under the GNU General Public License, version 2 or (at
your option) any later version; see [`COPYING`](COPYING). It is not covered
by the Project Midas MIT license.

`inflate.c` was written by Mark Adler and is not copyrighted. The game's own
decompressors (`src/inflate/inflate.c`, `src/game/zlib.c`) derive from that
file only, not from the GPL parts of gzip.
