# The rendered tree. Node is sealed, so render_nodes (lib/render.di)
# stops compiling the moment a fourth kind is added and not handled.

sealed class Node
end

class TextNode < Node
  def initialize(text: String)
    @text = text
  end
  def text() = @text
end

class VarNode < Node
  def initialize(path: String, escaped: Bool)
    @path = path
    @escaped = escaped
  end
  def path() = @path
  def escaped?() = @escaped
end

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

class TemplateError < StandardError
  def initialize(message, position)
    super(message)
    @position = position
  end
  def position() = @position
end
