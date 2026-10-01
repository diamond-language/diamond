# grades: a typed grade report from CSV lines of student,course,score.
#
#   diamond grades.di scores.csv [more.csv ...]
#
# Prints per-student and per-course summaries and lists the lines it
# rejected and why. Exits 1 if any line was rejected, 64 for a usage error,
# 66 when a file can't be opened.
require "./lib/reports"

# Key functions for group_by_key (it takes a function, not a method name).
def student_of(score: Score) -> String = score.student()
def course_of(score: Score) -> String = score.course()

# The file's lines; `ensure` closes it even if reading fails.
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

  # Phase 1: read every file, sorting each line into a good Score or a
  # rejection message. `number` counts lines across ALL files, so a reported
  # line number is a position in the combined input.
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

      # Skip blanks, comments, and a CSV header row.
      next if line.strip().empty?() || line.start_with?("#") || line.start_with?("student,")

      # Exhaustive over the sealed ParseResult; the `Parsed{score: score}`
      # pattern pulls the score out of the object.
      case parse_score(line, number)
      when Parsed{score: score} then scores.push(score)
      when Rejected{number: at, reason: reason} then rejected.push("line #{at}: #{reason}")
      end
    end
  end

  # Phase 2: group the scores by student and by course, and print a table for
  # each. Rows are sorted by name; a report object wraps one group.
  students = group_by_key(scores, student_of)
  courses = group_by_key(scores, course_of)
  print_table("Students", students.keys().sort().map() do |name|
    StudentReport.new(name, students[name])
  end)
  print_table("Courses", courses.keys().sort().map() do |name|
    CourseReport.new(name, courses[name])
  end)

  # Phase 3: summary lines. Honor roll = students whose mean is 90 or more.
  honor_roll = students.keys().select() do |name|
    mean(students[name].map() do |score| score.points() end) >= 90.0
  end.sort()
  puts("Honor roll: #{if honor_roll.empty?() then "(none)" else honor_roll.join(", ") end}")
  puts("#{scores.length()} scores from #{students.length()} students in #{courses.length()} courses")

  # Phase 4: list what was rejected and why; any rejection makes the exit
  # status 1.
  return 0 if rejected.empty?()
  puts("")
  puts("Rejected")
  rejected.each() do |reason| puts("  #{reason}") end
  1
end

exit(grades_main(ARGV))
