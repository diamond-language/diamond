# Field Notes: Diamond & Friends

Diamond is a *gradually typed* language with **Ruby-like** syntax.
This paragraph spans two lines, and mentions `a < b && c > d` in code.

## Getting started

1. Install the toolchain.
2. Run `diamond script.di`.
3. Read the [language reference](docs/syntax.md).

## Getting started

A second heading with the same title gets its own id.

### Things to try

- Pattern matching with `case`/`when`
- **Fibers** for *generators*
- Threads with [channels](docs/threads.md#channels)
- A suspicious [link](JavaScript:steal) is neutralized

> Quotes can hold *anything*:
>
> - even lists
> - and more quotes:
>
> > nested, two levels deep

---

```ruby
def greet(name: String) -> String
  "hello, #{name} <b>not bold</b> **not strong**"
end
```

An unpaired ` backtick, 5 * 3 * 2, and a
**bold** that wraps *across* a line break.
