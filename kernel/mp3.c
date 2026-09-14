/* ===========================================================================
 *  BoltOS  -  kernel/mp3.c
 *  MPEG-1/2/2.5 Layer III decoder. See mp3.h for the contract.
 *
 *  Layer III is six passes over 576 spectral lines, and every one of them has
 *  to be right before you hear anything but noise: Huffman decode, requantise,
 *  undo the stereo coding, put short blocks back in time order, undo the
 *  aliasing the encoder's filterbank left behind, an IMDCT per subband, and
 *  then the polyphase synthesis that turns 32 subbands back into 32 samples.
 *  It is the largest single piece of arithmetic in this kernel.
 *
 *  The constants below are the standard's, not this project's invention:
 *  Table B.7 is the Huffman codebook, B.8 the scalefactor band boundaries and
 *  B.3 the synthesis window. Everything else -- the IMDCT bases, the window
 *  shapes, the alias coefficients, the requantisation powers -- is generated
 *  at first use from sin/cos/pow, because a formula that fits on a line is
 *  worth more in a source file than 2000 printed decimals.
 *
 *  Floating point in the kernel is fine here: the build is -msse -msse2 and
 *  sched.c fxsaves the SSE state on every switch. Doubles are only used where
 *  libm hands one back.
 *
 *  Two things this decoder does not do, said out loud rather than discovered:
 *  free-format streams (no encoder has emitted one in twenty years) and MPEG-2
 *  intensity stereo. MPEG-1 intensity stereo is here; the LSF variant reads its
 *  positions out of a differently-shaped scalefactor block and no encoder on
 *  this machine can produce a file to test it against, so those bands decode as
 *  ordinary stereo rather than as code nobody has ever run.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "mp3.h"
#include "audio.h"
#include "kheap.h"
#include "string.h"
#include "math.h"

/* Huffman code tables, ISO 11172-3 Table B.7. Each is a complete
 * prefix code over (x,y) pairs -- or over the four-value quadruples,
 * for the two count1 tables at the end. Lengths and codes are held
 * apart so the tree builder can walk them without unpacking. */
static const uint8_t  hl1[] = {
     1,  3,  2,  3,
};
static const uint16_t hc1[] = {
        1,     1,     1,     0,
};
static const uint8_t  hl2[] = {
     1,  3,  6,  3,  3,  5,  5,  5,  6,
};
static const uint16_t hc2[] = {
        1,     2,     1,     3,     1,     1,     3,     2,     0,
};
static const uint8_t  hl3[] = {
     2,  2,  6,  3,  2,  5,  5,  5,  6,
};
static const uint16_t hc3[] = {
        3,     2,     1,     1,     1,     1,     3,     2,     0,
};
static const uint8_t  hl5[] = {
     1,  3,  6,  7,  3,  3,  6,  7,  6,  6,  7,  8,  7,  6,  7,  8,
};
static const uint16_t hc5[] = {
        1,     2,     6,     5,     3,     1,     4,     4,     7,     5,     7,     1,     6,     1,     1,     0,
};
static const uint8_t  hl6[] = {
     3,  3,  5,  7,  3,  2,  4,  5,  4,  4,  5,  6,  6,  5,  6,  7,
};
static const uint16_t hc6[] = {
        7,     3,     5,     1,     6,     2,     3,     2,     5,     4,     4,     1,     3,     3,     2,     0,
};
static const uint8_t  hl7[] = {
     1,  3,  6,  8,  8,  9,  3,  4,  6,  7,  7,  8,  6,  5,  7,  8,  8,  9,  7,  7,  8,  9,  9,  9,
     7,  7,  8,  9,  9, 10,  8,  8,  9, 10, 10, 10,
};
static const uint16_t hc7[] = {
        1,     2,    10,    19,    16,    10,     3,     3,     7,    10,     5,     3,    11,     4,    13,    17,
        8,     4,    12,    11,    18,    15,    11,     2,     7,     6,     9,    14,     3,     1,     6,     4,
        5,     3,     2,     0,
};
static const uint8_t  hl8[] = {
     2,  3,  6,  8,  8,  9,  3,  2,  4,  8,  8,  8,  6,  4,  6,  8,  8,  9,  8,  8,  8,  9,  9, 10,
     8,  7,  8,  9, 10, 10,  9,  8,  9,  9, 11, 11,
};
static const uint16_t hc8[] = {
        3,     4,     6,    18,    12,     5,     5,     1,     2,    16,     9,     3,     7,     3,     5,    14,
        7,     3,    19,    17,    15,    13,    10,     4,    13,     5,     8,    11,     5,     1,    12,     4,
        4,     1,     1,     0,
};
static const uint8_t  hl9[] = {
     3,  3,  5,  6,  8,  9,  3,  3,  4,  5,  6,  8,  4,  4,  5,  6,  7,  8,  6,  5,  6,  7,  7,  8,
     7,  6,  7,  7,  8,  9,  8,  7,  8,  8,  9,  9,
};
static const uint16_t hc9[] = {
        7,     5,     9,    14,    15,     7,     6,     4,     5,     5,     6,     7,     7,     6,     8,     8,
        8,     5,    15,     6,     9,    10,     5,     1,    11,     7,     9,     6,     4,     1,    14,     4,
        6,     2,     6,     0,
};
static const uint8_t  hl10[] = {
     1,  3,  6,  8,  9,  9,  9, 10,  3,  4,  6,  7,  8,  9,  8,  8,  6,  6,  7,  8,  9, 10,  9,  9,
     7,  7,  8,  9, 10, 10,  9, 10,  8,  8,  9, 10, 10, 10, 10, 10,  9,  9, 10, 10, 11, 11, 10, 11,
     8,  8,  9, 10, 10, 10, 11, 11,  9,  8,  9, 10, 10, 11, 11, 11,
};
static const uint16_t hc10[] = {
        1,     2,    10,    23,    35,    30,    12,    17,     3,     3,     8,    12,    18,    21,    12,     7,
       11,     9,    15,    21,    32,    40,    19,     6,    14,    13,    22,    34,    46,    23,    18,     7,
       20,    19,    33,    47,    27,    22,     9,     3,    31,    22,    41,    26,    21,    20,     5,     3,
       14,    13,    10,    11,    16,     6,     5,     1,     9,     8,     7,     8,     4,     4,     2,     0,
};
static const uint8_t  hl11[] = {
     2,  3,  5,  7,  8,  9,  8,  9,  3,  3,  4,  6,  8,  8,  7,  8,  5,  5,  6,  7,  8,  9,  8,  8,
     7,  6,  7,  9,  8, 10,  8,  9,  8,  8,  8,  9,  9, 10,  9, 10,  8,  8,  9, 10, 10, 11, 10, 11,
     8,  7,  7,  8,  9, 10, 10, 10,  8,  7,  8,  9, 10, 10, 10, 10,
};
static const uint16_t hc11[] = {
        3,     4,    10,    24,    34,    33,    21,    15,     5,     3,     4,    10,    32,    17,    11,    10,
       11,     7,    13,    18,    30,    31,    20,     5,    25,    11,    19,    59,    27,    18,    12,     5,
       35,    33,    31,    58,    30,    16,     7,     5,    28,    26,    32,    19,    17,    15,     8,    14,
       14,    12,     9,    13,    14,     9,     4,     1,    11,     4,     6,     6,     6,     3,     2,     0,
};
static const uint8_t  hl12[] = {
     4,  3,  5,  7,  8,  9,  9,  9,  3,  3,  4,  5,  7,  7,  8,  8,  5,  4,  5,  6,  7,  8,  7,  8,
     6,  5,  6,  6,  7,  8,  8,  8,  7,  6,  7,  7,  8,  8,  8,  9,  8,  7,  8,  8,  8,  9,  8,  9,
     8,  7,  7,  8,  8,  9,  9, 10,  9,  8,  8,  9,  9,  9,  9, 10,
};
static const uint16_t hc12[] = {
        9,     6,    16,    33,    41,    39,    38,    26,     7,     5,     6,     9,    23,    16,    26,    11,
       17,     7,    11,    14,    21,    30,    10,     7,    17,    10,    15,    12,    18,    28,    14,     5,
       32,    13,    22,    19,    18,    16,     9,     5,    40,    17,    31,    29,    17,    13,     4,     2,
       27,    12,    11,    15,    10,     7,     4,     1,    27,    12,     8,    12,     6,     3,     1,     0,
};
static const uint8_t  hl13[] = {
     1,  4,  6,  7,  8,  9,  9, 10,  9, 10, 11, 11, 12, 12, 13, 13,  3,  4,  6,  7,  8,  8,  9,  9,
     9,  9, 10, 10, 11, 12, 12, 12,  6,  6,  7,  8,  9,  9, 10, 10,  9, 10, 10, 11, 11, 12, 13, 13,
     7,  7,  8,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 13,  8,  7,  9,  9, 10, 10, 11, 11,
    10, 11, 11, 12, 12, 13, 13, 14,  9,  8,  9, 10, 10, 10, 11, 11, 11, 11, 12, 11, 13, 13, 14, 14,
     9,  9, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14, 10,  9, 10, 11, 11, 11, 12, 12,
    12, 12, 13, 13, 13, 14, 16, 16,  9,  8,  9, 10, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 15, 15,
    10,  9, 10, 10, 11, 11, 11, 13, 12, 13, 13, 14, 14, 14, 16, 15, 10, 10, 10, 11, 11, 12, 12, 13,
    12, 13, 14, 13, 14, 15, 16, 17, 11, 10, 10, 11, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 15, 16,
    11, 11, 11, 12, 12, 13, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16, 12, 11, 12, 13, 13, 13, 14, 14,
    14, 14, 14, 15, 16, 15, 16, 16, 13, 12, 12, 13, 13, 13, 15, 14, 14, 17, 15, 15, 15, 17, 16, 16,
    12, 12, 13, 14, 14, 14, 15, 14, 15, 15, 16, 16, 19, 18, 19, 16,
};
static const uint16_t hc13[] = {
        1,     5,    14,    21,    34,    51,    46,    71,    42,    52,    68,    52,    67,    44,    43,    19,
        3,     4,    12,    19,    31,    26,    44,    33,    31,    24,    32,    24,    31,    35,    22,    14,
       15,    13,    23,    36,    59,    49,    77,    65,    29,    40,    30,    40,    27,    33,    42,    16,
       22,    20,    37,    61,    56,    79,    73,    64,    43,    76,    56,    37,    26,    31,    25,    14,
       35,    16,    60,    57,    97,    75,   114,    91,    54,    73,    55,    41,    48,    53,    23,    24,
       58,    27,    50,    96,    76,    70,    93,    84,    77,    58,    79,    29,    74,    49,    41,    17,
       47,    45,    78,    74,   115,    94,    90,    79,    69,    83,    71,    50,    59,    38,    36,    15,
       72,    34,    56,    95,    92,    85,    91,    90,    86,    73,    77,    65,    51,    44,    43,    42,
       43,    20,    30,    44,    55,    78,    72,    87,    78,    61,    46,    54,    37,    30,    20,    16,
       53,    25,    41,    37,    44,    59,    54,    81,    66,    76,    57,    54,    37,    18,    39,    11,
       35,    33,    31,    57,    42,    82,    72,    80,    47,    58,    55,    21,    22,    26,    38,    22,
       53,    25,    23,    38,    70,    60,    51,    36,    55,    26,    34,    23,    27,    14,     9,     7,
       34,    32,    28,    39,    49,    75,    30,    52,    48,    40,    52,    28,    18,    17,     9,     5,
       45,    21,    34,    64,    56,    50,    49,    45,    31,    19,    12,    15,    10,     7,     6,     3,
       48,    23,    20,    39,    36,    35,    53,    21,    16,    23,    13,    10,     6,     1,     4,     2,
       16,    15,    17,    27,    25,    20,    29,    11,    17,    12,    16,     8,     1,     1,     0,     1,
};
static const uint8_t  hl15[] = {
     3,  4,  5,  7,  7,  8,  9,  9,  9, 10, 10, 11, 11, 11, 12, 13,  4,  3,  5,  6,  7,  7,  8,  8,
     8,  9,  9, 10, 10, 10, 11, 11,  5,  5,  5,  6,  7,  7,  8,  8,  8,  9,  9, 10, 10, 11, 11, 11,
     6,  6,  6,  7,  7,  8,  8,  9,  9,  9, 10, 10, 10, 11, 11, 11,  7,  6,  7,  7,  8,  8,  9,  9,
     9,  9, 10, 10, 10, 11, 11, 11,  8,  7,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 11, 11, 11, 12,
     9,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 11, 11, 12, 12,  9,  8,  8,  9,  9,  9,  9, 10,
    10, 10, 10, 10, 11, 11, 11, 12,  9,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11, 12, 12, 12,
     9,  8,  9,  9,  9,  9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 10,  9,  9,  9, 10, 10, 10, 10,
    10, 11, 11, 11, 11, 12, 13, 12, 10,  9,  9,  9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13,
    11, 10,  9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 13, 13, 11, 10, 10, 10, 10, 11, 11, 11,
    11, 12, 12, 12, 12, 12, 13, 13, 12, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 12, 13,
    12, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13,
};
static const uint16_t hc15[] = {
        7,    12,    18,    53,    47,    76,   124,   108,    89,   123,   108,   119,   107,    81,   122,    63,
       13,     5,    16,    27,    46,    36,    61,    51,    42,    70,    52,    83,    65,    41,    59,    36,
       19,    17,    15,    24,    41,    34,    59,    48,    40,    64,    50,    78,    62,    80,    56,    33,
       29,    28,    25,    43,    39,    63,    55,    93,    76,    59,    93,    72,    54,    75,    50,    29,
       52,    22,    42,    40,    67,    57,    95,    79,    72,    57,    89,    69,    49,    66,    46,    27,
       77,    37,    35,    66,    58,    52,    91,    74,    62,    48,    79,    63,    90,    62,    40,    38,
      125,    32,    60,    56,    50,    92,    78,    65,    55,    87,    71,    51,    73,    51,    70,    30,
      109,    53,    49,    94,    88,    75,    66,   122,    91,    73,    56,    42,    64,    44,    21,    25,
       90,    43,    41,    77,    73,    63,    56,    92,    77,    66,    47,    67,    48,    53,    36,    20,
       71,    34,    67,    60,    58,    49,    88,    76,    67,   106,    71,    54,    38,    39,    23,    15,
      109,    53,    51,    47,    90,    82,    58,    57,    48,    72,    57,    41,    23,    27,    62,     9,
       86,    42,    40,    37,    70,    64,    52,    43,    70,    55,    42,    25,    29,    18,    11,    11,
      118,    68,    30,    55,    50,    46,    74,    65,    49,    39,    24,    16,    22,    13,    14,     7,
       91,    44,    39,    38,    34,    63,    52,    45,    31,    52,    28,    19,    14,     8,     9,     3,
      123,    60,    58,    53,    47,    43,    32,    22,    37,    24,    17,    12,    15,    10,     2,     1,
       71,    37,    34,    30,    28,    20,    17,    26,    21,    16,    10,     6,     8,     6,     2,     0,
};
static const uint8_t  hl16[] = {
     1,  4,  6,  8,  9,  9, 10, 10, 11, 11, 11, 12, 12, 12, 13,  9,  3,  4,  6,  7,  8,  9,  9,  9,
    10, 10, 10, 11, 12, 11, 12,  8,  6,  6,  7,  8,  9,  9, 10, 10, 11, 10, 11, 11, 11, 12, 12,  9,
     8,  7,  8,  9,  9, 10, 10, 10, 11, 11, 12, 12, 12, 13, 13, 10,  9,  8,  9,  9, 10, 10, 11, 11,
    11, 12, 12, 12, 13, 13, 13,  9,  9,  8,  9,  9, 10, 11, 11, 12, 11, 12, 12, 13, 13, 13, 14, 10,
    10,  9,  9, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 14, 10, 10,  9, 10, 10, 11, 11, 11, 12,
    12, 13, 13, 13, 13, 15, 15, 10, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 10,
    11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 13, 14, 13, 11, 11, 11, 10, 11, 12, 12, 12, 12,
    13, 14, 14, 14, 15, 15, 14, 10, 12, 11, 11, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 13, 14, 11,
    12, 12, 12, 12, 12, 13, 13, 13, 13, 15, 14, 14, 14, 14, 16, 11, 14, 12, 12, 12, 13, 13, 14, 14,
    14, 16, 15, 15, 15, 17, 15, 11, 13, 13, 11, 12, 14, 14, 13, 14, 14, 15, 16, 15, 17, 15, 14, 11,
     9,  8,  8,  9,  9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11,  8,
};
static const uint16_t hc16[] = {
        1,     5,    14,    44,    74,    63,   110,    93,   172,   149,   138,   242,   225,   195,   376,    17,
        3,     4,    12,    20,    35,    62,    53,    47,    83,    75,    68,   119,   201,   107,   207,     9,
       15,    13,    23,    38,    67,    58,   103,    90,   161,    72,   127,   117,   110,   209,   206,    16,
       45,    21,    39,    69,    64,   114,    99,    87,   158,   140,   252,   212,   199,   387,   365,    26,
       75,    36,    68,    65,   115,   101,   179,   164,   155,   264,   246,   226,   395,   382,   362,     9,
       66,    30,    59,    56,   102,   185,   173,   265,   142,   253,   232,   400,   388,   378,   445,    16,
      111,    54,    52,   100,   184,   178,   160,   133,   257,   244,   228,   217,   385,   366,   715,    10,
       98,    48,    91,    88,   165,   157,   148,   261,   248,   407,   397,   372,   380,   889,   884,     8,
       85,    84,    81,   159,   156,   143,   260,   249,   427,   401,   392,   383,   727,   713,   708,     7,
      154,    76,    73,   141,   131,   256,   245,   426,   406,   394,   384,   735,   359,   710,   352,    11,
      139,   129,    67,   125,   247,   233,   229,   219,   393,   743,   737,   720,   885,   882,   439,     4,
      243,   120,   118,   115,   227,   223,   396,   746,   742,   736,   721,   712,   706,   223,   436,     6,
      202,   224,   222,   218,   216,   389,   386,   381,   364,   888,   443,   707,   440,   437,  1728,     4,
      747,   211,   210,   208,   370,   379,   734,   723,   714,  1735,   883,   877,   876,  3459,   865,     2,
      377,   369,   102,   187,   726,   722,   358,   711,   709,   866,  1734,   871,  3458,   870,   434,     0,
       12,    10,     7,    11,    10,    17,    11,     9,    13,    12,    10,     7,     5,     3,     1,     3,
};
static const uint8_t  hl24[] = {
     4,  4,  6,  7,  8,  9,  9, 10, 10, 11, 11, 11, 11, 11, 12,  9,  4,  4,  5,  6,  7,  8,  8,  9,
     9,  9, 10, 10, 10, 10, 10,  8,  6,  5,  6,  7,  7,  8,  8,  9,  9,  9,  9, 10, 10, 10, 11,  7,
     7,  6,  7,  7,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10,  7,  8,  7,  7,  8,  8,  8,  8,  9,
     9,  9, 10, 10, 10, 10, 11,  7,  9,  7,  8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10,  7,
     9,  8,  8,  8,  8,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11,  7, 10,  8,  8,  8,  9,  9,  9,  9,
    10, 10, 10, 10, 10, 11, 11,  8, 10,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 11, 11,  8,
    10,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10, 11, 11, 11,  8, 11,  9,  9,  9,  9, 10, 10, 10,
    10, 10, 10, 11, 11, 11, 11,  8, 11, 10,  9,  9,  9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11,  8,
    11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11,  8, 11, 10, 10, 10, 10, 10, 10, 10,
    11, 11, 11, 11, 11, 11, 11,  8, 12, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11,  8,
     8,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  4,
};
static const uint16_t hc24[] = {
       15,    13,    46,    80,   146,   262,   248,   434,   426,   669,   653,   649,   621,   517,  1032,    88,
       14,    12,    21,    38,    71,   130,   122,   216,   209,   198,   327,   345,   319,   297,   279,    42,
       47,    22,    41,    74,    68,   128,   120,   221,   207,   194,   182,   340,   315,   295,   541,    18,
       81,    39,    75,    70,   134,   125,   116,   220,   204,   190,   178,   325,   311,   293,   271,    16,
      147,    72,    69,   135,   127,   118,   112,   210,   200,   188,   352,   323,   306,   285,   540,    14,
      263,    66,   129,   126,   119,   114,   214,   202,   192,   180,   341,   317,   301,   281,   262,    12,
      249,   123,   121,   117,   113,   215,   206,   195,   185,   347,   330,   308,   291,   272,   520,    10,
      435,   115,   111,   109,   211,   203,   196,   187,   353,   332,   313,   298,   283,   531,   381,    17,
      427,   212,   208,   205,   201,   193,   186,   177,   169,   320,   303,   286,   268,   514,   377,    16,
      335,   199,   197,   191,   189,   181,   174,   333,   321,   305,   289,   275,   521,   379,   371,    11,
      668,   184,   183,   179,   175,   344,   331,   314,   304,   290,   277,   530,   383,   373,   366,    10,
      652,   346,   171,   168,   164,   318,   309,   299,   287,   276,   263,   513,   375,   368,   362,     6,
      648,   322,   316,   312,   307,   302,   292,   284,   269,   261,   512,   376,   370,   364,   359,     4,
      620,   300,   296,   294,   288,   282,   273,   266,   515,   380,   374,   369,   365,   361,   357,     2,
     1033,   280,   278,   274,   267,   264,   259,   382,   378,   372,   367,   363,   360,   358,   356,     0,
       43,    20,    19,    17,    15,    13,    11,     9,     7,     6,     4,     7,     5,     3,     1,     3,
};
static const uint8_t  hl32[] = {
     1,  4,  4,  5,  4,  6,  5,  6,  4,  5,  5,  6,  5,  6,  6,  6,
};
static const uint16_t hc32[] = {
        1,     5,     4,     5,     6,     5,     4,     4,     7,     3,     6,     0,     7,     2,     3,     1,
};
static const uint8_t  hl33[] = {
     4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
};
static const uint16_t hc33[] = {
       15,    14,    13,    12,    11,    10,     9,     8,     7,     6,     5,     4,     3,     2,     1,     0,
};

#define HTAB(n) hl##n, hc##n, (uint16_t)(sizeof hl##n)

static const struct { const uint8_t *len; const uint16_t *cod;
                     uint16_t n; uint8_t side; uint8_t linbits; } htab[34] = {
    { 0, 0, 0, 0, 0 },        /* 0: not used by any encoder */
    { HTAB(1), 2, 0 },
    { HTAB(2), 3, 0 },
    { HTAB(3), 3, 0 },
    { 0, 0, 0, 0, 0 },        /* 4: not used by any encoder */
    { HTAB(5), 4, 0 },
    { HTAB(6), 4, 0 },
    { HTAB(7), 6, 0 },
    { HTAB(8), 6, 0 },
    { HTAB(9), 6, 0 },
    { HTAB(10), 8, 0 },
    { HTAB(11), 8, 0 },
    { HTAB(12), 8, 0 },
    { HTAB(13), 16, 0 },
    { 0, 0, 0, 0, 0 },        /* 14: not used by any encoder */
    { HTAB(15), 16, 0 },
    { HTAB(16), 16, 1 },
    { HTAB(16), 16, 2 },
    { HTAB(16), 16, 3 },
    { HTAB(16), 16, 4 },
    { HTAB(16), 16, 6 },
    { HTAB(16), 16, 8 },
    { HTAB(16), 16, 10 },
    { HTAB(16), 16, 13 },
    { HTAB(24), 16, 4 },
    { HTAB(24), 16, 5 },
    { HTAB(24), 16, 6 },
    { HTAB(24), 16, 7 },
    { HTAB(24), 16, 8 },
    { HTAB(24), 16, 9 },
    { HTAB(24), 16, 11 },
    { HTAB(24), 16, 13 },
    /* The count1 codebooks. They are indexed by a four-bit mask rather than by
     * an (x,y) pair, so `side` means nothing here and nothing reads it. */
    { HTAB(32), 0, 0 },
    { HTAB(33), 0, 0 },
};

/* Scalefactor band boundaries, ISO 11172-3 Table B.8, in the sample-rate
 * order the frame header uses: 44100/48000/32000, then the MPEG-2 halves
 * and the MPEG-2.5 quarters of each. The short table is per window, so a
 * short band covers three times as many lines as its width suggests. */
static const uint16_t sfb_long[9][23] = {
    { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576 },
    { 0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576 },
    { 0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576 },
    { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 },
    { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576 },
    { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 },
    { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 },
    { 0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576 },
    { 0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576 },
};
static const uint16_t sfb_short[9][14] = {
    { 0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192 },
    { 0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192 },
    { 0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192 },
    { 0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192 },
    { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192 },
    { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192 },
    { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192 },
    { 0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192 },
    { 0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192 },
};

/* Synthesis window, ISO 11172-3 Table B.3, first half. Every value in
 * that table is an exact multiple of 1/65536, so it is kept as the
 * integer and divided once at startup. The second half is the mirror. */
static const int32_t winbase[257] = {
          0,      -1,      -1,      -1,      -1,      -1,      -1,      -2,      -2,      -2,
         -2,      -3,      -3,      -4,      -4,      -5,      -5,      -6,      -7,      -7,
         -8,      -9,     -10,     -11,     -13,     -14,     -16,     -17,     -19,     -21,
        -24,     -26,     -29,     -31,     -35,     -38,     -41,     -45,     -49,     -53,
        -58,     -63,     -68,     -73,     -79,     -85,     -91,     -97,    -104,    -111,
       -117,    -125,    -132,    -139,    -147,    -154,    -161,    -169,    -176,    -183,
       -190,    -196,    -202,    -208,    -213,    -218,    -222,    -225,    -227,    -228,
       -228,    -227,    -224,    -221,    -215,    -208,    -200,    -189,    -177,    -163,
       -146,    -127,    -106,     -83,     -57,     -29,       2,      36,      72,     111,
        153,     197,     244,     294,     347,     401,     459,     519,     581,     645,
        711,     779,     848,     919,     991,    1064,    1137,    1210,    1283,    1356,
       1428,    1498,    1567,    1634,    1698,    1759,    1817,    1870,    1919,    1962,
       2001,    2032,    2057,    2075,    2085,    2087,    2080,    2063,    2037,    2000,
       1952,    1893,    1822,    1739,    1644,    1535,    1414,    1280,    1131,     970,
        794,     605,     402,     185,     -45,    -288,    -545,    -814,   -1095,   -1388,
      -1692,   -2006,   -2330,   -2663,   -3004,   -3351,   -3705,   -4063,   -4425,   -4788,
      -5153,   -5517,   -5879,   -6237,   -6589,   -6935,   -7271,   -7597,   -7910,   -8209,
      -8491,   -8755,   -8998,   -9219,   -9416,   -9585,   -9727,   -9838,   -9916,   -9959,
      -9966,   -9935,   -9863,   -9750,   -9592,   -9389,   -9139,   -8840,   -8492,   -8092,
      -7640,   -7134,   -6574,   -5959,   -5288,   -4561,   -3776,   -2935,   -2037,   -1082,
        -70,     998,    2122,    3300,    4533,    5818,    7154,    8540,    9975,   11455,
      12980,   14548,   16155,   17799,   19478,   21189,   22929,   24694,   26482,   28289,
      30112,   31947,   33791,   35640,   37489,   39336,   41176,   43006,   44821,   46617,
      48390,   50137,   51853,   53534,   55178,   56778,   58333,   59838,   61289,   62684,
      64019,   65290,   66494,   67629,   68692,   69679,   70590,   71420,   72169,   72835,
      73415,   73908,   74313,   74630,   74856,   74992,   75038,
};

/* ---------------------------------------------------------------------------
 *  Tables built once, the first time anything opens an MP3.
 * ------------------------------------------------------------------------*/

#define TREE_NODES 3072

static int16_t  tree[TREE_NODES][2];    /* >=0 child, <0 leaf = -(symbol+1) */
static int16_t  root[34];               /* per table number, -1 if unused   */
static uint16_t tree_used;

static float dwin[512];                 /* synthesis window, Table B.3      */
static float smat[64][32];              /* the 64x32 matrixing              */
static float imdct36[36][18];
static float imdct12[12][6];
static float bwin[4][36];               /* the four block window shapes     */
static float alias_ca[8], alias_cs[8];
static float pow43[256];                /* |is|^(4/3) for the common range  */
static float is_ratio[2][8];            /* MPEG-1 intensity stereo weights  */
static int   tables_ready;

/* Preemphasis, Table B.6. Only applied when a granule sets preflag. */
static const uint8_t pretab[22] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0
};

/* Scalefactor field widths, Table B.4, indexed by scalefac_compress. */
static const uint8_t slen1[16] = { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 };
static const uint8_t slen2[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 };

/* How many scalefactor bands each of the four MPEG-2 partitions covers, by
 * the slen index the scalefac_compress arithmetic lands on and by block
 * shape (long / short / mixed). Table B.5's layout, one row per index. */
static const uint8_t lsf_nr[6][3][4] = {
    { {  6,  5,  5, 5 }, {  9,  9,  9, 9 }, {  6,  9,  9, 9 } },
    { {  6,  5,  7, 3 }, {  9,  9, 12, 6 }, {  6,  9, 12, 6 } },
    { { 11, 10,  0, 0 }, { 18, 18,  0, 0 }, { 15, 18,  0, 0 } },
    { {  7,  7,  7, 0 }, { 12, 12, 12, 0 }, {  6, 15, 12, 0 } },
    { {  6,  6,  6, 3 }, { 12,  9,  9, 6 }, {  6, 12,  9, 6 } },
    { {  8,  8,  5, 0 }, { 15, 12,  9, 0 }, {  6, 18,  9, 0 } }
};

static const uint16_t bitrate_tab[2][15] = {
    /* MPEG-1 Layer III */
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },
    /* MPEG-2 and 2.5 Layer III */
    { 0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160 }
};
static const uint16_t rate_tab[3] = { 44100, 48000, 32000 };

static int tree_insert(int r, uint32_t code, int len, int sym) {
    int at = r;
    for (int b = len - 1; b >= 0; b--) {
        int dir = (int)((code >> b) & 1);
        if (b == 0) { tree[at][dir] = (int16_t)(-(sym + 1)); return 0; }
        if (tree[at][dir] == 0) {
            if (tree_used >= TREE_NODES) return -1;
            tree[at][dir] = (int16_t)tree_used;
            tree[tree_used][0] = tree[tree_used][1] = 0;
            tree_used++;
        }
        if (tree[at][dir] < 0) return -1;        /* a code is another's prefix */
        at = tree[at][dir];
    }
    return 0;
}

static void build_trees(void) {
    for (int i = 0; i < 34; i++) root[i] = -1;
    tree_used = 0;
    for (int t = 0; t < 34; t++) {
        if (!htab[t].n || root[t] >= 0) continue;
        /* 16..23 share one codebook and 24..31 another; only the linbits
         * differ, so the tree is built once and pointed at eight times. */
        int shared = -1;
        for (int u = 0; u < t; u++)
            if (htab[u].cod == htab[t].cod) { shared = root[u]; break; }
        if (shared >= 0) { root[t] = (int16_t)shared; continue; }

        int r = (int)tree_used;
        tree[r][0] = tree[r][1] = 0;
        tree_used++;
        for (int s = 0; s < htab[t].n; s++)
            tree_insert(r, htab[t].cod[s], htab[t].len[s], s);
        root[t] = (int16_t)r;
        for (int u = t + 1; u < 34; u++)
            if (htab[u].cod == htab[t].cod) root[u] = (int16_t)r;
    }
}

/* The synthesis window, unfolded from the 257 values of the prototype.
 *
 * D is a 512-tap filter, symmetric about its centre, and every value in Table
 * B.3 is an exact multiple of 1/65536 -- which is why the prototype is kept as
 * integers and divided once. All the table adds to the prototype is a sign that
 * flips every 32 taps in a period of four: D[d] = +/- P[d], negative when
 * (d / 32) mod 4 is 2 or 3, with P mirrored about 512.
 *
 * Table B.3 is *printed* as 32 rows of 16, and printed entry (r, c) is D[32c+r]
 * -- down the columns, not along the rows. Reading it the other way builds a
 * window whose peak lands on every sixteenth tap instead of in the middle, so
 * one output sample in sixteen comes out enormous and the rest come out as
 * nothing. It sounds like a loud buzz and it looks, in a dump, exactly like the
 * window shape, which is the clue: out[j] had been reduced to one term. */
static void build_window(void) {
    for (int d = 0; d < 512; d++) {
        int32_t v = winbase[d <= 256 ? d : 512 - d];
        dwin[d] = (float)(((d >> 5) & 2) ? -v : v) / 65536.0f;
    }
}

static void build_tables(void) {
    if (tables_ready) return;

    build_trees();
    build_window();

    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            smat[i][k] = (float)cos((16 + i) * (2 * k + 1) * 3.14159265358979323846 / 64.0);

    for (int i = 0; i < 36; i++)
        for (int k = 0; k < 18; k++)
            imdct36[i][k] = (float)cos(3.14159265358979323846 / 72.0 *
                                       (2 * i + 19) * (2 * k + 1));
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++)
            imdct12[i][k] = (float)cos(3.14159265358979323846 / 24.0 *
                                       (2 * i + 7) * (2 * k + 1));

    const double PI = 3.14159265358979323846;
    for (int i = 0; i < 36; i++) {
        double n = sin(PI / 36.0 * (i + 0.5));
        bwin[0][i] = (float)n;                       /* normal        */
        bwin[1][i] = (float)n;                       /* start, fixed up below */
        bwin[3][i] = (float)n;                       /* stop,  likewise       */
        bwin[2][i] = 0.0f;                           /* short: unused here    */
    }
    for (int i = 18; i < 24; i++) bwin[1][i] = 1.0f;
    for (int i = 24; i < 30; i++) bwin[1][i] = (float)sin(PI / 12.0 * (i - 18 + 0.5));
    for (int i = 30; i < 36; i++) bwin[1][i] = 0.0f;
    for (int i = 0; i < 6; i++)   bwin[3][i] = 0.0f;
    for (int i = 6; i < 12; i++)  bwin[3][i] = (float)sin(PI / 12.0 * (i - 6 + 0.5));
    for (int i = 12; i < 18; i++) bwin[3][i] = 1.0f;
    /* The short window is the same twelve-point sine three times over; it is
     * applied inside the short IMDCT rather than across the 36 samples. */
    for (int i = 0; i < 12; i++) bwin[2][i] = (float)sin(PI / 12.0 * (i + 0.5));

    /* Alias reduction, Table B.9: eight butterflies either side of each
     * subband boundary, with cs/ca the normalised pair of each ci. */
    static const double ci[8] = { -0.6, -0.535, -0.33, -0.185,
                                  -0.095, -0.041, -0.0142, -0.0037 };
    for (int i = 0; i < 8; i++) {
        double d = sqrt(1.0 + ci[i] * ci[i]);
        alias_cs[i] = (float)(1.0 / d);
        alias_ca[i] = (float)(ci[i] / d);
    }

    for (int i = 0; i < 256; i++)
        pow43[i] = (float)(i * cbrt((double)i));

    /* MPEG-1 intensity stereo: the right channel's scalefactor is an angle,
     * and the two weights are its tangent normalised so they sum to one.
     * Position 7 is the illegal one and is left alone by the caller. */
    for (int p = 0; p < 8; p++) {
        double t = tan(p * PI / 12.0);
        is_ratio[0][p] = (float)(t / (1.0 + t));     /* left  */
        is_ratio[1][p] = (float)(1.0 / (1.0 + t));   /* right */
    }

    tables_ready = 1;
}

/* 2^(e/4), with e in quarter-steps of the exponent. Everything the standard
 * asks for -- global_gain, subblock_gain, the scalefactors -- lands in these
 * units, so the whole requantisation exponent is one integer. */
static float exp4(int e) {
    static const float frac[4] = { 1.0f, 1.18920712f, 1.41421356f, 1.68179283f };
    int q = e >> 2;                    /* arithmetic: floors, which is wanted */
    if (q < -140) return 0.0f;
    if (q > 120) q = 120;
    return (float)ldexp((double)frac[e & 3], q);
}

static float pow43_of(int v) {
    if (v < 256) return pow43[v];
    return (float)(v * cbrt((double)v));
}

/* ---------------------------------------------------------------------------
 *  Bit reader. Reading past the end returns zeros and raises `over`, because a
 *  truncated frame is a thing that happens to files and must not be a thing
 *  that happens to memory.
 * ------------------------------------------------------------------------*/
typedef struct {
    const uint8_t *p;
    uint32_t bytes;
    uint32_t bit;
    int      over;
} br_t;

static void br_init(br_t *b, const uint8_t *p, uint32_t bytes) {
    b->p = p; b->bytes = bytes; b->bit = 0; b->over = 0;
}

static uint32_t br_get(br_t *b, int n) {
    uint32_t v = 0;
    while (n-- > 0) {
        uint32_t byte = b->bit >> 3;
        uint32_t got;
        if (byte >= b->bytes) { b->over = 1; got = 0; }
        else got = (b->p[byte] >> (7 - (b->bit & 7))) & 1;
        v = (v << 1) | got;
        b->bit++;
    }
    return v;
}

static uint32_t br_bit(br_t *b) {
    uint32_t byte = b->bit >> 3;
    uint32_t got;
    if (byte >= b->bytes) { b->over = 1; got = 0; }
    else got = (b->p[byte] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return got;
}

/* ---------------------------------------------------------------------------
 *  Frame header and side information.
 * ------------------------------------------------------------------------*/

typedef struct {
    int ver;           /* 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5 */
    int lsf;           /* 1 when there is one granule of 576 instead of two */
    int rate_idx;      /* 0..8 into the scalefactor band tables */
    int rate;
    int kbps;
    int chans;
    int mode, mode_ext;
    int crc;
    uint32_t bytes;    /* whole frame including the header */
    uint32_t side;     /* side information length */
} hdr_t;

static int parse_header(const uint8_t *p, uint32_t avail, hdr_t *h) {
    if (avail < 4) return -1;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;

    int ver = (p[1] >> 3) & 3;
    int layer = (p[1] >> 1) & 3;
    if (ver == 1 || layer != 1) return -1;          /* reserved, or not Layer III */

    int br_idx = (p[2] >> 4) & 15;
    int sr_idx = (p[2] >> 2) & 3;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) return -1;

    h->ver = ver;
    h->lsf = (ver != 3);
    h->crc = !((p[1] >> 0) & 1);
    h->kbps = bitrate_tab[h->lsf][br_idx];
    h->rate = rate_tab[sr_idx] >> (ver == 3 ? 0 : (ver == 2 ? 1 : 2));
    h->rate_idx = sr_idx + (ver == 3 ? 0 : (ver == 2 ? 3 : 6));
    h->mode = (p[3] >> 6) & 3;
    h->mode_ext = (p[3] >> 4) & 3;
    h->chans = (h->mode == 3) ? 1 : 2;

    int pad = (p[2] >> 1) & 1;
    /* 1152 samples a frame on MPEG-1, 576 on the low-sampling-rate versions,
     * which is the whole of why the byte count halves. */
    h->bytes = (uint32_t)((h->lsf ? 72000 : 144000) * h->kbps / h->rate) + (uint32_t)pad;
    if (h->bytes < 24 || h->bytes > avail) return -1;

    if (h->lsf) h->side = (h->chans == 1) ? 9u : 17u;
    else        h->side = (h->chans == 1) ? 17u : 32u;
    if (4 + (h->crc ? 2u : 0u) + h->side > h->bytes) return -1;
    return 0;
}

typedef struct {
    uint16_t part2_3_length;
    uint16_t big_values;
    uint16_t scalefac_compress;
    uint8_t  global_gain;
    uint8_t  window_switching;
    uint8_t  block_type;
    uint8_t  mixed;
    uint8_t  table_select[3];
    uint8_t  subblock_gain[3];
    uint8_t  region0_count, region1_count;
    uint8_t  preflag, scalefac_scale, count1table;
} gr_t;

typedef struct {
    uint16_t main_data_begin;
    uint8_t  scfsi[2][4];
    gr_t     gr[2][2];              /* [granule][channel] */
} side_t;

static void parse_side(br_t *b, const hdr_t *h, side_t *s) {
    int ngr = h->lsf ? 1 : 2;
    memset(s, 0, sizeof *s);

    if (h->lsf) {
        s->main_data_begin = (uint16_t)br_get(b, 8);
        br_get(b, h->chans == 1 ? 1 : 2);
    } else {
        s->main_data_begin = (uint16_t)br_get(b, 9);
        br_get(b, h->chans == 1 ? 5 : 3);
        for (int ch = 0; ch < h->chans; ch++)
            for (int i = 0; i < 4; i++) s->scfsi[ch][i] = (uint8_t)br_bit(b);
    }

    for (int gr = 0; gr < ngr; gr++) {
        for (int ch = 0; ch < h->chans; ch++) {
            gr_t *g = &s->gr[gr][ch];
            g->part2_3_length = (uint16_t)br_get(b, 12);
            g->big_values     = (uint16_t)br_get(b, 9);
            g->global_gain    = (uint8_t)br_get(b, 8);
            g->scalefac_compress = (uint16_t)br_get(b, h->lsf ? 9 : 4);
            g->window_switching  = (uint8_t)br_bit(b);
            if (g->window_switching) {
                g->block_type = (uint8_t)br_get(b, 2);
                g->mixed      = (uint8_t)br_bit(b);
                g->table_select[0] = (uint8_t)br_get(b, 5);
                g->table_select[1] = (uint8_t)br_get(b, 5);
                for (int i = 0; i < 3; i++) g->subblock_gain[i] = (uint8_t)br_get(b, 3);
                /* The fields that would carry the region split are not in the
                 * stream for a switched granule; the standard fixes them. */
                g->region0_count = 7;
                g->region1_count = 13;
            } else {
                for (int i = 0; i < 3; i++) g->table_select[i] = (uint8_t)br_get(b, 5);
                g->region0_count = (uint8_t)br_get(b, 4);
                g->region1_count = (uint8_t)br_get(b, 3);
            }
            if (!h->lsf) g->preflag = (uint8_t)br_bit(b);
            g->scalefac_scale = (uint8_t)br_bit(b);
            g->count1table    = (uint8_t)br_bit(b);
            if (g->big_values > 288) g->big_values = 288;
        }
    }
}

/* ---------------------------------------------------------------------------
 *  The decoder proper.
 * ------------------------------------------------------------------------*/

#define RESERVOIR 511
#define MAINMAX   2048

struct mp3 {
    const uint8_t *file;
    uint32_t len;
    uint32_t first;              /* byte offset of the first frame        */
    uint32_t pos;                /* byte offset of the next frame         */
    uint32_t end;                /* one past the last audio byte          */

    hdr_t h;                     /* the first frame's shape               */
    hdr_t cur;                   /* and this frame's, which is not the same
                                  * thing: an encoder picks mid/side per
                                  * frame, so mode_ext changes underneath a
                                  * decoder that only read the first header */
    uint32_t nframes;            /* counted, or taken from a VBR header   */
    uint32_t vbr_bytes;
    uint8_t  toc[100];
    int      have_toc;

    /* bit reservoir: main data spilling backwards out of earlier frames */
    uint8_t  res[RESERVOIR + MAINMAX];
    uint32_t res_len;

    /* persistent per-channel filter state */
    float overlap[2][32][18];
    float V[2][1024];
    int   voff[2];

    int scalefac_l[2][23];
    int scalefac_s[2][13][3];

    /* Scratch for the frame being decoded. These were function-level statics
     * until it was pointed out that two threads reach this at once: the
     * compositor calls media_pump() while a blocking `play` decodes in the
     * shell's thread, and the preempt hook only stops the *stream*, after the
     * decode has already happened. Two open files would have shared them in
     * any case. Nine kilobytes to not have to think about it again. */
    int   is[2][576];
    float xr[2][576];
    float sb[32];

    /* one decoded MPEG frame, at the file's own rate. Slot 0 carries the last
     * sample of the previous frame so the resampler can interpolate across the
     * seam; the stream therefore comes out one sample late, forever, which
     * nobody can hear and which costs nothing to keep consistent. */
    int16_t  nat[(1152 + 1) * 2];
    uint32_t nat_frames;
    uint64_t posf;               /* 16.16 cursor into nat, in source frames */
    uint64_t step;               /* source frames per output frame, 16.16   */
    uint64_t out_done;           /* output frames handed out so far         */
    int      started;
    int      ended;
    char     desc[80];
};

/* ---- scalefactors -------------------------------------------------------- */

static void read_scalefac_mpeg1(mp3_t *m, br_t *b, const side_t *s,
                                int gr, int ch, const gr_t *g) {
    int s1 = slen1[g->scalefac_compress & 15];
    int s2 = slen2[g->scalefac_compress & 15];

    if (g->window_switching && g->block_type == 2) {
        if (g->mixed) {
            for (int sfb = 0; sfb < 8; sfb++)
                m->scalefac_l[ch][sfb] = (int)br_get(b, s1);
            for (int sfb = 3; sfb < 12; sfb++) {
                int n = sfb < 6 ? s1 : s2;
                for (int w = 0; w < 3; w++) m->scalefac_s[ch][sfb][w] = (int)br_get(b, n);
            }
        } else {
            for (int sfb = 0; sfb < 12; sfb++) {
                int n = sfb < 6 ? s1 : s2;
                for (int w = 0; w < 3; w++) m->scalefac_s[ch][sfb][w] = (int)br_get(b, n);
            }
        }
        /* Twelve bands are transmitted and thirteen exist; the last one's
         * scalefactor is zero by definition. Leaving it alone instead means the
         * top third of a short granule is requantised with whatever the
         * previous short granule put there, which is silent on steady material
         * and audibly wrong on exactly the transients short blocks are for. */
        for (int w = 0; w < 3; w++) m->scalefac_s[ch][12][w] = 0;
        return;
    }

    /* Long blocks, where scfsi lets granule 1 inherit granule 0's values in
     * four groups. Inheriting is the common case on steady material. */
    static const int grp[4][2] = { { 0, 6 }, { 6, 11 }, { 11, 16 }, { 16, 21 } };
    for (int k = 0; k < 4; k++) {
        if (gr == 1 && s->scfsi[ch][k]) continue;
        int n = k < 2 ? s1 : s2;
        for (int sfb = grp[k][0]; sfb < grp[k][1]; sfb++)
            m->scalefac_l[ch][sfb] = (int)br_get(b, n);
    }
    m->scalefac_l[ch][21] = 0;
    m->scalefac_l[ch][22] = 0;
}

static void read_scalefac_lsf(mp3_t *m, br_t *b, int ch, const gr_t *g, int is_right) {
    int slen[4], index, sc = g->scalefac_compress;

    if (!is_right) {
        if (sc < 400) {
            slen[0] = (sc >> 4) / 5;  slen[1] = (sc >> 4) % 5;
            slen[2] = (sc & 15) >> 2; slen[3] = sc & 3;
            index = 0;
        } else if (sc < 500) {
            sc -= 400;
            slen[0] = (sc >> 2) / 5;  slen[1] = (sc >> 2) % 5;
            slen[2] = sc & 3;         slen[3] = 0;
            index = 1;
        } else {
            sc -= 500;
            slen[0] = sc / 3; slen[1] = sc % 3; slen[2] = 0; slen[3] = 0;
            index = 2;
        }
    } else {
        sc >>= 1;
        if (sc < 180) {
            slen[0] = sc / 36; slen[1] = (sc % 36) / 6; slen[2] = sc % 6; slen[3] = 0;
            index = 3;
        } else if (sc < 244) {
            sc -= 180;
            slen[0] = (sc & 63) >> 4; slen[1] = (sc & 15) >> 2; slen[2] = sc & 3; slen[3] = 0;
            index = 4;
        } else {
            sc -= 244;
            slen[0] = sc / 3; slen[1] = sc % 3; slen[2] = 0; slen[3] = 0;
            index = 5;
        }
    }

    int shape = (g->block_type == 2) ? (g->mixed ? 2 : 1) : 0;
    const uint8_t *nr = lsf_nr[index][shape];

    memset(m->scalefac_l[ch], 0, sizeof m->scalefac_l[ch]);
    memset(m->scalefac_s[ch], 0, sizeof m->scalefac_s[ch]);

    if (g->block_type == 2) {
        /* The partition sizes count scalefactors, not bands -- a short granule
         * has twelve bands times three windows and the four partitions of
         * {9,9,9,9} add up to those 36, not to 36 bands. Reading them as bands
         * takes three times too many bits, which the count1 region then hides
         * by filling whatever is left: the granule parses, the spectrum is
         * wrong, and only a transient at a low bitrate is coded finely enough
         * for anyone to hear it. */
        int first = g->mixed ? 3 : 0;    /* mixed: the long part took bands 0..2 */
        int part = 0, idx = 0;
        if (g->mixed) {
            for (int i = 0; i < nr[0] && i < 22; i++)
                m->scalefac_l[ch][i] = slen[0] ? (int)br_get(b, slen[0]) : 0;
            part = 1;
        }
        for (; part < 4; part++)
            for (int i = 0; i < nr[part]; i++, idx++) {
                int sfb = first + idx / 3, w = idx % 3;
                int v = slen[part] ? (int)br_get(b, slen[part]) : 0;
                if (sfb < 13) m->scalefac_s[ch][sfb][w] = v;
            }
    } else {
        int sfb = 0;
        for (int part = 0; part < 4; part++)
            for (int i = 0; i < nr[part] && sfb < 22; i++, sfb++)
                m->scalefac_l[ch][sfb] = slen[part] ? (int)br_get(b, slen[part]) : 0;
    }
}

/* ---- Huffman ------------------------------------------------------------- */

static int huff_sym(br_t *b, int r) {
    int at = r;
    for (int steps = 0; steps < 24; steps++) {
        int nxt = tree[at][br_bit(b)];
        if (nxt < 0) return -nxt - 1;
        if (nxt == 0) return -1;             /* a code that is not in the book */
        at = nxt;
    }
    return -1;
}

/* Decode one granule's spectrum into `is`. Returns the number of lines that
 * turned out to be non-zero, which is what intensity stereo needs to know. */
static int huff_decode(mp3_t *m, br_t *b, const gr_t *g, uint32_t end_bit, int *is) {
    memset(is, 0, 576 * sizeof *is);

    const uint16_t *sfb = sfb_long[m->cur.rate_idx];
    uint32_t r1, r2;
    if (g->window_switching && g->block_type == 2) {
        /* A short granule's regions are not in the stream: region 0 is the
         * first three scalefactor bands of all three windows, and there is no
         * region 2. That is 36 lines at every rate anyone encodes at, which is
         * why it is so often written as a constant -- but not at 8 kHz, where
         * the bands are twice as wide. */
        r1 = (uint32_t)sfb_short[m->cur.rate_idx][3] * 3;
        r2 = 576;
    } else {
        /* A switched long block does not carry the counts either; the standard
         * fixes them at 7 and 13, which parse_side has already filled in. */
        int a = g->region0_count + 1;
        int c = g->region0_count + g->region1_count + 2;
        if (a > 22) a = 22;
        if (c > 22) c = 22;
        r1 = sfb[a];
        r2 = sfb[c];
    }
    uint32_t big = (uint32_t)g->big_values * 2;
    if (big > 576) big = 576;
    if (r1 > big) r1 = big;
    if (r2 > big) r2 = big;

    uint32_t i = 0;
    for (int region = 0; region < 3; region++) {
        uint32_t stop = region == 0 ? r1 : region == 1 ? r2 : big;
        int t = g->table_select[region];
        if (t >= 32 || root[t] < 0) { i = stop; continue; }
        int side = htab[t].side, lin = htab[t].linbits;
        int r = root[t];
        while (i < stop) {
            if (b->bit >= end_bit || b->over) return (int)i;
            int sym = huff_sym(b, r);
            if (sym < 0) return (int)i;
            int x = sym / side, y = sym % side;
            if (x == 15 && lin) x += (int)br_get(b, lin);
            if (x && br_bit(b)) x = -x;
            if (y == 15 && lin) y += (int)br_get(b, lin);
            if (y && br_bit(b)) y = -y;
            is[i++] = x;
            is[i++] = y;
        }
    }

    /* count1: quadruples of -1, 0 or 1, which is what the top of the spectrum
     * almost always decays to. It runs until the granule's bits are gone, so
     * overshooting the budget by one codeword is normal and is rolled back. */
    int r = root[g->count1table ? 33 : 32];
    while (i + 3 < 576 && b->bit < end_bit && !b->over) {
        uint32_t at = b->bit;
        int sym = huff_sym(b, r);
        if (sym < 0) { b->bit = at; break; }
        int v[4] = { (sym >> 3) & 1, (sym >> 2) & 1, (sym >> 1) & 1, sym & 1 };
        for (int k = 0; k < 4; k++)
            if (v[k] && br_bit(b)) v[k] = -v[k];
        if (b->bit > end_bit) { b->bit = at; break; }
        for (int k = 0; k < 4; k++) is[i++] = v[k];
    }

    int nz = 0;
    for (int k = 575; k >= 0; k--) if (is[k]) { nz = k + 1; break; }
    return nz;
}

/* ---- requantisation ------------------------------------------------------ */

static void requantize(mp3_t *m, const gr_t *g, int ch, const int *is, float *xr) {
    const uint16_t *sl = sfb_long[m->cur.rate_idx];
    const uint16_t *ss = sfb_short[m->cur.rate_idx];
    int mul = 1 + g->scalefac_scale;             /* quarter-step multiplier */
    int gain = (int)g->global_gain - 210;

    for (int i = 0; i < 576; i++) xr[i] = 0.0f;

    int short_from = 0;                          /* first line of the short part */
    if (g->window_switching && g->block_type == 2) {
        if (g->mixed) {
            short_from = sl[8];
            for (int sfb = 0; sfb < 8; sfb++) {
                int e = gain - 2 * mul * (m->scalefac_l[ch][sfb] +
                                          (g->preflag ? pretab[sfb] : 0));
                float s = exp4(e);
                for (int i = sl[sfb]; i < sl[sfb + 1]; i++) {
                    int v = is[i];
                    if (!v) continue;
                    float a = pow43_of(v < 0 ? -v : v) * s;
                    xr[i] = v < 0 ? -a : a;
                }
            }
        }
        /* Short bands are stored band-major: all three windows of a band sit
         * next to each other, which is also the order they were coded in. */
        int start_sfb = g->mixed ? 3 : 0;
        for (int sfb = start_sfb; sfb < 13; sfb++) {
            int w0 = ss[sfb], w1 = ss[sfb + 1];
            if (w0 * 3 >= 576) break;
            for (int w = 0; w < 3; w++) {
                int e = gain - 8 * (int)g->subblock_gain[w]
                             - 2 * mul * m->scalefac_s[ch][sfb][w];
                float s = exp4(e);
                int base = w0 * 3 + w * (w1 - w0);
                for (int k = 0; k < w1 - w0; k++) {
                    int i = base + k;
                    if (i >= 576) break;
                    if (i < short_from) continue;
                    int v = is[i];
                    if (!v) continue;
                    float a = pow43_of(v < 0 ? -v : v) * s;
                    xr[i] = v < 0 ? -a : a;
                }
            }
        }
        return;
    }

    for (int sfb = 0; sfb < 22; sfb++) {
        int e = gain - 2 * mul * (m->scalefac_l[ch][sfb] +
                                  (g->preflag ? pretab[sfb] : 0));
        float s = exp4(e);
        for (int i = sl[sfb]; i < sl[sfb + 1] && i < 576; i++) {
            int v = is[i];
            if (!v) continue;
            float a = pow43_of(v < 0 ? -v : v) * s;
            xr[i] = v < 0 ? -a : a;
        }
    }
}

/* ---- stereo -------------------------------------------------------------- */

static void stereo(mp3_t *m, const gr_t *g, float xr[2][576], int nz_right) {
    int ms = (m->cur.mode == 1) && (m->cur.mode_ext & 2);
    int is_on = (m->cur.mode == 1) && (m->cur.mode_ext & 1);

    /* Intensity stereo, MPEG-1 only: above the last line the right channel
     * coded, the pair is one signal plus an angle, and the angle arrives as
     * what would otherwise be the right channel's scalefactor. */
    int is_from = 576;
    if (is_on && !m->cur.lsf) {
        const uint16_t *sl = sfb_long[m->cur.rate_idx];
        const uint16_t *ss = sfb_short[m->cur.rate_idx];
        if (g->window_switching && g->block_type == 2) {
            for (int sfb = 0; sfb < 13; sfb++) {
                int w0 = ss[sfb], w1 = ss[sfb + 1];
                if (w0 * 3 >= nz_right) {
                    for (int s = sfb; s < 13; s++) {
                        int a0 = ss[s], a1 = ss[s + 1];
                        for (int w = 0; w < 3; w++) {
                            int p = m->scalefac_s[1][s][w];
                            if (p == 7) continue;
                            int base = a0 * 3 + w * (a1 - a0);
                            for (int k = 0; k < a1 - a0 && base + k < 576; k++) {
                                float v = xr[0][base + k];
                                xr[0][base + k] = v * is_ratio[0][p & 7];
                                xr[1][base + k] = v * is_ratio[1][p & 7];
                            }
                        }
                    }
                    is_from = w0 * 3;
                    break;
                }
                (void)w1;
            }
        } else {
            for (int sfb = 0; sfb < 21; sfb++) {
                if (sl[sfb] >= nz_right) {
                    for (int s = sfb; s < 21; s++) {
                        int p = m->scalefac_l[1][s];
                        if (p == 7) continue;
                        for (int i = sl[s]; i < sl[s + 1] && i < 576; i++) {
                            float v = xr[0][i];
                            xr[0][i] = v * is_ratio[0][p & 7];
                            xr[1][i] = v * is_ratio[1][p & 7];
                        }
                    }
                    is_from = sl[sfb];
                    break;
                }
            }
        }
    }

    if (ms) {
        const float k = 0.70710678f;
        for (int i = 0; i < is_from; i++) {
            float mid = xr[0][i], sidev = xr[1][i];
            xr[0][i] = (mid + sidev) * k;
            xr[1][i] = (mid - sidev) * k;
        }
    }
}

/* ---- reorder, alias, IMDCT ----------------------------------------------- */

/* Short blocks are coded band by band; the IMDCT wants them window by window. */
static void reorder(mp3_t *m, const gr_t *g, float *xr) {
    if (!(g->window_switching && g->block_type == 2)) return;
    const uint16_t *ss = sfb_short[m->cur.rate_idx];
    float tmp[576];
    int from = 0, sfb0 = 0;
    if (g->mixed) { from = sfb_long[m->cur.rate_idx][8]; sfb0 = 3; }
    for (int i = 0; i < from; i++) tmp[i] = xr[i];

    int at = from;
    for (int sfb = sfb0; sfb < 13; sfb++) {
        int w0 = ss[sfb], w1 = ss[sfb + 1], width = w1 - w0;
        for (int k = 0; k < width; k++)
            for (int w = 0; w < 3; w++) {
                int src = w0 * 3 + w * width + k;
                if (at < 576 && src < 576) tmp[at++] = xr[src];
            }
    }
    while (at < 576) tmp[at++] = 0.0f;
    for (int i = 0; i < 576; i++) xr[i] = tmp[i];
}

static void antialias(const gr_t *g, float *xr) {
    int bands;
    if (g->window_switching && g->block_type == 2) bands = g->mixed ? 1 : 0;
    else bands = 31;
    for (int b = 0; b < bands; b++) {
        float *lo = xr + b * 18 + 17;
        float *hi = xr + b * 18 + 18;
        for (int i = 0; i < 8; i++) {
            float a = lo[-i], c = hi[i];
            lo[-i] = a * alias_cs[i] - c * alias_ca[i];
            hi[i]  = c * alias_cs[i] + a * alias_ca[i];
        }
    }
}

/* One subband: 18 lines in, 18 samples out, 18 carried into the next granule. */
static void imdct_band(const gr_t *g, const float *in, float *out, float *over) {
    float raw[36];

    if (g->window_switching && g->block_type == 2) {
        for (int i = 0; i < 36; i++) raw[i] = 0.0f;
        for (int w = 0; w < 3; w++) {
            float part[12];
            for (int i = 0; i < 12; i++) {
                float s = 0.0f;
                for (int k = 0; k < 6; k++) s += in[w + 3 * k] * imdct12[i][k];
                part[i] = s * bwin[2][i];
            }
            for (int i = 0; i < 12; i++) raw[6 + w * 6 + i] += part[i];
        }
    } else {
        const float *win = bwin[g->block_type];
        for (int i = 0; i < 36; i++) {
            float s = 0.0f;
            for (int k = 0; k < 18; k++) s += in[k] * imdct36[i][k];
            raw[i] = s * win[i];
        }
    }

    for (int i = 0; i < 18; i++) {
        out[i] = raw[i] + over[i];
        over[i] = raw[i + 18];
    }
}

/* ---- polyphase synthesis ------------------------------------------------- */

static void synth(mp3_t *m, int ch, const float *sb, int16_t *out, int stride) {
    float *V = m->V[ch];
    int off = (m->voff[ch] - 64) & 1023;
    m->voff[ch] = off;

    for (int i = 0; i < 64; i++) {
        float s = 0.0f;
        const float *row = smat[i];
        for (int k = 0; k < 32; k++) s += row[k] * sb[k];
        V[(off + i) & 1023] = s;
    }

    for (int j = 0; j < 32; j++) {
        float sum = 0.0f;
        for (int i = 0; i < 16; i++) {
            int u = j + 32 * i;
            int v = 128 * (u >> 6) + (u & 63) + ((u & 32) ? 64 : 0);
            sum += dwin[u] * V[(off + v) & 1023];
        }
        int s = (int)(sum * 32767.0f + (sum >= 0 ? 0.5f : -0.5f));
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[j * stride] = (int16_t)s;
    }
}

/* ---- one MPEG frame ------------------------------------------------------ */

/* Decode the frame at m->pos into m->nat. Returns frames produced, 0 at the
 * end of the stream. A frame that will not decode is not fatal: it is skipped
 * and the next one tried, which is what a player should do with a file that
 * has a bad patch in the middle. */
static uint32_t decode_frame(mp3_t *m) {
    int   (*is)[576] = m->is;
    float (*xr)[576] = m->xr;
    float  *sb       = m->sb;

    int16_t carryl = m->nat_frames ? m->nat[m->nat_frames * 2 + 0] : 0;
    int16_t carryr = m->nat_frames ? m->nat[m->nat_frames * 2 + 1] : 0;

    for (;;) {
        if (m->pos + 4 > m->end) { m->ended = 1; return 0; }

        hdr_t h;
        if (parse_header(m->file + m->pos, m->end - m->pos, &h) != 0) {
            /* Resync: an ID3 tag, a bad byte or padding between frames. */
            m->pos++;
            continue;
        }

        m->cur = h;
        const uint8_t *frame = m->file + m->pos;
        uint32_t hlen = 4 + (h.crc ? 2u : 0u);
        uint32_t mainlen = h.bytes - hlen - h.side;
        m->pos += h.bytes;

        br_t sb_r;
        side_t si;
        br_init(&sb_r, frame + hlen, h.side);
        parse_side(&sb_r, &h, &si);

        if (mainlen > MAINMAX) mainlen = MAINMAX;

        /* Slot the frame's main data behind whatever the reservoir still holds,
         * then decode from main_data_begin bytes before the join. A granule
         * routinely starts in the frame before the one carrying its header. */
        if (si.main_data_begin > m->res_len) {
            /* Not enough history -- the first frames after opening or seeking.
             * Keep the data, produce nothing, and try the next frame. */
            if (m->res_len + mainlen > sizeof m->res) m->res_len = 0;
            memcpy(m->res + m->res_len, frame + hlen + h.side, mainlen);
            m->res_len += mainlen;
            if (m->res_len > RESERVOIR) {
                memmove(m->res, m->res + m->res_len - RESERVOIR, RESERVOIR);
                m->res_len = RESERVOIR;
            }
            continue;
        }

        if (m->res_len + mainlen > sizeof m->res) {
            m->res_len = 0;
            continue;
        }
        memcpy(m->res + m->res_len, frame + hlen + h.side, mainlen);
        uint32_t start = m->res_len - si.main_data_begin;
        uint32_t avail = si.main_data_begin + mainlen;

        br_t b;
        br_init(&b, m->res + start, avail);

        int ngr = h.lsf ? 1 : 2;
        uint32_t frames = 0;

        for (int gr = 0; gr < ngr; gr++) {
            int nz[2] = { 0, 0 };
            for (int ch = 0; ch < h.chans; ch++) {
                const gr_t *g = &si.gr[gr][ch];
                uint32_t begin = b.bit;
                uint32_t end_bit = begin + g->part2_3_length;
                if (end_bit > avail * 8) end_bit = avail * 8;

                if (h.lsf) {
                    int right = (ch == 1) && (h.mode == 1) && (h.mode_ext & 1);
                    read_scalefac_lsf(m, &b, ch, g, right);
                } else {
                    read_scalefac_mpeg1(m, &b, &si, gr, ch, g);
                }
                nz[ch] = huff_decode(m, &b, g, end_bit, is[ch]);
                b.bit = end_bit;                    /* whatever we did, land here */
                requantize(m, g, ch, is[ch], xr[ch]);
            }

            if (h.chans == 2) stereo(m, &si.gr[gr][0], xr, nz[1]);

            for (int ch = 0; ch < h.chans; ch++) {
                const gr_t *g = &si.gr[gr][ch];
                reorder(m, g, xr[ch]);
                antialias(g, xr[ch]);

                float pcmband[32][18];
                for (int band = 0; band < 32; band++)
                    imdct_band(g, xr[ch] + band * 18, pcmband[band],
                               m->overlap[ch][band]);

                /* Frequency inversion: every other subband comes out of the
                 * IMDCT with its odd samples flipped, and the filterbank below
                 * expects them the other way up. */
                for (int band = 1; band < 32; band += 2)
                    for (int i = 1; i < 18; i += 2) pcmband[band][i] = -pcmband[band][i];

                for (int i = 0; i < 18; i++) {
                    for (int band = 0; band < 32; band++) sb[band] = pcmband[band][i];
                    int16_t *dst = m->nat + (1 + gr * 576 + i * 32) * 2 + ch;
                    synth(m, ch, sb, dst, 2);
                }
            }

            if (h.chans == 1) {
                for (int i = 0; i < 576; i++) {
                    int16_t v = m->nat[(1 + gr * 576 + i) * 2];
                    m->nat[(1 + gr * 576 + i) * 2 + 1] = v;
                }
            }
            frames += 576;
        }

        m->res_len += mainlen;
        if (m->res_len > RESERVOIR) {
            memmove(m->res, m->res + m->res_len - RESERVOIR, RESERVOIR);
            m->res_len = RESERVOIR;
        }

        m->nat[0] = m->started ? carryl : m->nat[2];
        m->nat[1] = m->started ? carryr : m->nat[3];
        m->started = 1;
        m->nat_frames = frames;
        return frames;
    }
}

/* ---------------------------------------------------------------------------
 *  Opening, and the tags that get in the way of it.
 * ------------------------------------------------------------------------*/

static uint32_t id3v2_len(const uint8_t *p, uint32_t len) {
    if (len < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
    /* Syncsafe: seven bits a byte, so a size can never look like a frame sync. */
    uint32_t n = ((uint32_t)(p[6] & 0x7F) << 21) | ((uint32_t)(p[7] & 0x7F) << 14) |
                 ((uint32_t)(p[8] & 0x7F) << 7)  |  (uint32_t)(p[9] & 0x7F);
    uint32_t total = 10 + n + ((p[5] & 0x10) ? 10u : 0u);
    return total <= len ? total : len;
}

/* Xing/Info (LAME and most others) or VBRI (Fraunhofer): a frame count in the
 * first frame, which is the only way to know how long a variable-bitrate file
 * runs without decoding all of it. */
static void read_vbr_header(mp3_t *m, const hdr_t *h) {
    const uint8_t *f = m->file + m->first;
    uint32_t avail = m->end - m->first;
    if (avail < h->bytes) return;

    uint32_t off = 4 + h->side;
    if (off + 8 <= h->bytes &&
        (memcmp(f + off, "Xing", 4) == 0 || memcmp(f + off, "Info", 4) == 0)) {
        uint32_t flags = ((uint32_t)f[off + 4] << 24) | ((uint32_t)f[off + 5] << 16) |
                         ((uint32_t)f[off + 6] << 8) | f[off + 7];
        uint32_t at = off + 8;
        if ((flags & 1) && at + 4 <= h->bytes) {
            m->nframes = ((uint32_t)f[at] << 24) | ((uint32_t)f[at + 1] << 16) |
                         ((uint32_t)f[at + 2] << 8) | f[at + 3];
            at += 4;
        }
        if ((flags & 2) && at + 4 <= h->bytes) {
            m->vbr_bytes = ((uint32_t)f[at] << 24) | ((uint32_t)f[at + 1] << 16) |
                           ((uint32_t)f[at + 2] << 8) | f[at + 3];
            at += 4;
        }
        if ((flags & 4) && at + 100 <= h->bytes) {
            memcpy(m->toc, f + at, 100);
            m->have_toc = 1;
        }
        /* The header frame itself carries no audio; stepping over it keeps a
         * file from starting with 26 ms of silence and a wrong duration. */
        m->first += h->bytes;
        m->pos = m->first;
        return;
    }

    if (36 + 26 <= h->bytes && memcmp(f + 36, "VBRI", 4) == 0) {
        m->vbr_bytes = ((uint32_t)f[36 + 10] << 24) | ((uint32_t)f[36 + 11] << 16) |
                       ((uint32_t)f[36 + 12] << 8) | f[36 + 13];
        m->nframes = ((uint32_t)f[36 + 14] << 24) | ((uint32_t)f[36 + 15] << 16) |
                     ((uint32_t)f[36 + 16] << 8) | f[36 + 17];
        m->first += h->bytes;
        m->pos = m->first;
    }
}

int mp3_sniff(const void *file, uint32_t len) {
    const uint8_t *p = (const uint8_t *)file;
    if (!p || len < 4) return 0;
    uint32_t at = id3v2_len(p, len);
    if (at) return 1;

    /* No tag: insist on two headers in a row that agree, or a WAV would match
     * on any byte pair that happened to look like a sync. */
    for (uint32_t scan = 0; scan < 8192 && at + 4 <= len; scan++, at++) {
        hdr_t h;
        if (parse_header(p + at, len - at, &h) != 0) continue;
        if (at + h.bytes + 4 > len) return 1;       /* one frame, and that is all */
        hdr_t h2;
        if (parse_header(p + at + h.bytes, len - at - h.bytes, &h2) == 0 &&
            h2.rate == h.rate && h2.chans == h.chans)
            return 1;
    }
    return 0;
}

static const char *mode_name(int mode) {
    return mode == 0 ? "stereo" : mode == 1 ? "joint stereo"
         : mode == 2 ? "dual mono" : "mono";
}

/* Enough string building for one label. snprintf would do it in a line, but
 * pulling libc's stdio into the decoder for that is not a trade worth making:
 * the host harness compiles this file against the real stdio.h, and the two
 * declarations of FILE do not agree. */
static char *put_str(char *at, char *end, const char *s) {
    while (*s && at < end - 1) *at++ = *s++;
    *at = 0;
    return at;
}
static char *put_num(char *at, char *end, uint32_t v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n-- > 0 && at < end - 1) *at++ = tmp[n];
    *at = 0;
    return at;
}

mp3_t *mp3_open(const void *file, uint32_t len, const char **err) {
    const uint8_t *p = (const uint8_t *)file;
    const char *why = "not an MP3";

    if (!p || len < 128) { if (err) *err = "file too short"; return 0; }
    build_tables();

    uint32_t end = len;
    /* An ID3v1 tag at the tail is 128 bytes that are not audio. Harmless to
     * decode -- it never syncs -- but it throws the frame count off. */
    if (end > 128 && memcmp(p + end - 128, "TAG", 3) == 0) end -= 128;

    uint32_t at = id3v2_len(p, len);
    hdr_t h;
    uint32_t scan = 0;
    for (; at + 4 <= end && scan < 65536; at++, scan++)
        if (parse_header(p + at, end - at, &h) == 0) break;
    if (at + 4 > end || scan >= 65536) { if (err) *err = why; return 0; }

    mp3_t *m = (mp3_t *)kmalloc(sizeof *m);
    if (!m) { if (err) *err = "out of memory"; return 0; }
    memset(m, 0, sizeof *m);

    m->file = p;
    m->len = len;
    m->end = end;
    m->first = at;
    m->pos = at;
    m->h = h;
    m->cur = h;
    m->voff[0] = m->voff[1] = 0;

    read_vbr_header(m, &h);
    if (!m->nframes) {
        /* Constant bitrate, or near enough: the byte count divided by a frame
         * is within a frame of the truth and costs nothing to work out. */
        uint32_t bytes = m->end - m->first;
        m->nframes = h.bytes ? bytes / h.bytes : 0;
    }

    m->step = ((uint64_t)h.rate << 16) / AUDIO_RATE;

    const char *v = h.ver == 3 ? "MPEG-1" : h.ver == 2 ? "MPEG-2" : "MPEG-2.5";
    char *d = m->desc, *dend = m->desc + sizeof m->desc;
    d = put_str(d, dend, v);
    d = put_str(d, dend, " Layer III, ");
    d = put_num(d, dend, (uint32_t)h.rate / 1000);
    d = put_str(d, dend, ".");
    d = put_num(d, dend, ((uint32_t)h.rate % 1000) / 100);
    d = put_str(d, dend, " kHz ");
    d = put_str(d, dend, mode_name(h.mode));
    d = put_str(d, dend, ", ");
    d = put_num(d, dend, (uint32_t)h.kbps);
    put_str(d, dend, " kbps");
    return m;
}

void mp3_close(mp3_t *m) { if (m) kfree(m); }

const char *mp3_describe(const mp3_t *m) { return m ? m->desc : ""; }
uint32_t mp3_rate(const mp3_t *m)        { return m ? (uint32_t)m->h.rate : 0; }
uint32_t mp3_channels(const mp3_t *m)    { return m ? (uint32_t)m->h.chans : 0; }
uint32_t mp3_bitrate(const mp3_t *m)     { return m ? (uint32_t)m->h.kbps : 0; }
int      mp3_done(const mp3_t *m)        { return m ? m->ended : 1; }

uint32_t mp3_duration_ms(const mp3_t *m) {
    if (!m || !m->h.rate) return 0;
    uint64_t samples = (uint64_t)m->nframes * (m->h.lsf ? 576 : 1152);
    return (uint32_t)(samples * 1000 / (uint32_t)m->h.rate);
}

uint32_t mp3_position_ms(const mp3_t *m) {
    if (!m) return 0;
    return (uint32_t)(m->out_done * 1000 / AUDIO_RATE);
}

/* Pull `frames` output frames out of the current decoded MPEG frame, decoding
 * at most `budget` more of them. Shared by mp3_read and mp3_step. */
static uint32_t pull(mp3_t *m, int16_t *out, uint32_t frames, int budget) {
    uint32_t made = 0;
    while (made < frames) {
        uint64_t idx = m->posf >> 16;
        if (idx >= m->nat_frames) {
            if (m->ended || budget-- <= 0) break;
            uint32_t old = m->nat_frames;
            if (!decode_frame(m)) break;
            m->posf -= (uint64_t)old << 16;
            continue;
        }
        uint32_t frac = (uint32_t)(m->posf & 0xFFFF);
        const int16_t *a = m->nat + idx * 2;
        const int16_t *b = a + 2;
        for (int ch = 0; ch < 2; ch++) {
            int32_t lo = a[ch], hi = b[ch];
            out[made * 2 + ch] = (int16_t)(lo + (int32_t)(((int64_t)(hi - lo) * frac) >> 16));
        }
        m->posf += m->step;
        made++;
    }
    m->out_done += made;
    return made;
}

uint32_t mp3_read(mp3_t *m, int16_t *out, uint32_t frames) {
    if (!m) return 0;
    return pull(m, out, frames, 1 << 20);
}

uint32_t mp3_step(mp3_t *m, int16_t *out, uint32_t frames) {
    if (!m) return 0;
    return pull(m, out, frames, 1);
}

void mp3_seek_ms(mp3_t *m, uint32_t ms) {
    if (!m) return;
    uint32_t dur = mp3_duration_ms(m);
    if (ms > dur) ms = dur;

    uint32_t bytes = m->end - m->first;
    uint64_t at;
    if (m->have_toc && dur) {
        /* The Xing table of contents is 100 percentiles of the file, which is
         * as good as seeking gets without an index of every frame. */
        uint32_t pct = (uint32_t)((uint64_t)ms * 100 / dur);
        if (pct > 99) pct = 99;
        at = (uint64_t)m->toc[pct] * bytes / 256;
    } else {
        at = dur ? (uint64_t)ms * bytes / dur : 0;
    }

    m->pos = m->first + (uint32_t)at;
    if (m->pos > m->end) m->pos = m->end;

    /* Everything carried between frames is now wrong: the reservoir belongs to
     * frames we skipped and the overlap is the tail of a sound that is not
     * playing any more. Both refill within two frames, which is what the
     * short burst of quiet after a seek is. */
    m->res_len = 0;
    m->nat_frames = 0;
    m->posf = 0;
    m->started = 0;
    m->ended = 0;
    memset(m->overlap, 0, sizeof m->overlap);
    memset(m->V, 0, sizeof m->V);
    m->voff[0] = m->voff[1] = 0;
    m->out_done = (uint64_t)ms * AUDIO_RATE / 1000;
}
