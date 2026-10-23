# ParseResult is sealed with two subclasses; a case that forgets Rejected
# doesn't compile.
require "../lib/records"
def count_parsed(result: ParseResult) -> Int
  case result
  when Parsed then 1
  end
end
count_parsed(parse_score("ada,math,90", 1))
