# Resolves a dotted path against a Hash context, and walks a Node tree
# into a String against it.
require "./nodes"

# "." is the implicit-iterator path (Mustache's own name for it): the
# current context itself, used inside a section over an array of plain
# values rather than an array of Hashes. Anything else walks the path one
# dotted part at a time, missing at any step reads as nil rather than
# raising -- a template author's typo in a path shouldn't crash the
# render, the same forgiving direction real Mustache implementations take.
def template_lookup(context, path: String)
  return context if path == "."
  parts = path.split(".")
  current = context
  index = 0
  while index < parts.length() && current is Hash
    current = current.fetch(parts[index], nil)
    index += 1
  end
  index == parts.length() ? current : nil
end

# Mustache truthiness: nil, false and an empty Array are false; everything
# else (including 0 and "") is true.
def template_truthy?(value) -> Bool
  if value == nil || value == false
    false
  elsif value is Array
    !value.empty?()
  else
    true
  end
end

# How a value prints: nothing for nil, strings as-is, anything else via
# interpolation.
def template_stringify(value) -> String
  if value == nil then "" elsif value is String then value else "#{value}" end
end

# Escapes the five HTML-significant characters. `&` must go first so the
# entities added by the later rules are not themselves escaped.
def template_escape_html(text: String) -> String
  escaped = text.gsub(Regexp.new("&"), "&amp;")
  escaped = escaped.gsub(Regexp.new("<"), "&lt;")
  escaped = escaped.gsub(Regexp.new(">"), "&gt;")
  escaped = escaped.gsub(Regexp.new("\""), "&quot;")
  escaped.gsub(Regexp.new("'"), "&#39;")
end

# Renders a list of nodes against `context`. Exhaustive over the sealed Node.
def render_nodes(nodes: Array, context) -> String
  out = ""

  nodes.each() do |node|
    case node
    when TextNode
      out = out + node.text()
    # A variable: look it up, convert to text, and escape unless it was
    # written {{{...}}}.
    when VarNode
      text = template_stringify(template_lookup(context, node.path()))
      out = out + (node.escaped?() ? template_escape_html(text) : text)
    when SectionNode
      out = out + render_section(node, context)
    end
  end
  out
end

# Renders a section. The looked-up value decides how many times, and with
# what context, the body renders.
def render_section(node: SectionNode, context) -> String
  value = template_lookup(context, node.path())
  truthy = template_truthy?(value)

  # An inverted section renders (once, in the current context) only when the
  # value is NOT truthy.
  if node.inverted?()
    return truthy ? "" : render_nodes(node.body(), context)
  end
  return "" unless truthy

  # An Array repeats the body once per element, each as the new context. A
  # Hash becomes the new context; any other truthy value (true, a number...)
  # renders once with the context unchanged.
  if value is Array
    value.reduce("") do |out, element| out + render_nodes(node.body(), element) end
  else
    render_nodes(node.body(), value is Hash ? value : context)
  end
end
