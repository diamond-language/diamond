# One parsed line of input, and the result of trying to parse one.
# ParseResult is sealed, so every `case` over it must handle both kinds.

# A valid line. Student and course are stored lowercased, so "Ada" and "ada"
# are one student.
struct Score(student: String, course: String, points: Int)
end

sealed class ParseResult
end

# The two outcomes of parsing a line.
class Parsed < ParseResult
  attr_reader score: Score
  def initialize(score: Score)
    @score = score
  end
end

class Rejected < ParseResult
  attr_reader number: Int
  attr_reader reason: String
  def initialize(number: Int, reason: String)
    @number = number
    @reason = reason
  end
end

# "ada,math,93" -> Parsed; anything else -> Rejected with a reason.
def parse_score(line: String, number: Int) -> ParseResult
  # Trim every field, then validate in order, returning the first problem
  # found (each rejection says what is wrong).
  fields = line.split(",").map() do |field| field.strip() end
  return Rejected.new(number, "expected 3 fields, got #{fields.length()}") if fields.length() != 3
  [student, course, points_text] = fields
  return Rejected.new(number, "missing student") if student.empty?()
  return Rejected.new(number, "missing course") if course.empty?()

  # Digits only: this also rejects "-5" and "9.5". The regexp comes first
  # because to_i() would turn "abc" into 0 without complaint.
  unless Regexp.new("^[0-9]+$").match?(points_text)
    return Rejected.new(number, "score '#{points_text}' is not a whole number")
  end
  points = points_text.to_i()
  return Rejected.new(number, "score #{points} is over 100") if points > 100
  Parsed.new(Score.new(student.downcase(), course.downcase(), points))
end
