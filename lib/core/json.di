# Public JSON entry points. JSONCodec is defined by the compatibility core.
module JSON
  module_function

  def stringify(value) -> String = JSONCodec.new().stringify(value)
  def parse(source: String) = JSONCodec.new().parse(source)
end
