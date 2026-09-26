# examples/redact

Scrubs secrets and personal data out of logs before you paste them into an
issue: emails, IP addresses, payment card numbers, bearer tokens, AWS keys,
`password=`-style values, and private key headers, plus any rules you add.

```text
$ diamond redact.di --summary --rules testdata/rules.txt testdata/app.log
2026-09-26T10:00:01Z INFO login ok user=[email-1] ip=[ipv4-1]
2026-09-26T10:00:02Z INFO GET /api/orders Authorization: Bearer [bearer-1]
2026-09-26T10:00:03Z WARN payment declined card=[card-1] user=[email-1]
2026-09-26T10:00:04Z INFO order 1234567890123 shipped to [email-2]
2026-09-26T10:00:05Z DEBUG config loaded: db_url=postgres://app@[ipv4-2]/prod password=[secret-1]&retries=3
2026-09-26T10:00:06Z INFO build 1.2.3.4000 from 999.1.1.1 not an address; [aws_key-1] rotated
2026-09-26T10:00:07Z ERROR key dump [private_key-1] truncated
2026-09-26T10:00:08Z INFO [ticket-1] opened by [employee_id-1] via [ipv4-1]
aws_key        1
bearer         1
card           1
email          3
...
```

The same value always gets the same tag, across every file in one run, so
the redacted log still shows that lines 1 and 3 are the same user and that
line 8 came from the same address as line 1. `--mask` keeps the shape
instead: every letter and digit becomes `*`, except a card's last four
digits.

Not everything that looks like a match is one. `1234567890123` has the
length of a card number but fails its checksum, and `999.1.1.1` has an
octet over 255, so both are left alone.

## Usage

```text
redact [--mask] [--rules FILE] [--summary] [FILE...]
```

- Reads each `FILE` in order, or stdin when there are none (or for `-`),
  and writes the redacted text to stdout.
- `--summary` prints how many values each rule replaced, to stderr.
- `--rules FILE` adds rules after the built-in ones. Each line is a name,
  flags (`-` for none, `i` to ignore case, `x` for extended), and a
  pattern that runs to the end of the line:

  ```text
  employee_id  -  \bEMP-[0-9]{6}\b
  ticket       i  \bticket #[0-9]+
  ```

Exit status is 0 on success, 64 for a usage error, 65 for a bad rules file
(an unparseable line, or a pattern that doesn't compile, reported with its
line number), and 66 when a file can't be opened.

## What it shows

- **`gsub` with a block.** `Tagger#redact` passes each match to a block and
  uses what it returns, which is how one rule can tag, mask, or leave a
  match alone depending on a check.
- **Regexp features**: lookbehind (`(?<=[Bb]earer )`), alternation inside
  lookbehind, word boundaries, case-insensitive and extended options, and
  capture groups read with `Regexp#match` in the rules-file parser.
- **Checks a regex can't express** (`lib/checks.di`): the Luhn checksum and
  octet ranges, passed around as `Callable` values in a `Rule` struct.
- **`RegexpError` handling**: a bad pattern in a rules file becomes a
  line-numbered error instead of a crash.
- **Collections**: `reduce` threads a line through every rule, `rules +
  load_rules(...)` joins two arrays, and a Hash keeps each value's tag.
- **Methods calling methods**: inside `Tagger`, `masked(rule, found)` calls
  a sibling method without `self.`.

## Test

```sh
bash smoke_test.sh
```

It runs the program under the interpreter and as a `diamond build` binary,
and checks both modes, the summary, stdin input, tags that carry across
files, and every exit status.
