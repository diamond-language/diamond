# examples/udiff

A unified diff and patch tool: compares two text files the way `diff -u`
does, and applies a unified diff (from `udiff`, `diff -u`, or `git diff`) to
a file, forwards or backwards.

```text
$ diamond udiff.di testdata/old.txt testdata/new.txt
--- testdata/old.txt
+++ testdata/new.txt
@@ -1,11 +1,14 @@
 The quick brown fox
 jumps over the lazy dog.
 
-Pack my box with five
+Pack my box with six
 dozen liquor jugs.
 How vexingly quick daft zebras jump!
 The five boxing wizards
 jump quickly.
+Bright vixens jump;
+dozy fowl quack.
 
 Sphinx of black quartz,
 judge my vow.
+And one more line.
$ diamond udiff.di --apply -R testdata/diff.expected testdata/new.txt | cmp testdata/old.txt -
```

## Usage

```text
udiff [-U N] [-i] [-w] [-q] [--stat] OLD NEW
udiff --apply [-R] [-o OUT] PATCH FILE
```

`-U N` sets the lines of context (default 3), `-i` ignores case, `-w`
ignores all whitespace, `-q` only says whether the files differ, and `--stat`
prints a change count instead of the diff. `--apply` writes the patched file
to stdout, or to `-o OUT`; `-R` applies the patch backwards. A patch that
doesn't fit is refused whole, naming the first line that didn't match.

Exit status: 0 when the files are the same (or the patch applied), 1 when
they differ, 2 for a usage error, an unreadable file, or a patch that
doesn't fit -- the same convention as `diff`.

## How it works

- `lib/edit.di` -- `Edit` is a **sealed** class with three kinds, `Keep`,
  `Delete`, and `Insert`. Every `case` over an `Edit` must name all three, so
  adding a fourth stops the diff, hunk, and patch code from compiling until
  each decides what to do with it.
- `lib/myers.di` -- Myers' O(ND) shortest-edit-script algorithm, the one
  `diff` and `git` use. Cost grows with the number of changed lines, not
  with the product of the file sizes: a 3000-line file with 180 changed
  lines diffs in about a quarter of a second in a release build. (An
  earlier version filled a longest-common-subsequence table instead and
  took 38 seconds on the same input.)
- `lib/hunks.di` -- groups an edit script into hunks with N lines of
  context, merges hunks whose contexts touch, and prints `@@ -a,b +c,d @@`
  headers, including GNU's convention that an empty range starts at the line
  *before* the hunk.
- `lib/patch.di` -- parses hunks with a `Regexp`, checks each hunk's line
  counts against its header, and applies them, verifying every context and
  deleted line against the file before changing anything.

## Checked against diff(1)

`smoke_test.sh` compares `udiff` with GNU `diff -U N` byte for byte (headers
aside) at four context sizes when `diff` is installed. Beyond that, the
edit script was fuzzed against `diff` and `git apply` on several hundred
random file pairs: every diff round-trips through `--apply`, undoes with
`--apply -R`, applies cleanly with `git apply`, and has the same number of
changed lines as GNU's -- two minimal diffs of the same pair can differ in
*which* equal lines they keep, but never in how many they change.

## Limits

Files are read whole. The `\ No newline at end of file` marker is skipped
when reading a patch and never written, so a file's missing final newline is
not preserved.

```bash
bash smoke_test.sh          # interpreter, then a `diamond build` binary
```
