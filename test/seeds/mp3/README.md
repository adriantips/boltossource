# MP3 seeds

Starting points for a fuzzer, not crashers. Nothing here has ever broken
anything — these are files that parse *correctly*, which is the point: a
mutator handed random bytes never invents a valid Xing header or a plausible
`main_data_begin`, so it never reaches the code where the interesting bugs
live. Saved crashers belong in `test/corpus/`, get replayed unmutated before
every run, and have to keep passing; these have no such contract and can be
regenerated at will.

All six are synthetic audio encoded here with LAME. Between them they reach
branches of `kernel/mp3.c` that no single file does:

| file | what it is there for |
|---|---|
| `mpeg1-cbr.mp3` | the ordinary case, plus a fat ID3v2 tag in front — the tag skip is its own code path and is what anything off the internet looks like |
| `mpeg1-short.mp3` | clicks, so the encoder emits block types 1, 2 and 3: short blocks, the reordering pass, and the 12-point IMDCT |
| `mpeg1-mono.mp3` | one channel, where the side information is 17 bytes rather than 32 |
| `mpeg1-vbr-toc.mp3` | variable bitrate with a Xing header, frame count and 100-byte table of contents — the seek path reads that TOC |
| `mpeg2-lsf.mp3` | MPEG-2: one granule a frame, 9-bit `scalefac_compress`, and the scalefactor partition tables |
| `mpeg25-8khz.mp3` | MPEG-2.5 at a quarter rate, where the scalefactor bands widen and the short-block region boundary stops being 36 lines |

They are marked `binary` in `.gitattributes`. Without that, autocrlf rewrites
newline bytes *inside the frames* and the file decodes to noise, with nothing
visible in the diff to say why.

Regenerating them needs `lame`; the script that made them is not kept, because
a seed corpus that has to be reproducible byte for byte is a regression corpus
and belongs in the other directory.
