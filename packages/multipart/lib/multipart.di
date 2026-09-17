# multipart/form-data parsing for Diamond -- pure Diamond, no VM
# changes needed: Diamond's own String is already documented as a raw,
# binary-safe byte buffer (docs/io.md), and String#split already
# handles a multi-byte separator correctly (confirmed directly:
# "a--X--b--X--c".split("--X--") == ["a","b","c"]), which is all real
# multipart parsing actually needs.
#
# `packages/dials`' own Dials::Params only ever parses
# application/x-www-form-urlencoded bodies -- a route expecting a file
# upload calls multipart_parse(request) directly instead, alongside (not
# through) the router's own `params` argument. See
# packages/http/lib/http/status.di's http_max_body_size for the one
# hard cap on how large a request body (and therefore an upload) can be
# at all -- this package does no size-limiting of its own beyond that.

# Extracts the boundary value out of a Content-Type header, or nil if
# the header is missing, isn't multipart/form-data, or has no boundary
# at all. Stops at a trailing ";" (e.g. "; charset=..."), not just
# "everything after boundary=" -- a real request can have parameters
# after boundary as well as before it. Strips a surrounding quote pair
# if present, though real browsers don't quote it.
def multipart_boundary(content_type)
  if content_type == nil
    return nil
  end
  if !content_type.start_with?("multipart/form-data")
    return nil
  end
  marker = "boundary="
  start = content_type.index_of(marker)
  if start == nil
    return nil
  end
  value_start = start + marker.length()
  rest = content_type.slice(value_start, content_type.length() - value_start)
  semicolon = rest.index_of(";")
  boundary = if semicolon == nil then rest else rest.slice(0, semicolon) end
  boundary = boundary.strip()
  if boundary.length() >= 2 && boundary.start_with?("\"") && boundary.end_with?("\"")
    boundary = boundary.slice(1, boundary.length() - 2)
  end
  if boundary == ""
    nil
  else
    boundary
  end
end

# Pulls `key="value"` out of one header line (e.g. `name`/`filename` out
# of a Content-Disposition line) via plain index_of/slice, the same
# style packages/cookies' own cookie_parse already uses -- no Regexp
# needed. Returns nil if `key="` doesn't appear at all, or if the
# opening quote is never closed. Does not handle a backslash-escaped
# quote *inside* the value (a filename containing a literal `"`) --
# a deliberate scope cut, matching this package's "keep it minimal"
# brief; every other character, including spaces and non-ASCII bytes,
# round-trips correctly.
def multipart_extract_quoted(line, key)
  marker = "#{key}=\""
  start = line.index_of(marker)
  if start == nil
    return nil
  end
  value_start = start + marker.length()
  rest = line.slice(value_start, line.length() - value_start)
  quote_end = rest.index_of("\"")
  if quote_end == nil
    return nil
  end
  rest.slice(0, quote_end)
end

# Parses one part's own header block (everything before the blank line
# that separates it from its content) into {"name", "filename",
# "content_type"} -- the three things multipart_parse itself needs.
# `filename`/`content_type` are nil when the part is an ordinary field,
# not a file.
def multipart_parse_part_headers(header_block)
  name = nil
  filename = nil
  content_type = nil
  def check_header_line(line)
    if line.start_with?("Content-Disposition:")
      name = multipart_extract_quoted(line, "name")
      filename = multipart_extract_quoted(line, "filename")
    elsif line.start_with?("Content-Type:")
      value_start = "Content-Type:".length()
      content_type = line.slice(value_start, line.length() - value_start).strip()
    end
  end
  header_block.split("\r\n").each(check_header_line)
  {"name": name, "filename": filename, "content_type": content_type}
end

# The whole request -> {"fields": Hash, "files": Hash} -- or nil if the
# request isn't a (parseable) multipart/form-data request at all, so a
# caller can fall back to Dials::Params itself for an ordinary form
# post. "fields" maps a plain field name to its String value -- except
# a name ending in "[]" (the standard HTML-forms array convention,
# e.g. several checkboxes all named "platforms[]"), which maps the
# "[]"-stripped key to an Array of every value seen for that name, in
# submission order (a single "platforms[]" part still becomes a
# one-element Array, not a bare String -- the "[]" suffix is what a
# caller opts into, not how many parts actually showed up). "files"
# maps a file field's name to {"filename", "content_type", "data"} (the
# raw uploaded bytes, "content_type" defaulting to
# "application/octet-stream" when the part didn't send its own) --
# except a name ending in "[]" (e.g. a real `<input type="file"
# multiple name="screenshots[]">`), which gets the exact same array
# treatment "fields" does: the "[]"-stripped key maps to an Array of
# every file part seen for that name, in submission order, and a
# single such part still becomes a one-element Array. This is each
# individual part carrying repeated `name="x[]"` Content-Disposition
# headers (what every real browser sends for a multi-file input), not a
# nested multipart/mixed part wrapping several files inside one part --
# that second, rarer shape (a single part whose own body is itself a
# multipart/mixed document) stays out of scope.
#
# A malformed body (the boundary never actually appears, or a part is
# missing its own blank-line header/content separator, or a part has no
# Content-Disposition name at all) fails the *whole* parse -- returns
# nil rather than partially succeeding, so a caller always gets either a
# fully-parsed request or a clean "reject this" signal, never a
# half-populated Hash to reason about.
def multipart_parse(request)
  boundary = multipart_boundary(request["headers"]["content-type"])
  if boundary == nil
    return nil
  end
  delimiter = "--#{boundary}"
  parts = request["body"].split(delimiter)
  if parts.length() < 2
    return nil
  end
  fields = {}
  files = {}
  index = 1
  while index < parts.length() - 1
    part = parts[index]
    if part.start_with?("\r\n")
      part = part.slice(2, part.length() - 2)
    end
    if part.end_with?("\r\n")
      part = part.slice(0, part.length() - 2)
    end
    separator = part.index_of("\r\n\r\n")
    if separator == nil
      return nil
    end
    header_block = part.slice(0, separator)
    content_start = separator + 4
    content = part.slice(content_start, part.length() - content_start)
    headers = multipart_parse_part_headers(header_block)
    name = headers["name"]
    if name == nil
      return nil
    end
    filename = headers["filename"]
    if filename == nil
      if name.end_with?("[]")
        array_name = name.slice(0, name.length() - 2)
        existing = fields[array_name]
        if existing == nil
          fields[array_name] = [content]
        else
          existing.push(content)
        end
      else
        fields[name] = content
      end
    else
      content_type = headers["content_type"]
      content_type = if content_type == nil then "application/octet-stream" else content_type end
      file = {"filename": filename, "content_type": content_type, "data": content}
      if name.end_with?("[]")
        array_name = name.slice(0, name.length() - 2)
        existing = files[array_name]
        if existing == nil
          files[array_name] = [file]
        else
          existing.push(file)
        end
      else
        files[name] = file
      end
    end
    index = index + 1
  end
  {"fields": fields, "files": files}
end

# Saving a parsed upload to disk -- generalized from
# applications/skindicate.dia's own upload_extension/save_uploaded_file,
# unchanged in behavior. Kept in this file rather than split out: two
# small functions, no new dependency, and multipart_save_file's own
# `file` parameter is exactly one of multipart_parse's own "files" Hash
# entries -- the two are one concern (this package's whole job is
# turning a multipart body into something a route handler can use),
# not two.

# The lowercased extension of `filename` (no leading "."), or nil if it
# has none at all -- `"a.b.ZIP"` -> `"zip"`, `"noext"` -> nil.
def multipart_file_extension(filename)
  reversed = filename.reverse()
  dot_from_end = reversed.index_of(".")
  if dot_from_end == nil
    return nil
  end
  filename.slice(filename.length() - dot_from_end, dot_from_end).downcase()
end

# Writes one multipart_parse "files" entry to a freshly random name
# (SecureRandom.hex(16) plus its original extension) under `directory`,
# returning that stored filename (not the full path -- join it onto
# whatever URL/directory prefix the caller serves `directory` back out
# from) on success. Returns nil -- writes nothing -- if `file` is nil
# (the field wasn't submitted) or its extension isn't in
# `allowed_extensions`.
#
# The extension check is basic abuse prevention on what an upload
# endpoint accepts, not a sandbox -- pair this with a static-file server
# that never executes anything it serves (e.g. packages/rack's own
# StaticFiles) if code execution is a concern at all. The on-disk name
# is always freshly random, never the submitted original filename --
# avoiding both collisions between two uploads and any path-traversal
# surface from a user-controlled name, the same reasoning StaticFiles
# itself rejects a ".." request path for on the read side. Keep the real
# original filename (for display/download-name purposes) in your own
# model/database instead -- it never touches the filesystem here.
def multipart_save_file(file, allowed_extensions, directory)
  if file == nil
    return nil
  end
  extension = multipart_file_extension(file["filename"])
  if extension == nil || !allowed_extensions.include?(extension)
    return nil
  end
  stored_name = "#{SecureRandom.hex(16)}.#{extension}"
  handle = File.open("#{directory}/#{stored_name}", "w")
  handle.write(file["data"])
  handle.close()
  stored_name
end
