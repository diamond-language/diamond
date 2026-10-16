# JSON text with sorted keys and two-space indentation, the layout the reviewed inventory
# files use (and byte-for-byte what `json.dumps(value, indent=2, sort_keys=True)` produced).
def json_pretty(value, level)
  indent = " " * (2 * (level + 1))
  closing = " " * (2 * level)
  if value.is_a?(Hash)
    return "{}" if value.length() == 0
    parts = value.keys().sort().map() do |key|
      indent + JSON.stringify(key) + ": " + json_pretty(value[key], level + 1)
    end
    return "{\n" + parts.join(",\n") + "\n" + closing + "}"
  end
  if value.is_a?(Array)
    return "[]" if value.length() == 0
    parts = value.map() do |item|
      indent + json_pretty(item, level + 1)
    end
    return "[\n" + parts.join(",\n") + "\n" + closing + "]"
  end
  JSON.stringify(value)
end
