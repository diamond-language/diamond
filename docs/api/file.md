# File

[API reference](index.md) · Next: [Dir](dir.md)

`File` reads and writes files, inspects paths and works with the filesystem.
Functions that touch the filesystem raise `IOError` on failure, with the
operating system's reason in the message, and are refused under `--sandbox`
unless the `filesystem` capability is allowed (see [sandbox](../sandbox.md)).
The path functions (`join`, `dirname`, `basename`, `extname`, `absolute?`,
`expand_path`) only manipulate strings.

[Local I/O](../local-io.md) explains the durability functions (`publish`,
`rename`, `sync`) in more depth.

<!-- doc-check: skip -->
```ruby
File.write("notes.txt", "first line\n")
text = File.read("notes.txt")

f = File.open("log.txt", "a")
f.write("started\n")
f.close()
```

## Reading and writing whole files

### `File.read(path) -> String`

The file's whole contents.

### `File.write(path, text) -> Int`

Replaces the file's contents with `text`, creating it if needed, and returns
the number of bytes written.

### `File.embed(path) -> String`

The contents of `path` read when the program is compiled, so a built binary or
bytecode cache carries them. `path` is a string literal, resolved against the
directory of the file the call is in. Up to 16 MiB.

<!-- doc-check: skip -->
```ruby
SCHEMA = File.embed("schema.graphql")
```

## Open files

### `File.open(path, mode) -> File`

Opens `path` with a C `fopen` mode: `"r"`, `"w"`, `"a"`, `"r+"` and so on.
Unreachable handles are closed by the garbage collector, but close them
yourself.

### `read() -> String`

### `read(n) -> String`

The rest of the file, or up to `n` bytes of it.

### `gets() -> String | Nil`

The next line without its line ending, or `nil` at the end.

### `write(value)`

Writes `value`, converted to text as `print` does.

### `flush() -> File`

Hands buffered writes to the operating system, so other readers see them.
`File.sync` forces them to disk.

### `close()`

Closes the file. Closing it again does nothing; any other call on a closed
file raises `IOError`.

### `shutdown_write()`

### `peer_address() -> String | Nil`

On a blocking TCP connection, which is a File: see
[Networking](network.md#socket-methods). On other files they raise `IOError`.

## Paths

### `File.absolute?(path) -> Bool`

Whether `path` starts with `/`.

```ruby
File.absolute?("/a/b")   # => true
```

### `File.basename(path, suffix = nil) -> String`

The last part of `path`, without `suffix` when it ends with exactly that.

```ruby
File.basename("/a/b/c.rb")          # => "c.rb"
File.basename("/a/b/c.rb", ".rb")   # => "c"
```

### `File.dirname(path) -> String`

Everything before the last part; `"."` when there is no `/`.

```ruby
File.dirname("/a/b/c")   # => "/a/b"
File.dirname("c")        # => "."
```

### `File.expand_path(path, base = nil) -> String`

`path` as an absolute, normalized path: joined onto `base`, or onto the
current directory, with `.` and `..` resolved.

```ruby
File.expand_path("../b", "/a/x")   # => "/a/b"
```

### `File.extname(path) -> String`

The extension of the last part, dot included; `""` when there is none or the
name is a dotfile.

```ruby
File.extname("archive.tar.gz")   # => ".gz"
File.extname(".bashrc")          # => ""
```

### `File.join(part, ...) -> String`

The parts joined with `/`, skipping empty parts and collapsing doubled
separators.

```ruby
File.join("a/", "/b/", "c")   # => "a/b/c"
```

## Inspecting the filesystem

### `File.directory?(path) -> Bool`

Whether `path` is a directory. A missing path is `false`, not an error.

### `File.exist?(path) -> Bool`

Whether anything exists at `path`.

### `File.file?(path) -> Bool`

Whether `path` is a regular file, or a symlink to one.

### `File.mode(path) -> Int`

The permission bits, such as `0o644`.

### `File.sha256(path) -> String`

The lowercase hex SHA-256 of the file, read in chunks.

### `File.size(path) -> Int`

The size in bytes.

### `File.symlink?(path) -> Bool`

Whether `path` is itself a symlink.

## Changing the filesystem

### `File.chmod(path, mode) -> String`

Sets the permission bits and returns `path`.

### `File.copy(from, to) -> String`

Copies a regular file to a new path, keeping its permission bits, and returns
`to`. An existing `to` raises `IOError` instead of being replaced.

### `File.delete(path) -> Bool`

Removes a file. Returns `false` when it was already gone.

### `File.publish(path, text) -> String`

Creates `path` with exactly `text`, atomically and durably: readers never see
a partial file, and an existing path raises `IOError` instead of being
replaced. Returns `path`.

### `File.rename(from, to) -> String`

Renames `from` to `to`, atomically replacing any file at `to`, and returns
`to`. Writing a temporary file and renaming it over the original is the safe
way to update a file.

### `File.sync(path) -> String`

Flushes a regular file and its directory to disk and returns `path`.
