# packages/multipart

`multipart/form-data` request-body parsing for
[Diamond](https://gitlab.com/dmn9180/diamond) — the piece
[`packages/dials`](../dials/README.md)'s own `Dials::Params` doesn't
cover (it only ever parses `application/x-www-form-urlencoded` bodies).
Pure Diamond, no VM changes: Diamond's own `String` is already a raw,
binary-safe byte buffer ([local I/O](../../docs/local-io.md)), and `String#split` already
handles a multi-byte separator correctly — that's all real multipart
parsing actually needs.

## Install

Same story as every other package here — copy this directory into
another project as `cuts/multipart/`, or give it its own git remote and
depend on it via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Use

A route expecting a file upload calls `multipart_parse(request)`
directly — alongside, not through, `Dials::Router`'s own `params`
argument (that argument comes from `Dials::Params`, which can't parse a
multipart body at all):

```ruby
require "/path/to/multipart/lib/multipart"

class SkinsController
  def self.create(request, context, params)
    upload = multipart_parse(request)
    if upload == nil
      return [400, {"Content-Type": "text/plain"}, "expected multipart/form-data"]
    end
    title = upload["fields"]["title"]
    file = upload["files"]["theme_file"]
    # file => {"filename": "theme.zip", "content_type": "application/zip",
    #          "data": "<raw uploaded bytes>"}
    ...
  end
end
```

`multipart_parse(request)` returns `nil` if the request isn't
`multipart/form-data` at all (wrong or missing `Content-Type`) — lets a
caller fall back to `Dials::Params` for an ordinary form post — or a
malformed body of some other kind (the boundary never actually appears,
or a part is missing its own header/content separator, or a part has no
`Content-Disposition` name). A malformed body fails the *whole* parse
rather than partially succeeding, so a caller always gets either a
fully-parsed request or a clean "reject this" signal, never a
half-populated `Hash` to reason about.

Otherwise, it returns `{"fields": Hash, "files": Hash}`:
- `"fields"` maps a plain field name to its `String` value.
- `"files"` maps a file field's name to `{"filename", "content_type",
  "data"}` — `"data"` is the raw uploaded bytes; `"content_type"`
  defaults to `"application/octet-stream"` when the part didn't send its
  own (matching `packages/rack`'s own `StaticFiles` default for the same
  "we genuinely don't know" case).

See `packages/http/lib/http/status.di`'s `http_max_body_size` for the
one hard cap on how large a request body — and therefore an upload —
can be at all; this package does no size-limiting of its own beyond
that.

## Saving an uploaded file to disk

Generalized from `applications/skindicate.dia`'s own hand-rolled
`upload_extension`/`save_uploaded_file` — same behavior, just not tied
to that app's own upload directory:

```ruby
file = upload["files"]["theme_file"]
stored_name = multipart_save_file(file, ["zip", "itheme", "deskthemepack"], "/opt/myapp/public/uploads")
# => "3f9c2a...e1.zip", already written to
#    /opt/myapp/public/uploads/3f9c2a...e1.zip -- or nil if `file` is
#    nil (field wasn't submitted) or its extension isn't allowed
#    (nothing is written in that case)
```

The stored filename is always freshly random
(`SecureRandom.hex(16)` + the original extension), never the submitted
original filename — avoiding both collisions between two uploads and any
path-traversal surface from a user-controlled name, the same reasoning
`packages/rack`'s own `StaticFiles` rejects a `".."` request path for on
the read side. Keep the real original filename (for display/download
purposes) in your own model/database instead — it never touches the
filesystem here. The extension check is basic abuse prevention on what
an upload endpoint accepts, not a sandbox — pair this with a static-file
server that never executes anything it serves if code execution is a
concern at all.

`multipart_file_extension(filename)` is the lowercased extension with no
leading `.` (`"a.b.ZIP"` -> `"zip"`), or `nil` if there isn't one.

## What's deliberately out of scope

- **A backslash-escaped quote inside a filename.** `filename="a \"b\"
  c.zip"` isn't handled — the value extraction stops at the first `"`,
  full stop. Every other character, including spaces and non-ASCII
  bytes, round-trips correctly; only a literal embedded `"` breaks it.
- **Nested `multipart/mixed` parts** (multiple files under one field
  name). Not needed for one file per named field, which is the only
  shape this package builds.
- **Header line-folding** (an obsolete HTTP header continuation form).
  Every header this package looks at is expected on one line.
