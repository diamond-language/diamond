# active_record roadmap

`active_record` provides repositories, associations, validation, migrations, lazy relations, and an optional model layer. See [README.md](README.md) for usage.

## Next work

- Improve `ActiveRecord::Model` ergonomics where a real application needs them.
- Keep relation composition lazy and preserve explicit database connections at execution.
- Verify model and migration behavior across supported database dialects.

Avoid adding Ruby-style macros unless Diamond can support them with clear, predictable behavior.
