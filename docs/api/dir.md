# Dir

[API reference](index.md) · Previous: [File](file.md) · Next: [Time](time.md)

`Dir` lists, creates and removes directories. Failures raise `IOError`, and
every function needs the `filesystem` sandbox capability.

<!-- doc-check: skip -->
```ruby
Dir.mkdir_p("backups/2026")
Dir.entries("backups")   # => ["2026"]
```

## Functions

### `Dir.delete(path) -> String`

Removes an empty directory and returns `path`.

### `Dir.entries(path) -> Array[String]`

The names in a directory, without `.` and `..`, in no particular order. It does
not recurse.

### `Dir.mkdir(path, mode = 0o777) -> String`

Creates one directory (the mode is subject to the umask) and returns `path`.
An existing path or a missing parent raises `IOError`.

### `Dir.mkdir_p(path, mode = 0o777) -> String`

Creates the directory and any missing parents, like `mkdir -p`, and returns
`path`. A directory that already exists is fine.

### `Dir.sync(path) -> String`

Flushes the directory itself to disk, so new or renamed entries survive a
crash, and returns `path`.
