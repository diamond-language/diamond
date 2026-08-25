# Hand-written recursive descent over Lexer's own token Array, producing
# a Document (nodes.di). Query-document grammar only -- see nodes.di's
# own header for why there's no SDL support here.
module GraphQL
module Language

class ParseError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

class Parser
  def initialize(tokens)
    @tokens = tokens
    @pos = 0
  end

  def self.parse(source: String)
    tokens = Lexer.tokenize(source)
    parser = Parser.new(tokens)
    parser.parse_document()
  end

  def current() = @tokens[@pos]
  def peek_kind() = self.current().kind()
  def peek_value() = self.current().value()

  def advance()
    tok = self.current()
    unless tok.kind() == "EOF"
      @pos += 1
    end
    tok
  end

  def at_punct(text) = self.peek_kind() == "PUNCT" && self.peek_value() == text
  def at_name(text) = self.peek_kind() == "NAME" && self.peek_value() == text

  def fail_expected(what)
    tok = self.current()
    raise ParseError.new("expected #{what}, found #{tok.kind()} \"#{tok.value()}\" (line #{tok.line()})")
  end

  def expect_punct(text)
    unless self.at_punct(text)
      self.fail_expected("\"#{text}\"")
    end
    self.advance()
  end

  def expect_keyword(text)
    unless self.at_name(text)
      self.fail_expected("\"#{text}\"")
    end
    self.advance()
  end

  def expect_name()
    unless self.peek_kind() == "NAME"
      self.fail_expected("a name")
    end
    self.advance().value()
  end

  def parse_document()
    definitions = []
    while self.peek_kind() != "EOF"
      definitions.push(self.parse_definition())
    end
    Document.new(definitions)
  end

  def parse_definition()
    if self.at_punct("{") || self.at_name("query") || self.at_name("mutation") ||
       self.at_name("subscription")
      self.parse_operation_definition()
    elsif self.at_name("fragment")
      self.parse_fragment_definition()
    else
      self.fail_expected("a query, mutation, subscription, or fragment definition")
    end
  end

  def parse_operation_definition()
    if self.at_punct("{")
      selection_set = self.parse_selection_set()
      return OperationDefinition.new("query", nil, [], [], selection_set)
    end
    operation = self.advance().value()
    name = nil
    if self.peek_kind() == "NAME"
      name = self.advance().value()
    end
    variable_definitions = []
    if self.at_punct("(")
      variable_definitions = self.parse_variable_definitions()
    end
    directives = self.parse_directives()
    selection_set = self.parse_selection_set()
    OperationDefinition.new(operation, name, variable_definitions, directives, selection_set)
  end

  def parse_variable_definitions()
    self.expect_punct("(")
    definitions = []
    while !self.at_punct(")")
      definitions.push(self.parse_variable_definition())
    end
    self.expect_punct(")")
    definitions
  end

  def parse_variable_definition()
    self.expect_punct("$")
    name = self.expect_name()
    self.expect_punct(":")
    type = self.parse_type_reference()
    default_value = nil
    if self.at_punct("=")
      self.advance()
      default_value = self.parse_value()
    end
    VariableDefinition.new(name, type, default_value)
  end

  # Handles arbitrary nesting (`[[String!]]!`) via plain recursion for
  # the inner type, then wrapping in NonNullType if a trailing `!`
  # follows -- matches how nodes.di's NamedType/ListType/NonNullType
  # compose.
  def parse_type_reference()
    if self.at_punct("[")
      self.advance()
      inner = self.parse_type_reference()
      self.expect_punct("]")
      type = ListType.new(inner)
    else
      type = NamedType.new(self.expect_name())
    end
    if self.at_punct("!")
      self.advance()
      type = NonNullType.new(type)
    end
    type
  end

  def parse_directives()
    directives = []
    while self.at_punct("@")
      directives.push(self.parse_directive())
    end
    directives
  end

  def parse_directive()
    self.expect_punct("@")
    name = self.expect_name()
    arguments = []
    if self.at_punct("(")
      arguments = self.parse_arguments()
    end
    Directive.new(name, arguments)
  end

  def parse_arguments()
    self.expect_punct("(")
    arguments = []
    while !self.at_punct(")")
      arguments.push(self.parse_argument())
    end
    self.expect_punct(")")
    arguments
  end

  def parse_argument()
    name = self.expect_name()
    self.expect_punct(":")
    value = self.parse_value()
    Argument.new(name, value)
  end

  def parse_selection_set()
    self.expect_punct("{")
    selections = []
    while !self.at_punct("}")
      selections.push(self.parse_selection())
    end
    self.expect_punct("}")
    selections
  end

  def parse_selection()
    if self.at_punct("...")
      self.parse_fragment_or_inline()
    else
      self.parse_field()
    end
  end

  def parse_field()
    first = self.expect_name()
    alias_name = nil
    name = first
    if self.at_punct(":")
      self.advance()
      alias_name = first
      name = self.expect_name()
    end
    arguments = []
    if self.at_punct("(")
      arguments = self.parse_arguments()
    end
    directives = self.parse_directives()
    selection_set = nil
    if self.at_punct("{")
      selection_set = self.parse_selection_set()
    end
    Field.new(alias_name, name, arguments, directives, selection_set)
  end

  # A fragment can never be named "on" (the grammar excludes it), so
  # "..." followed by the NAME "on" always means a typed inline
  # fragment, never a spread of a fragment literally named "on" -- the
  # same disambiguation the spec itself relies on.
  def parse_fragment_or_inline()
    self.expect_punct("...")
    if self.at_name("on")
      self.advance()
      type_condition = self.expect_name()
      directives = self.parse_directives()
      selection_set = self.parse_selection_set()
      InlineFragment.new(type_condition, directives, selection_set)
    elsif self.peek_kind() == "NAME"
      name = self.advance().value()
      directives = self.parse_directives()
      FragmentSpread.new(name, directives)
    else
      directives = self.parse_directives()
      selection_set = self.parse_selection_set()
      InlineFragment.new(nil, directives, selection_set)
    end
  end

  def parse_fragment_definition()
    self.advance()
    name = self.expect_name()
    self.expect_keyword("on")
    type_condition = self.expect_name()
    directives = self.parse_directives()
    selection_set = self.parse_selection_set()
    FragmentDefinition.new(name, type_condition, directives, selection_set)
  end

  # Literal scalars (Int/Float/String/Bool) come back as plain native
  # Diamond values -- only the kinds execution needs to distinguish from
  # an ordinary literal get a wrapper node (see nodes.di's own header).
  def parse_value()
    kind = self.peek_kind()
    if kind == "INT"
      self.advance().value().to_i()
    elsif kind == "FLOAT"
      self.advance().value().to_f()
    elsif kind == "STRING"
      self.advance().value()
    elsif self.at_punct("$")
      self.advance()
      Variable.new(self.expect_name())
    elsif self.at_punct("[")
      self.parse_list_value()
    elsif self.at_punct("{")
      self.parse_object_value()
    elsif kind == "NAME"
      self.parse_name_value()
    else
      self.fail_expected("a value")
    end
  end

  def parse_name_value()
    text = self.peek_value()
    if text == "true"
      self.advance()
      true
    elsif text == "false"
      self.advance()
      false
    elsif text == "null"
      self.advance()
      NullValue.new()
    else
      self.advance()
      EnumValue.new(text)
    end
  end

  def parse_list_value()
    self.expect_punct("[")
    values = []
    while !self.at_punct("]")
      values.push(self.parse_value())
    end
    self.expect_punct("]")
    ListValue.new(values)
  end

  def parse_object_value()
    self.expect_punct("{")
    fields = []
    while !self.at_punct("}")
      name = self.expect_name()
      self.expect_punct(":")
      value = self.parse_value()
      fields.push(ObjectField.new(name, value))
    end
    self.expect_punct("}")
    ObjectValue.new(fields)
  end
end

end
end
