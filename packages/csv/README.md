# csv

Read and write CSV files: quoted fields, embedded commas, quotes and line breaks, any one-byte
separator, and files too large to load at once.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/d6/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no Cutfile yet
facet add csv
facet install
```

This installs the cut into `cuts/csv/`; load it with `require_cut "csv"`.

## Reading

`CSV.parse` returns every row as an `Array` of `String`s:

```ruby
require_cut "csv"

rows = CSV.parse("name,city\nAda,London\n\"Grace\",\"New York, NY\"\n")
rows[2]   # => ["Grace", "New York, NY"]
```

`CSV.parse_records` treats the first row as column names and returns a `Hash` for each later
row. A row shorter than the header gives `nil` for the missing columns:

```ruby
require_cut "csv"

people = CSV.parse_records("name,age\nAda,36\nBob\n")
people[0]          # => {"name": "Ada", "age": "36"}
people[1]["age"]   # => nil
```

Values stay `String`s; convert them where you use them (`people[0]["age"].to_i()`).

Pass `separator:` for tab-separated or semicolon-separated files:

```ruby
require_cut "csv"

CSV.parse("id\tnote\n7\ta, b\n", separator: "\t")   # => [["id", "note"], ["7", "a, b"]]
```

### Large files

`CSV.foreach` reads a file in 64 KiB chunks and calls the block with each row, and
`CSV.foreach_record` does the same with a `Hash` per row:

```ruby
require_cut "csv"

CSV.foreach_record("orders.csv") do |order|
  puts("#{order["id"]}: #{order["total"]}")
end
```

For input that arrives some other way, such as a socket or an HTTP body, feed it to a
`CSV::Reader` yourself. `feed` returns the rows each chunk completes, and `finish` returns the
rest once the input ends:

```ruby
require_cut "csv"

reader = CSV::Reader.new()
first = reader.feed("a,\"b\n")   # => []
reader.feed("c\"\nd,e\n")        # => [["a", "b\nc"], ["d", "e"]]
reader.finish()                  # => []
```

## Writing

`CSV.generate` quotes a field only when it has to: when it holds the separator, a quote or a
line break, or starts or ends with a space. `nil` becomes an empty field, and other values are
written with `to_s`:

```ruby
require_cut "csv"

CSV.generate([["id", "note"], [1, "says \"hi\", twice"], [2, nil]])
# => "id,note\n1,\"says \"\"hi\"\", twice\"\n2,\n"
```

`CSV.generate_records` writes a header row and one row per `Hash`. The columns are the first
record's keys, or the ones you list in `headers:`:

```ruby
require_cut "csv"

CSV.generate_records([{"sku": "A1", "qty": 2}, {"sku": "B7"}])   # => "sku,qty\nA1,2\nB7,\n"
CSV.generate_records([{"sku": "A1", "qty": 2}], headers: ["qty"])  # => "qty\n2\n"
```

Lines end in `"\n"`. Pass `line_ending: "\r\n"` for tools that want RFC 4180's line endings,
and `separator:` here as well. `CSV.generate_line(row)` writes a single row.

## Rules

- Fields in double quotes can hold the separator, line breaks and `""` for a literal quote.
- A quote in the middle of an unquoted field (`5" screen`) is kept as an ordinary character.
- Lines can end in `\n`, `\r\n` or `\r`. Blank lines are skipped, and a UTF-8 byte-order mark
  at the start of the input is dropped.
- Text is passed through byte for byte; the cut doesn't check or convert encodings.
- `CSV::MalformedError` is raised for a quoted field that never closes, for text right after a
  closing quote (`"a"b`), and by the record methods for a row with more fields than the header.
  Its `line()` is the line where the problem is (0 for a record error, whose message names the
  row instead).

## API

- `CSV.parse(text, separator = ",") -> Array`
- `CSV.parse_records(text, separator = ",") -> Array`
- `CSV.foreach(path, separator = ",") do |row| ... end`
- `CSV.foreach_record(path, separator = ",") do |record| ... end`
- `CSV::Reader.new(separator = ",")`, with `feed(chunk) -> Array` and `finish() -> Array`
- `CSV.generate(rows, separator = ",", line_ending = "\n") -> String`
- `CSV.generate_line(row, separator = ",", line_ending = "\n") -> String`
- `CSV.generate_records(records, headers = nil, separator = ",", line_ending = "\n") -> String`
- `CSV.format_field(value, separator = ",") -> String`
