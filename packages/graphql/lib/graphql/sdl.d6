module GraphQL

  # Prints a schema as GraphQL SDL, for client codegen and review (`schema.to_sdl()`). Types come
  # out in name order and the built-in scalars are left out, so the text is stable from run to run.
  # Resolvers, `resolve_type` callables and scalar coercions are not part of SDL and are not printed.
  class SchemaPrinter
    def self.builtin?(name) = ["String", "Int", "Float", "Boolean", "ID"].include?(name)

    def self.unwrap_non_null(type)
      if type.kind() == "NON_NULL" then type.of_type() else type end
    end

    # A description as a block string, indented `indent` spaces; "" when there is none.
    def self.description(text, indent)
      if text == nil || text == "" then return "" end
      pad = " " * indent
      body = text
      if body.include?("\"\"\"") then body = body.gsub(Regexp.new("\"\"\""), "\\\"\"\"") end
      if body.include?("\n")
        lines = body.split("\n").map() do |line| pad + line end
        "#{pad}\"\"\"\n" + lines.join("\n") + "\n#{pad}\"\"\"\n"
      else
        "#{pad}\"\"\"#{body}\"\"\"\n"
      end
    end

    # A default value as an SDL literal (an enum default is a bare name).
    def self.literal(value, type)
      type = SchemaPrinter.unwrap_non_null(type)
      if value == nil
        "null"
      elsif value is String
        if type.kind() == "ENUM" then value else JSON.stringify(value) end
      elsif value is Array
        item_type = if type.kind() == "LIST" then type.of_type() else type end
        "[" + value.map() do |item| SchemaPrinter.literal(item, item_type) end.join(", ") + "]"
      elsif value is Hash
        "{" + value.keys().map() do |key| "#{key}: #{SchemaPrinter.literal(value[key], type)}" end.join(", ") + "}"
      else
        value.to_s()
      end
    end

    def self.argument(argument)
      text = "#{argument.name()}: #{argument.type().name()}"
      if argument.has_default?() then text = text + " = " + SchemaPrinter.literal(argument.default_value(), argument.type()) end
      text
    end

    # "(a: Int = 1, b: String)" or, when any argument has a description, one argument per line.
    def self.arguments(arguments, indent)
      if arguments.length() == 0 then return "" end
      if arguments.any?() do |argument| argument.description() != nil && argument.description() != "" end
        pad = " " * (indent + 2)
        lines = arguments.map() do |argument|
          SchemaPrinter.description(argument.description(), indent + 2) + pad + SchemaPrinter.argument(argument)
        end
        "(\n" + lines.join("\n") + "\n" + (" " * indent) + ")"
      else
        "(" + arguments.map() do |argument| SchemaPrinter.argument(argument) end.join(", ") + ")"
      end
    end

    def self.fields(fields)
      fields.map() do |field|
        SchemaPrinter.description(field.description(), 2) + "  #{field.name()}" + SchemaPrinter.arguments(field.arguments(), 2) + ": #{field.type().name()}\n"
      end.join("")
    end

    def self.type_definition(type)
      kind = type.kind()
      head = ""
      if kind == "SCALAR"
        head + "scalar #{type.name()}\n"
      elsif kind == "OBJECT"
        implements = if type.interfaces().length() == 0 then "" else " implements " + type.interfaces().map() do |i| i.name() end.join(" & ") end
        head + "type #{type.name()}#{implements}" + (if type.fields().length() == 0 then "\n" else " {\n" + SchemaPrinter.fields(type.fields()) + "}\n" end)
      elsif kind == "INTERFACE"
        head + "interface #{type.name()}" + (if type.fields().length() == 0 then "\n" else " {\n" + SchemaPrinter.fields(type.fields()) + "}\n" end)
      elsif kind == "UNION"
        head + "union #{type.name()} = " + type.possible_types().map() do |member| member.name() end.join(" | ") + "\n"
      elsif kind == "ENUM"
        values = type.values().map() do |value| SchemaPrinter.description(value["description"], 2) + "  #{value["name"]}\n" end.join("")
        head + "enum #{type.name()} {\n" + values + "}\n"
      else
        members = type.arguments().map() do |argument|
          SchemaPrinter.description(argument.description(), 2) + "  " + SchemaPrinter.argument(argument) + "\n"
        end.join("")
        head + "input #{type.name()} {\n" + members + "}\n"
      end
    end

    def self.print(schema)
      query = schema.query_type()
      mutation = schema.mutation_type()
      blocks = []
      custom_roots = (query != nil && query.name() != "Query") || (mutation != nil && mutation.name() != "Mutation")
      if custom_roots
        roots = ""
        if query != nil then roots = roots + "  query: #{query.name()}\n" end
        if mutation != nil then roots = roots + "  mutation: #{mutation.name()}\n" end
        blocks.push("schema {\n" + roots + "}\n")
      end
      types = schema.type_map()
      types.keys().sort().each() do |name|
        if !SchemaPrinter.builtin?(name) then blocks.push(SchemaPrinter.type_definition(types[name])) end
      end
      blocks.join("\n")
    end
  end

end
