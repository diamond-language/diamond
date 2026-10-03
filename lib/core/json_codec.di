# Parsing and stringification both live natively now: String#parse_json
# and diamond_json_stringify (src/vm.c) -- see their own comments for why
# (the pure-Diamond forms cost ~330ms/MB to parse and ~11us per small
# object to stringify, and the stringifier's recursion hit
# DIAMOND_MAX_CALL_DEPTH at ~44 nesting levels). JSONError itself is a
# builtin (DIAMOND_CLASS_JSON_ERROR), so native code raises it the same way
# every other native error class already does -- no class declaration
# needed here.
#
# JSONCodec is kept as the compatibility entry point JSON.stringify used to
# go through; it carries no state and no logic of its own.
class JSONCodec
  def stringify(value) -> String = diamond_json_stringify(value)
end
