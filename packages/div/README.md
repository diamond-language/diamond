# div

Compile `.html.div` templates into Diamond source files.

## Installation

Install the cut at `cuts/div/` and load it with `require_cut "div"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```sh
diamond cuts/div/bin/divc.di views/index.html.div
# Writes views/.cache/index.html.di
```

## Notes

Use `require "./views/.cache/index.html"` to load generated code, then call `index_html(...)`. A template named `views/greeting.html.div` generates `greeting_html(...)`. `<%= value %>` escapes HTML; `<%== html %>` inserts trusted HTML. `<% code %>` runs Diamond code.
