# Public JSON entry points. JSONCodec is defined by the compatibility core.
module JSON
  module_function

  def stringify(value) -> String = JSONCodec.new().stringify(value)
  # Native (String#parse_json, src/vm.c) -- see its own comment for why.
  def parse(source: String) = source.parse_json()
end
