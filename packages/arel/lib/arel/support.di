# Immutable SQL AST and SQLite renderer. Query nodes describe intent; only
# SQLiteVisitor knows how that intent becomes SQL.

interface ArelTraversalNode
  def arel_children() -> Array
end

interface ArelInspectable
  def arel_inspect() -> String
end

interface ArelComparableNode
  def arel_same?(other) -> Bool
end

interface ArelReplaceableNode
  def arel_with_children(replacements: Array)
end

def arel_array(value)
  if value is Array
    value
  else
    [value]
  end
end

def arel_quote_identifier(name: String) -> String
  if name.length() == 0
    raise ArgumentError.new("SQL identifier cannot be empty")
  end
  pieces = ["\""]
  characters = name.chars()
  index = 0
  while index < characters.length()
    character = characters[index]
    if character == "\""
      pieces.push("\"")
    end
    pieces.push(character)
    index += 1
  end
  pieces.push("\"")
  pieces.join()
end

def arel_quote_identifier_backtick(name: String) -> String
  if name.length() == 0
    raise ArgumentError.new("SQL identifier cannot be empty")
  end
  pieces = ["`"]
  characters = name.chars()
  index = 0
  while index < characters.length()
    character = characters[index]
    if character == "`"
      pieces.push("`")
    end
    pieces.push(character)
    index += 1
  end
  pieces.push("`")
  pieces.join()
end

def arel_cte_name(value) -> String
  if value is String
    value
  else
    value.name()
  end
end

