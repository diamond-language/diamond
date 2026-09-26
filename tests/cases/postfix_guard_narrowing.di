# A one-line guard that exits narrows the rest of the function, like the
# block form of if does.
def maybe(flag: Bool) -> String | Nil = if flag then "value" else nil end
def or_default(flag: Bool) -> String
  text = maybe(flag)
  return "none" if text == nil
  text
end
def required(flag: Bool) -> String
  text = maybe(flag)
  raise ArgumentError.new("missing") unless text is String
  text.upcase()
end
def lengths(flags: Array) -> Int
  total = 0
  flags.each() do |flag|
    text = maybe(flag)
    next if text == nil
    total += text.length()
  end
  total
end
[or_default(true), or_default(false), required(true), lengths([true, false, true])]
