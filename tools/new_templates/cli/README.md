# {{name}}

A command-line program written in [Diamond](https://getdiamond.codes).

```sh
diamond main.d6 Ada        # Hello, Ada!
diamond main.d6 --help
bash test.sh               # run the tests
diamond build main.d6 -o {{name}}   # a standalone executable
```

- `main.d6` is the entry point. It passes `ARGV` to `{{Name}}.main` and exits with the
  status that returns.
- `lib/{{name}}.d6` holds the logic.
- `test/{{name}}_test.d6` tests it with `test/minitest.d6`, a copy of Diamond's minitest
  library.

Add a dependency with `facet add <cut> --registry https://cuts.dilang.tech --version "^x.y"`,
then `facet update`, and load it with `require_cut "<cut>"`.
