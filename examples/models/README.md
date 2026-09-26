# examples/models

Record classes whose fields come from data: `Book` and `Author` are empty
subclasses of `Model`, and every accessor they end up with is generated at
run time from a JSON schema. A tour of Diamond's metaprogramming.

```text
$ cat testdata/schema.json
{"Book": {"title": {"type": "String", "required": true, "max": 40},
          "pages": {"type": "Int", "min": 1},
          "isbn": {"type": "String", "required": true},
          "in_print": {"type": "Bool"}},
 "Author": {...}}

$ diamond models.di testdata/schema.json
== generated methods
  Book responds to title: true
  Book responds to in_print?: true
  Book responds to name: false
...
== type checks, read-only fields, validation
  set pages to a String: TypeError: Book.pages must be Int, got String
  change the isbn: FrozenError: Book.isbn is read-only once set
  draft valid? false
    title is longer than 40 characters
    pages must be at least 1
    isbn is required

== dynamic finders
  find_by_pages(173): Book(title: Babel-17, pages: 173, isbn: 978-0375706691, in_print: false)
  where_in_print(true): [Dune, The Left Hand of Darkness]
...
```

`testdata/expected.txt` has the complete output.

## What it shows

- **Methods from data.** `Model.fields(spec)` (`lib/model.di`) loops over
  the schema and, for each field, builds a method body from a string with
  `self.compile_method` and installs it with `self.define_method`: a reader,
  a writer (`title=`, which `book.title = "Dune"` calls), and for Bool
  fields a predicate (`in_print?`). Values the body needs (the field name,
  its type) are passed in as bound values rather than spliced into the
  source.
- **Per-subclass methods.** Because `fields` runs as `Book.fields(...)`,
  `self` is `Book`, so the methods land on `Book` only: `Author` never gains
  `title`.
- **Replacing a method.** `Model.readonly("isbn")` swaps the generated
  `isbn=` for a write-once version with `self.redefine_method`.
- **Inherited factories.** `Book.build(attributes)` is defined once on
  `Model`; `self.new()` constructs a `Book`, and `public_send("#{key}=", ...)`
  sets each attribute by name.
- **`method_missing`.** `Collection` answers `find_by_<field>(value)` and
  `where_<field>(value)` by parsing the method name with a Regexp; anything
  else is still a `NoMethodError`.
- **Reflection.** `respond_to?` and `public_send` inspect and call the
  generated methods; `self.class()` names the record in error messages.
- **Change tracking and validation** on top: the generated writers go
  through `Model#assign`, which type-checks, records `[before, after]`
  pairs (dropping a field changed back to its original value), and the
  schema's `required`/`max`/`min` rules drive `errors()`.

## Test

```sh
bash smoke_test.sh
```

This compares the demo's output with `testdata/expected.txt` under the
interpreter and as a `diamond build` binary, and checks that removing a
field from the schema removes its generated method.
