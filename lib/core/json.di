# Public JSON entry points. JSONCodec is defined by the compatibility core.
module JSON
  module_function

  # Native (diamond_json_stringify, src/vm.c) -- see json_codec.di.
  def stringify(value) -> String = diamond_json_stringify(value)
  # Native (String#parse_json, src/vm.c) -- see its own comment for why.
  def parse(source: String) = source.parse_json()
end
