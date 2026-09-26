# Two kinds of report row that share no base class. Both satisfy the
# Summary interface just by having its methods, so one table printer
# handles either.
require "./records"
require "./stats"

interface Summary
  def label() -> String
  def points() -> Array[Int]
  def detail() -> String
end

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

class CourseReport
  def initialize(course: String, scores: Array[Score])
    @course = course
    @scores = scores
  end
  def label() -> String = @course
  def points() -> Array[Int] = @scores.map() do |score| score.points() end
  def detail() -> String
    top = best_by(@scores, score_value)
    case top
    when Score then "top: #{top.student()} (#{top.points()})"
    when nil then "no scores"
    end
  end
end

def score_value(score: Score) -> Float = to_f(score.points())

def print_table(title: String, rows: Array)
  puts(title)
  puts("  #{"name".ljust(10, " ")} #{"mean".rjust(6, " ")} #{"median".rjust(6, " ")} grade")
  rows.each() do |row|
    print_row(row)
  end
  puts("")
end

def print_row(row: Summary)
  average = mean(row.points())
  line = "  #{row.label().ljust(10, " ")} #{"%6.1f".format(average)} " +
    "#{"%6.1f".format(median(row.points()))} #{letter(average).ljust(5, " ")} #{row.detail()}"
  puts(line)
end
