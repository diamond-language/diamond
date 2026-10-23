# Two kinds of report row that share no base class. Both satisfy the
# Summary interface just by having its methods, so one table printer
# handles either.
require "./records"
require "./stats"

# What print_table needs from a row: a name, the scores to average, and a
# description. A class satisfies this just by having these three methods; it
# never has to declare that it does.
interface Summary
  def label() -> String
  def points() -> Array[Int]
  def detail() -> String
end

# One student's row: detail lists their courses.
class StudentReport
  def initialize(student: String, scores: Array[Score])
    @student = student
    @scores = scores
  end
  def label() -> String = @student
  def points() -> Array[Int] = @scores.map() do |score| score.points() end
  def detail() -> String
    courses = @scores.map() do |score| score.course() end.sort()
    "#{courses.length()} course(s): #{courses.join(", ")}"
  end
end

# One course's row: detail names its top scorer.
class CourseReport
  def initialize(course: String, scores: Array[Score])
    @course = course
    @scores = scores
  end
  def label() -> String = @course
  def points() -> Array[Int] = @scores.map() do |score| score.points() end
  def detail() -> String
    # best_by returns a Score or nil, and `case` must handle both.
    top = best_by(@scores, score_value)
    case top
    when Score then "top: #{top.student()} (#{top.points()})"
    when nil then "no scores"
    end
  end
end

# The measure best_by compares scores by (it wants a Float).
def score_value(score: Score) -> Float = to_f(score.points())

# A titled table, one row per report. `rows` can mix StudentReports and
# CourseReports freely, because print_row only needs the Summary methods.
def print_table(title: String, rows: Array)
  puts(title)
  puts("  #{"name".ljust(10, " ")} #{"mean".rjust(6, " ")} #{"median".rjust(6, " ")} grade")
  rows.each() do |row|
    print_row(row)
  end
  puts("")
end

# One row: name, mean, median, letter grade, then the row's own detail.
def print_row(row: Summary)
  average = mean(row.points())
  line = "  #{row.label().ljust(10, " ")} #{"%6.1f".format(average)} " +
    "#{"%6.1f".format(median(row.points()))} #{letter(average).ljust(5, " ")} #{row.detail()}"
  puts(line)
end
