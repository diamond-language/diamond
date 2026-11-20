# toml

Read and write [TOML 1.0](https://toml.io/en/v1.0.0) configuration files. The parser passes all
717 cases of the official [toml-test](https://github.com/toml-lang/toml-test) suite for TOML 1.0.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/d6/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no Cutfile yet
facet add toml
facet install
```

This installs the cut into `cuts/toml/`; load it with `require_cut "toml"`.

## Reading

`TOML.parse` returns the document as a `Hash`, and `TOML.parse_file(path)` reads a file:

```ruby
require_cut "toml"

config = TOML.parse("name = \"shop\"\n\n[database]\nhost = \"localhost\"\nports = [5432, 5433]\n")
config["name"]                # => "shop"
config["database"]["ports"]   # => [5432, 5433]
```

TOML values become d6 values:

| TOML | d6 |
|---|---|
| string | `String` |
| integer | `Int` |
| float, `inf`, `nan` | `Float` |
| boolean | `true` / `false` |
| array | `Array` |
| table, inline table | `Hash` |
| offset date-time (`1979-05-27T07:32:00Z`) | `Time` |
| local date, time or date-time (`1979-05-27`) | `String` |

A local date or time names no single instant, so it stays text, written with an upper-case `T`
between date and time. An offset date-time becomes a `Time`, which keeps about a microsecond of
precision.

```ruby
require_cut "toml"

times = TOML.parse("released = 1979-05-27T07:32:00Z\nbirthday = 1979-05-27\n")
times["released"].year()   # => 1979
times["birthday"]          # => "1979-05-27"
```

Invalid TOML raises `TOML::ParseError`, whose message starts with the line and column:

```ruby
require_cut "toml"

begin
  TOML.parse("[server]\nport = 80\nport = 8080\n")
rescue error: TOML::ParseError
  error.message()   # => "line 3, column 1: port is defined twice"
end
```

## Writing

`TOML.generate` turns a `Hash` into TOML. Each table's plain values come first, then its
sub-tables as `[headers]`, then arrays of `Hash`es as `[[headers]]`:

```ruby
require_cut "toml"

text = TOML.generate({"name": "shop", "database": {"host": "localhost"}, "users": [{"id": 1}, {"id": 2}]})
puts(text)
# name = "shop"
#
# [database]
# host = "localhost"
#
# [[users]]
# id = 1
#
# [[users]]
# id = 2
```

`TOML.parse(TOML.generate(doc))` gives back `doc`. TOML has no null, so a `nil` anywhere raises
`TOML::GenerateError`, as does an integer outside 64 bits or a value of any other class.
Comments and formatting in a file you read aren't kept when you write it back.

## API

- `TOML.parse(text) -> Hash`
- `TOML.parse_file(path) -> Hash`
- `TOML.generate(document) -> String`
- `TOML::ParseError`, with `line()` and `column()`
- `TOML::GenerateError`
