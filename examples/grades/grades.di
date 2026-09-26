# grades: a typed grade report from CSV lines of student,course,score.
#
#   diamond grades.di scores.csv [more.csv ...]
#
# Prints per-student and per-course summaries and lists the lines it
# rejected and why. Exits 1 if any line was rejected, 64 for a usage error,
# 66 when a file can't be opened.
require "./lib/reports"

def student_of(score: Score) -> String = score.student()
def course_of(score: Score) -> String = score.course()

def read_lines(path: String) -> Array[String]
  file = File.open(path, "r")
  begin
    file.read().split("\n")
  ensure
    file.close()
  end
end

def grades_main(paths: Array[String]) -> Int
  if paths.empty?()
    warn("usage: grades FILE...")
    return 64
  end
  scores = []
  rejected = []
  number = 0
  paths.each() do |path|
    lines = []
    begin
      lines = read_lines(path)
    rescue error: IOError
      warn("grades: #{error.message()}")
      return 66
    end
    lines.each() do |line|
      number += 1
      next if line.strip().empty?() || line.start_with?("#") || line.start_with?("student,")
      case parse_score(line, number)
      when Parsed{score: score} then scores.push(score)
      when Rejected{number: at, reason: reason} then rejected.push("line #{at}: #{reason}")
      end
    end
  end

  students = group_by_key(scores, student_of)
  courses = group_by_key(scores, course_of)
  print_table("Students", students.keys().sort().map() do |name|
    StudentReport.new(name, students[name])
  end)
  print_table("Courses", courses.keys().sort().map() do |name|
    CourseReport.new(name, courses[name])
  end)

  honor_roll = students.keys().select() do |name|
    mean(students[name].map() do |score| score.points() end) >= 90.0
  end.sort()
  puts("Honor roll: #{if honor_roll.empty?() then "(none)" else honor_roll.join(", ") end}")
  puts("#{scores.length()} scores from #{students.length()} students in #{courses.length()} courses")

  return 0 if rejected.empty?()
  puts("")
  puts("Rejected")
  rejected.each() do |reason| puts("  #{reason}") end
  1
end

exit(grades_main(ARGV))
