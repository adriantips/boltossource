# Image seeds

Starting points for `test/t_fuzz.py`'s `image` harness, not crashers. Like
`../mp3`, every file here parses *correctly* — that is the whole point. A
mutator handed random bytes never invents a DEFLATE stream that inflates, an
LZW table that decodes or a Huffman table that builds, so it never reaches the
code where anything interesting lives. Saved crashers go in `test/corpus/image/`
instead, get replayed unmutated before every run, and have to keep passing.

All sixteen are the same 64×48 drawing — a dark ground, orange strokes, a
yellow ellipse — written out by Pillow in every format and depth the decoder
claims to handle. One drawing on purpose: the harness's selftest asserts that
the top-left pixel comes back as `#DC5028` from the formats that are lossless
and near it from the one that is not, so a decoder that returns something
plausible-but-wrong fails rather than passes.

| file | what it is there for |
|---|---|
| `png-rgb8.png` | the ordinary case, colour type 2 |
| `png-rgba8.png` | colour type 6, and the alpha channel |
| `png-gray8.png` | colour type 0 |
| `png-palette.png` | colour type 3 — Pillow packs this one at **2 bits** a pixel |
| `png-palette4.png` | 4 bits a pixel, the depth every 16-colour icon on the web is written at |
| `png-bilevel1.png` | 1 bit a pixel, where the grey has to be spread to 0..255 rather than shifted |
| `jpeg-420.jpg` | the common chroma layout, so the upsampler runs |
| `jpeg-444.jpg` | no subsampling, and a higher quality, so more coefficients survive |
| `jpeg-gray.jpg` | one component, where the colour conversion is skipped entirely |
| `gif-89a.gif` | LZW, and **interlaced** — Pillow writes it that way, which is how the de-interlace row map turned out to be filling half of itself |
| `gif-transparent.gif` | the graphic control extension and a transparent index |
| `bmp-24.bmp` / `bmp-32.bmp` | bottom-up rows, and the alpha byte |
| `tga-24.tga` | the dispatch fallback: no magic number, so every header field has to be checked |
| `tga-rle.tga` | run-length packets |
| `tga-gray.tga` | 8-bit, one byte a pixel |

They are marked `binary` in `.gitattributes` for the same reason the MP3 seeds
are: autocrlf rewriting a newline byte inside a DEFLATE stream produces a file
that no longer decodes, with nothing in the diff to say why.

Regenerating them needs Pillow, and the script is not kept — a seed corpus that
has to be reproducible byte for byte is a regression corpus and belongs in the
other directory.
