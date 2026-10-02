# The rendered tree. Node is sealed, so render_nodes (lib/render.di)
# stops compiling the moment a fourth kind is added and not handled.

sealed class Node
end

# Literal text, copied through unchanged.
class TextNode < Node
  def initialize(text: String)
    @text = text
  end
  def text() = @text
end

# {{path}} (escaped) or {{{path}}} (not escaped).
class VarNode < Node
  def initialize(path: String, escaped: Bool)
    @path = path
    @escaped = escaped
  end
  def path() = @path
  def escaped?() = @escaped
end

# {{#path}}...{{/path}}, or {{^path}}...{{/path}} when inverted. `body` is the
# list of nodes between the tags (which may contain more sections).
class SectionNode < Node
  def initialize(path: String, inverted: Bool, body: Array)
    @path = path
    @inverted = inverted
    @body = body
  end
  def path() = @path
  def inverted?() = @inverted
  def body() = @body
end

# A malformed template. `position` is the character offset of the problem.
class TemplateError < StandardError
  def initialize(message, position)
    super(message)
    @position = position
  end
  def position() = @position
end
