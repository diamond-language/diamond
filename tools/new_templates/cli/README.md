# {{name}}

A command-line program written in [d6](https://getdiamond.codes).

```sh
facet install              # fetch minitest, the test library, into cuts/
d6 main.d6 Ada        # Hello, Ada!
d6 main.d6 --help
bash test.sh               # run the tests
d6 build main.d6 -o {{name}}   # a standalone executable
```

- `main.d6` is the entry point. It passes `ARGV` to `{{Name}}.main` and exits with the
  status that returns.
- `lib/{{name}}.d6` holds the logic.
- `test/{{name}}_test.d6` tests it with minitest.
  `Cutfile.dev` lists it because only the tests need it.

Add a dependency with `facet add <cut>`, fetch it with `facet install`, and load it with
`require_cut "<cut>"`.
