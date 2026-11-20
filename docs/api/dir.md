# Dir

[API reference](index.md) · Previous: [File](file.md) · Next: [Time](time.md)

`Dir` lists, creates and removes directories. Failures raise `IOError`, and
every function needs the `filesystem` sandbox capability.

<!-- doc-check: skip -->
```ruby
Dir.mkdir_p("backups/2026")
Dir.entries("backups")   # => ["2026"]

scratch = Dir.mktmpdir()   # => "/tmp/d6-Xq3r9a"
Dir.delete_tree(scratch)   # => true
```

## Functions

### `Dir.delete(path) -> String`

Removes an empty directory and returns `path`.

### `Dir.delete_tree(path) -> Bool`

Removes a directory and everything under it, like `rm -r`, and returns `true`, or
`false` when nothing is at `path`. Symlinks inside are removed as links; what they
point at is never touched. A `path` that is not a directory (a file, or a symlink
even to a directory) raises `IOError`.

### `Dir.entries(path) -> Array[String]`

The names in a directory, without `.` and `..`, in no particular order. It does
not recurse.

### `Dir.mkdir(path, mode = 0o777) -> String`

Creates one directory (the mode is subject to the umask) and returns `path`.
An existing path or a missing parent raises `IOError`.

### `Dir.mkdir_p(path, mode = 0o777) -> String`

Creates the directory and any missing parents, like `mkdir -p`, and returns
`path`. A directory that already exists is fine.

### `Dir.mktmpdir(prefix = "d6-") -> String`

Creates a new directory only you can read (mode `0o700`) under `$TMPDIR`, or
`/tmp`, named `prefix` plus six random characters, and returns its path. Remove it
with `Dir.delete_tree` when you are done. A prefix with a `/` raises `TypeError`.

### `Dir.sync(path) -> String`

Flushes the directory itself to disk, so new or renamed entries survive a
crash, and returns `path`.
