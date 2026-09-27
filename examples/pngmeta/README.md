# examples/pngmeta

Reads and edits PNG metadata at the byte level, without touching the pixels:
the whole file is parsed into chunks, each chunk's CRC-32 is checked, and
edited files are written back with freshly computed CRCs.

```text
$ diamond pngmeta.di info testdata/photo.png
testdata/photo.png: 4x3, 8-bit RGB
  IHDR     13
  gAMA      4
  tEXt     19  Author: Ada Lovelace
  tEXt     23  Software: PaintThing 2.1
  tIME      7  2026-09-26 14:30:05 UTC
  IDAT     47
  tEXt     42  Comment: taken on the pier, GPS 51.5N 0.12W
  IEND      0
$ diamond pngmeta.di strip testdata/photo.png /tmp/clean.png
/tmp/clean.png: 120 bytes, removed 4 chunks
$ diamond pngmeta.di set testdata/photo.png /tmp/tagged.png Author "Grace Hopper"
/tmp/tagged.png: 259 bytes, Author set
```

## Usage

```text
pngmeta info FILE...
pngmeta strip IN OUT
pngmeta set IN OUT KEY VALUE
```

- `info` lists the dimensions, color type, and every chunk, with the text of
  `tEXt` entries and the date of a `tIME` chunk, and flags any chunk whose
  CRC doesn't match (exit status 1).
- `strip` drops the text, time, and EXIF chunks (`tEXt`, `zTXt`, `iTXt`,
  `tIME`, `eXIf`).
- `set` adds a `tEXt` entry, replacing any with the same keyword.

`strip` and `set` refuse to rewrite a file with a damaged chunk, since
copying it into a new file with a fresh CRC would hide the damage. Exit
status is 0 on success, 1 for a file that isn't a valid PNG, 64 for a usage
error, and 66 when a file can't be read.

## What it shows

- **Binary data in Strings.** `File.read`/`File.write` move the whole file
  as one String; `getbyte` reads single bytes, `slice` pulls out chunks, and
  `chr` builds bytes back up (`u32_bytes` in `lib/png.di`).
- **Byte and bit arithmetic**: big-endian integers assembled with `<<` and
  `|`, and a table-driven CRC-32 (`lib/crc32.di`) using `^`, `>>`, `&`, and
  hex literals such as `0xEDB8_8320`.
- **String escapes for bytes**: the PNG signature is
  `"\x89PNG\r\n\x1a\n"`.
- **A module class variable as a cache**: `Crc32.table` builds its
  256-entry table once and keeps it in `@@table`.
- **Structs with methods**: `Chunk` checks its own CRC (`intact?`) and
  kind (`ancillary?`); `Header` names its color type.

## Test

```sh
bash smoke_test.sh
```

It runs `info` under the interpreter and as a `diamond build` binary, then
checks `strip` and `set` by reading their output back, plus every error
case. The test images are described in `testdata/README.md`.
