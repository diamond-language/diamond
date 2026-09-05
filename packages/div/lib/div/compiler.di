# Translates `.html.div` (ERB-style) template source into an ordinary `.di`
# source file -- a hand-rolled `index_of`/`slice` tag scanner, not Regexp
# (Diamond's Regexp has no String integration -- no `=~`, no `.match`
# against a String receiver -- so a scanner built on String's own
# `index_of`/`slice` is the natural fit here, see docs/syntax.md).
#
# Tag syntax (see packages/div/README.md for the full writeup):
#   <%# locals: a, b %>   -- once per file, becomes the render function's
#                            own param list
#   <%# comment %>        -- dropped from output
#   <%= expr %>           -- HTML-escaped output
#   <%== expr %>          -- raw, unescaped output
#   <% code %>            -- verbatim statement code, no output
#   anything else         -- literal text
#
# Deliberately narrow, matching this repo's own pattern of scoped first
# slices (delegate, compile_method, closure): no whitespace-trim tags, no
# layout/yield mechanism (a compiled template is an ordinary function, so
# one render function calling another already covers partials with no
# special mechanism needed), and tag scanning stops at the first "%>" --
# a tag whose own code contains that literal substring (inside a nested
# string, say) will terminate early. See packages/div/ROADMAP.md.
#
# Every top-level name a generated file defines is derived from the
# input file's own basename (function_name_for below), not a fixed
# "render" -- `require`'s compile-time expansion merges every required
# file into one flat, shared top-level function namespace (see
# docs/roadmap.md's "Forward and mutual calls"), so two templates
# compiled to the same fixed name would collide the moment an app
# `require`s more than one of them together, and that's exactly the
# common case (a page template requiring a partial). Two different
# input files sharing a basename in different directories used to
# collide the same way -- resolved by function_name_for_relative below,
# used whenever the caller (bin/divc_all.sh, walking a template tree)
# knows the file's path relative to that tree's own root; a bare
# basename-only function_name_for(path) call, from someone invoking
# bin/divc.di directly on a single file with no meaningful root, is
# unaffected and still collides the old way -- not a namespacing system
# on its own, just what divc_all.sh's own root-relative path buys for
# free once it's passed through.
module Div
  module_function

  # DIAMOND_MAX_STRING_LENGTH is 255 bytes (src/vm.h) and escaping a raw
  # chunk can expand it (each byte can become up to 2-3 escaped bytes) --
  # 150 raw chars per literal chunk is a deliberately conservative margin
  # under that cap, not an exact byte computation.
  def literal_chunk_size()
    150
  end

  # Any byte outside [A-Za-z0-9_] becomes "_" -- originally scoped to
  # just "." and "-" (the two characters most likely in a plain
  # filename: "index.html.div" via its stripped-".div" base
  # "index.html", or a hyphenated name), widened to a real character
  # class once function_name_for_relative started feeding whole
  # directory segments through here too, not just basenames -- a
  # directory name is far more likely to carry a character (a space,
  # say -- see test.sh's own "nested folder" fixture) that would
  # otherwise land in a generated `def` line as an outright syntax
  # error, not just an unconventional identifier.
  def sanitize_identifier(text)
    sb = StringBuilder.new()
    i = 0
    while i < text.length()
      ch = text[i]
      code = ch.ord()
      is_safe = (code >= 97 && code <= 122) || (code >= 65 && code <= 90) || (code >= 48 && code <= 57) || ch == "_"
      if is_safe
        sb.append(ch)
      else
        sb.append("_")
      end
      i += 1
    end
    sb.to_s()
  end

  # "views/index.html.div" -> "index_html"; used both as the generated
  # render function's own name and as a per-file suffix on the inlined
  # escape helper (see escape_helper_lines) so two templates required
  # together don't collide on either name.
  def function_name_for(path)
    parts = path.split("/")
    base = parts[parts.length() - 1]
    if base.length() > 4 && base.slice(base.length() - 4, 4) == ".div"
      base = base.slice(0, base.length() - 4)
    end
    Div.sanitize_identifier(base)
  end

  # Like function_name_for, but qualifies the name with every directory
  # segment of `path` instead of just the basename -- "skins/show.html.div"
  # -> "skins_show_html", not "show_html". `path` here is expected to
  # already be relative to whatever root the caller considers meaningful
  # (bin/divc_all.sh passes each file's path relative to the tree root it
  # was told to walk); a path with no "/" at all (a file directly under
  # that root) produces exactly the same name function_name_for would,
  # so this is a strict superset, not a different scheme, for anything
  # not actually nested in a subdirectory.
  def function_name_for_relative(path)
    parts = path.split("/")
    last_index = parts.length() - 1
    base = parts[last_index]
    if base.length() > 4 && base.slice(base.length() - 4, 4) == ".div"
      base = base.slice(0, base.length() - 4)
    end
    parts[last_index] = base
    sanitized = parts.map() do |part| Div.sanitize_identifier(part) end
    sanitized.join("_")
  end

  # Scans `source` into an ordered Array of ["text", content] / ["tag",
  # content] tokens.
  def scan(source)
    tokens = []
    pos = 0
    length = source.length()
    while pos < length
      remaining = source.slice(pos, length - pos)
      tag_rel = remaining.index_of("<%")
      if tag_rel == nil
        tokens << ["text", source.slice(pos, length - pos)]
        pos = length
      else
        if tag_rel > 0
          tokens << ["text", source.slice(pos, tag_rel)]
        end
        tag_start = pos + tag_rel
        after_open = tag_start + 2
        remaining2 = source.slice(after_open, length - after_open)
        close_rel = remaining2.index_of("%>")
        if close_rel == nil
          raise "div: unterminated <% tag starting at offset #{tag_start}"
        end
        tokens << ["tag", source.slice(after_open, close_rel)]
        pos = after_open + close_rel + 2
      end
    end
    tokens
  end

  def extract_locals(tokens)
    locals = []
    tokens.each() do |token|
      if token[0] == "tag"
        body = token[1].strip()
        if body.length() >= 1 && body[0] == "#"
          directive = body.slice(1, body.length() - 1).strip()
          if directive.length() >= 7 && directive.slice(0, 7) == "locals:"
            names = directive.slice(7, directive.length() - 7).split(",")
            names.each() do |name|
              trimmed = name.strip()
              if trimmed.length() > 0
                locals << trimmed
              end
            end
          end
        end
      end
    end
    locals
  end

  # Escapes raw template text so it's safe to embed as the content of a
  # Diamond string literal in the generated file. Diamond's only legal
  # string escapes are \n \r \t \" \\ \# (src/compiler.c) -- every `#` is
  # always escaped here (not just ones followed by `{`), which is safe
  # (`\#` alone is already a legal escape) and avoids needing lookahead.
  def escape_di_string(raw)
    sb = StringBuilder.new()
    i = 0
    while i < raw.length()
      ch = raw[i]
      if ch == "\\"
        sb.append("\\\\")
      elsif ch == "\""
        sb.append("\\\"")
      elsif ch == "#"
        sb.append("\\#")
      elsif ch == "\n"
        sb.append("\\n")
      elsif ch == "\r"
        sb.append("\\r")
      elsif ch == "\t"
        sb.append("\\t")
      else
        sb.append(ch)
      end
      i += 1
    end
    sb.to_s()
  end

  def emit_literal_chunks(text, lines)
    pos = 0
    length = text.length()
    chunk_size = Div.literal_chunk_size()
    while pos < length
      chunk_len = length - pos
      if chunk_len > chunk_size
        chunk_len = chunk_size
      end
      chunk = text.slice(pos, chunk_len)
      escaped = Div.escape_di_string(chunk)
      lines << "  sb.append(\"#{escaped}\")"
      pos = pos + chunk_len
    end
  end

  def emit_tag(body, lines, escape_fn_name)
    trimmed = body.strip()
    if trimmed.length() >= 1 && trimmed[0] == "#"
      # <%# ... %> -- comment or the `locals:` directive, either way
      # already handled by extract_locals and dropped from output here.
    elsif trimmed.length() >= 2 && trimmed.slice(0, 2) == "=="
      expr = trimmed.slice(2, trimmed.length() - 2).strip()
      lines << "  sb.append(#{expr})"
    elsif trimmed.length() >= 1 && trimmed[0] == "="
      expr = trimmed.slice(1, trimmed.length() - 1).strip()
      lines << "  sb.append(#{escape_fn_name}(#{expr}))"
    else
      lines << trimmed
    end
  end

  def emit_body(tokens, escape_fn_name)
    lines = []
    tokens.each() do |token|
      if token[0] == "text"
        Div.emit_literal_chunks(token[1], lines)
      else
        Div.emit_tag(token[1], lines, escape_fn_name)
      end
    end
    lines
  end

  # Source lines for the escape helper inlined into every generated file,
  # named `escape_fn_name` (see function_name_for's own comment on why
  # this can't be a fixed name) -- kept behaviorally identical to
  # Div.escape_html in lib/div/runtime.di (see that file's own comment,
  # and test.sh's cross-check). Inlined rather than `require`d so a
  # generated file has zero runtime dependency on this package -- the
  # translator is a build-time-only tool.
  def escape_helper_lines(escape_fn_name)
    [
      "def #{escape_fn_name}(value)",
      "  s = \"\#{value}\"",
      "  sb = StringBuilder.new()",
      "  i = 0",
      "  while i < s.length()",
      "    ch = s[i]",
      "    if ch == \"&\"",
      "      sb.append(\"&amp;\")",
      "    elsif ch == \"<\"",
      "      sb.append(\"&lt;\")",
      "    elsif ch == \">\"",
      "      sb.append(\"&gt;\")",
      "    elsif ch == \"\\\"\"",
      "      sb.append(\"&quot;\")",
      "    elsif ch == \"'\"",
      "      sb.append(\"&#39;\")",
      "    else",
      "      sb.append(ch)",
      "    end",
      "    i += 1",
      "  end",
      "  sb.to_s()",
      "end"
    ]
  end

  def compile_source(source, function_name)
    escape_fn_name = "div_escape_html__#{function_name}"
    tokens = Div.scan(source)
    locals = Div.extract_locals(tokens)
    body_lines = Div.emit_body(tokens, escape_fn_name)

    out = []
    Div.escape_helper_lines(escape_fn_name).each() do |line|
      out << line
    end
    out << ""
    out << "def #{function_name}(#{locals.join(", ")})"
    out << "  sb = StringBuilder.new()"
    body_lines.each() do |line|
      out << line
    end
    out << "  sb.to_s()"
    out << "end"
    out.join("\n") + "\n"
  end

  # `name_path`, when given, is used instead of `input_path` to derive
  # the generated function's own name -- via function_name_for_relative,
  # not function_name_for, so a caller passing one actually gets
  # directory-qualified naming rather than the same basename-only result
  # either function would give a path with no "/" in it. Omitted (the
  # default), this is exactly the old behavior: name derived from
  # `input_path`'s own basename only.
  def compile_file(input_path, output_path, name_path = nil)
    input = File.open(input_path, "r")
    source = input.read()
    input.close()
    function_name = if name_path == nil then Div.function_name_for(input_path) else Div.function_name_for_relative(name_path) end
    generated = Div.compile_source(source, function_name)
    output = File.open(output_path, "w")
    output.write(generated)
    output.close()
  end
end
