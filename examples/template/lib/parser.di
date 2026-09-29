# Turns a template string straight into a Node tree, in one pass: a stack
# of in-progress sections (each a frame holding its own path, `^`/`#`
# flag, and body-so-far), rather than a separate flat-token pass first.
# {{path}} escapes, {{{path}}} doesn't, {{!comment}} is dropped, {{#path}}
# / {{^path}} open a section and {{/path}} closes the innermost one --
# closing the wrong name, or leaving one open at end of input, is a
# TemplateError naming the position.
require "./nodes"

def template_find(text: String, needle: String, start: Int) -> Int | Nil
  return nil if start >= text.length()
  found = text.slice(start, text.length() - start).index_of(needle)
  found == nil ? nil : found + start
end

def template_frame(path: String | Nil, inverted: Bool) -> Hash
  {"path": path, "inverted": inverted, "body": []}
end

def template_parse(text: String) -> Array
  stack = [template_frame(nil, false)]
  pos = 0
  while pos < text.length()
    top = stack[stack.length() - 1]
    open = template_find(text, "{{", pos)
    if open == nil
      top["body"].push(TextNode.new(text.slice(pos, text.length() - pos)))
      pos = text.length()
    else
      if open > pos
        top["body"].push(TextNode.new(text.slice(pos, open - pos)))
      end
      triple = text.slice(open, text.length() - open > 3 ? 3 : text.length() - open) == "{{{"
      tag_start = open + (triple ? 3 : 2)
      close_marker = triple ? "}}}" : "}}"
      close = template_find(text, close_marker, tag_start)
      if close == nil
        raise TemplateError.new("unterminated tag", open)
      end
      inner = text.slice(tag_start, close - tag_start).strip()
      pos = close + close_marker.length()
      if triple
        top["body"].push(VarNode.new(inner, false))
      elsif inner.start_with?("!")
        # comment: nothing rendered
      elsif inner.start_with?("#") || inner.start_with?("^")
        stack.push(template_frame(inner.slice(1, inner.length() - 1).strip(), inner.start_with?("^")))
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
      else
        top["body"].push(VarNode.new(inner, true))
      end
    end
  end
  if stack.length() != 1
    raise TemplateError.new("unclosed section {{##{stack[stack.length() - 1]["path"]}}}", text.length())
  end
  stack[0]["body"]
end
