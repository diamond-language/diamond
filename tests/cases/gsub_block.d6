# sub/gsub with a block: each match goes to the block, and what it returns
# (through to_s) replaces it.
digits = Regexp.new("[0-9]+")
doubled = "a1 b22 c333".gsub(digits) do |n| n.to_i() * 2 end
first = "a1 b22".sub(digits) do |n| "<#{n}>" end
empty = "abc".gsub(Regexp.new("x*")) do |m| "-" end
seen = []
"x1y2".gsub(digits) do |n|
  seen.push(n)
  n
end
def first_long(text: String) -> String
  text.gsub(Regexp.new("[a-z]+")) do |word|
    return word if word.length() > 3
    word
  end
  "none"
end
stopped = "a1 b22 c333".gsub(digits) do |n|
  break "stopped at #{n}" if n.length() == 2
  n
end
[doubled, first, empty, seen, first_long("ab cd efgh ij"), first_long("a b"), stopped]
