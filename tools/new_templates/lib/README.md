# {{name}}

TODO: say what {{name}} does.

## Installation

```sh
facet add {{name}} --registry https://cuts.dilang.tech --version "^0.1.0"
facet update
```

## Usage

```ruby
require_cut "{{name}}"

puts({{Name}}.greeting("Ada"))   # Hello, Ada!
```

## Development

```sh
facet install                    # fetch minitest, the test library, into cuts/
bash test.sh                     # run the tests
facet check .                    # check the cut can be published
```

`lib/{{name}}.d6` is the public entry point. `test/{{name}}_test.d6` uses minitest,
which `diamond.dev.cut` lists as a development dependency: users of {{name}} do not
install it, and neither `diamond.dev.cut`, `test/` nor `test.sh` is published. Before publishing, fill in `summary` in `diamond.cut` and check that the
maintainer contact is one you want listed publicly in the registry.
