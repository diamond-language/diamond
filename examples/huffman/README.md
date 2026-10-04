# examples/huffman

Lossless compression by Huffman coding: build a code tree from byte
frequencies, give each byte a bit string, and pack those into a real
binary file.

```text
$ diamond huffman.di --encode testdata/sample.txt out.huf
testdata/sample.txt: 2250 -> 1351 bytes (60%)
$ diamond huffman.di --decode out.huf back.txt
out.huf: decoded 2250 bytes
$ cmp testdata/sample.txt back.txt && echo identical
identical
```

A small or already-random-looking file can come out *larger*, not
smaller — the per-symbol code table has its own fixed overhead, and
Huffman only wins once the input is big enough, or skewed enough in
which bytes it uses, to make that overhead worth paying.

## Usage

```text
huffman.di --encode IN OUT
huffman.di --decode IN OUT
```

Exit status is 0 on success, 64 for a usage error, 65 for a bad input
(an empty file to encode, or a corrupt/truncated/not-a-container file to
decode), and 66 when a file can't be opened.

## What it shows

- **Building a tree from a small forest, no priority queue needed.**
  `huffman_build_tree` (`lib/tree.di`) repeatedly sorts the whole forest
  and merges its two least-frequent nodes — with at most 256 distinct
  byte values, that's simpler than a real heap and plenty fast.
- **A sealed `Leaf`/`Branch` tree**, walked recursively
  (`huffman_collect_codes`) to build each byte's code as a `String` of
  `'0'`/`'1'` characters — including the one-node edge case (a file using
  only a single byte value), which gets the code `"0"` directly rather
  than a zero-length string a Branch would normally produce one bit at a
  time.
- **A real binary container format** (`lib/bits.di`): big-endian
  fixed-width integers via `>>`/`&` on `Int` — the same idiom
  examples/pngmeta uses for a PNG chunk length — and packing a bit string
  into actual bytes, most-significant-bit first, with `File.read`/
  `File.write` (a one-shot whole-file read/write, not `File.open`'s own
  handle) moving the raw bytes.
- **Storing the code table, not the frequencies.** A decoder here never
  rebuilds the Huffman tree — it reads the literal code each byte was
  given and decodes by growing a bit string one bit at a time until it
  matches an entry. Rebuilding the tree from frequencies alone would risk
  a real bug: two symbols that tie on frequency can be merged in either
  order, so a decoder doing its own tree-building over the same
  frequencies isn't guaranteed to reconstruct the *same* tree the encoder
  built, and would decode garbage the moment it didn't.
- **A top-level constant read from functions** — the magic bytes are
  `MAGIC = "HUF1"`, shared by the encoder and the decoder's header check.

## Test

```sh
bash smoke_test.sh
```

Runs the program under the interpreter and as a `diamond build` binary,
checks the compressed output is byte-for-byte reproducible and decodes
back to the exact original, a single-symbol file round-trips, and every
error path.
