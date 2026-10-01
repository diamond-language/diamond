# Turns a template string straight into a Node tree, in one pass: a stack
# of in-progress sections (each a frame holding its own path, `^`/`#`
# flag, and body-so-far), rather than a separate flat-token pass first.
# {{path}} escapes, {{{path}}} doesn't, {{!comment}} is dropped, {{#path}}
# / {{^path}} open a section and {{/path}} closes the innermost one --
# closing the wrong name, or leaving one open at end of input, is a
# TemplateError naming the position.
require "./nodes"

# The index of the first `needle` at or after `start`, or nil. (Searches a
# slice and adds the offset back, since index_of here has no start argument.)
def template_find(text: String, needle: String, start: Int) -> Int | Nil
  return nil if start >= text.length()
  found = text.slice(start, text.length() - start).index_of(needle)
  found == nil ? nil : found + start
end

# A section being built: its name, whether it is `^`, and the nodes so far.
# The first frame (path nil) is the whole template.
def template_frame(path: String | Nil, inverted: Bool) -> Hash
  {"path": path, "inverted": inverted, "body": []}
end

def template_parse(text: String) -> Array
  # `stack` holds the sections currently open, innermost last; everything
  # parsed goes into the innermost one's body.
  stack = [template_frame(nil, false)]
  pos = 0

  while pos < text.length()
    top = stack[stack.length() - 1]
    open = template_find(text, "{{", pos)

    # No more tags: the rest of the text is literal.
    if open == nil
      top["body"].push(TextNode.new(text.slice(pos, text.length() - pos)))
      pos = text.length()
    else
      # Literal text before the tag.
      if open > pos
        top["body"].push(TextNode.new(text.slice(pos, open - pos)))
      end

      # `{{{` (three braces) means unescaped, and then must close with `}}}`.
      # The slice length is clamped so a "{{" at the very end of the input
      # does not read past it.
      triple = text.slice(open, text.length() - open > 3 ? 3 : text.length() - open) == "{{{"
      tag_start = open + (triple ? 3 : 2)
      close_marker = triple ? "}}}" : "}}"
      close = template_find(text, close_marker, tag_start)
      if close == nil
        raise TemplateError.new("unterminated tag", open)
      end

      # The tag's own text, and where to resume afterwards. Then decide by
      # its first character what kind of tag it is.
      inner = text.slice(tag_start, close - tag_start).strip()
      pos = close + close_marker.length()

      if triple
        top["body"].push(VarNode.new(inner, false))
      elsif inner.start_with?("!")
        # comment: nothing rendered
      # Opening a section: push a new frame; what follows belongs to it.
      elsif inner.start_with?("#") || inner.start_with?("^")
        stack.push(template_frame(inner.slice(1, inner.length() - 1).strip(), inner.start_with?("^")))
      # Closing: pop the frame, check it is the one being closed (so
      # sections nest properly), and append it, now complete, to its
      # parent's body. A close with only the root frame open is stray.
      elsif inner.start_with?("/")
        closed_path = inner.slice(1, inner.length() - 1).strip()
        if stack.length() <= 1
          raise TemplateError.new("unmatched closing tag {{/#{closed_path}}}", open)
        end
        frame = stack.pop()
        unless frame["path"] == closed_path
          raise TemplateError.new("expected {{/#{frame["path"]}}} but found {{/#{closed_path}}}", open)
        end
        stack[stack.length() - 1]["body"].push(SectionNode.new(frame["path"], frame["inverted"], frame["body"]))
      # Anything else is an ordinary escaped variable.
      else
        top["body"].push(VarNode.new(inner, true))
      end
    end
  end

  # At the end only the root frame should remain; otherwise a section was
  # never closed.
  if stack.length() != 1
    raise TemplateError.new("unclosed section {{##{stack[stack.length() - 1]["path"]}}}", text.length())
  end
  stack[0]["body"]
end
