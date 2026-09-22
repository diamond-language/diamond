# dials

Route requests to controllers and construct HTTP responses.

## Installation

Install the cut at `cuts/dials/` and load it with `require_cut "dials"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "dials"

router = Dials::Router.new()
router.get("/authors", AuthorsController.index)
router.get("/authors/:id", AuthorsController.show)
router.post("/authors", AuthorsController.create)

response = router.dispatch(request, context)
```

## Notes

Route handlers receive `(request, context, params)` and return `[status, headers, body]`. Routes are tested in registration order; `:name` segments populate `params`.
