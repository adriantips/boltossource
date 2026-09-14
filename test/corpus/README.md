# Fuzzer regression corpus

One file per bug that `test/t_fuzz.py` has found. Every one of them is replayed,
unmutated, before any fuzzing starts, so a fix that gets undone is a test
failure the same day rather than a rediscovery next year.

    test/corpus/<harness>/<target>/<what-it-broke>.bin

`<harness>` is `web`, `net` or `tcp`; `<target>` is one of the names that
harness's `--list` prints. The file name says what the input does, not which
seed produced it -- a seed stops meaning anything the moment the mutator or the
seed corpus changes, and the bytes do not.

A name ending `.oomN.bin` is replayed with the heap set to start refusing after
N allocations, which is the condition the bug needs. Several of these are
allocation-failure paths that no amount of ordinary input reaches.

To add one: a crash prints where it saved the input. Rename it for the bug and
drop it in the right directory.
