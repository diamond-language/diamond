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

def template_truthy?(value) -> Bool
  if value == nil || value == false
    false
  elsif value is Array
    !value.empty?()
  else
    true
  end
end

def template_stringify(value) -> String
  if value == nil then "" elsif value is String then value else "#{value}" end
end

def template_escape_html(text: String) -> String
  escaped = text.gsub(Regexp.new("&"), "&amp;")
  escaped = escaped.gsub(Regexp.new("<"), "&lt;")
  escaped = escaped.gsub(Regexp.new(">"), "&gt;")
  escaped = escaped.gsub(Regexp.new("\""), "&quot;")
  escaped.gsub(Regexp.new("'"), "&#39;")
end

def render_nodes(nodes: Array, context) -> String
  out = ""
  nodes.each() do |node|
    case node
    when TextNode
      out = out + node.text()
    when VarNode
      text = template_stringify(template_lookup(context, node.path()))
      out = out + (node.escaped?() ? template_escape_html(text) : text)
    when SectionNode
      out = out + render_section(node, context)
    end
  end
  out
end

def render_section(node: SectionNode, context) -> String
  value = template_lookup(context, node.path())
  truthy = template_truthy?(value)
  if node.inverted?()
    return truthy ? "" : render_nodes(node.body(), context)
  end
  return "" unless truthy
  if value is Array
    value.reduce("") do |out, element| out + render_nodes(node.body(), element) end
  else
    render_nodes(node.body(), value is Hash ? value : context)
  end
end
