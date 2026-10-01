# exact: linear algebra over exact fractions.
#
# exact.di solve FILE      solve the linear system in FILE
# exact.di det FILE        determinant of the square matrix in FILE
# exact.di inverse FILE    inverse of the square matrix in FILE
#
# FILE holds one matrix row per line, entries separated by spaces, each an
# integer or a fraction like 3/4. Blank lines and lines starting with # are
# skipped. For `solve`, the last column is the right-hand side.
require "./lib/matrix"

def usage() -> Int
  warn("usage: exact.di solve|det|inverse FILE")
  64
end

# Reads a matrix: one row per line, entries separated by spaces. Blank and
# `#` lines are skipped; each entry goes through Fraction.parse, so a bad
# entry raises ArgumentError naming it.
def read_matrix(path: String) -> Matrix
  rows = []

  File.read(path).split("\n").each() do |line|
    text = line.strip()
    next if text.empty?() || text.start_with?("#")
    rows.push(text.split(" ").reject() do |w| w.empty?() end.map() do |w| Fraction.parse(w) end)
  end
  raise ArgumentError.new("#{path} has no matrix rows") if rows.empty?()
  Matrix.new(rows)
end

# `solve` returns [kind, detail]: the kind says which of the three cases of a
# linear system this is, and the detail depends on it.
def print_solution(matrix: Matrix) -> Int
  [kind, detail] = matrix.solve()

  case kind
  # One solution: `detail` is the list of values.
  when :unique
    detail.each_with_index() do |value, i| puts("x#{i + 1} = #{value}") end
    0
  # Contradictory equations (e.g. x = 1 and x = 2).
  when :none
    puts("no solution: the equations contradict each other")
    1
  # Fewer independent equations than unknowns: `detail` lists the columns
  # that are free to take any value.
  else
    names = detail.map() do |c| "x#{c + 1}" end.join(", ")
    puts("infinitely many solutions: #{names} can be chosen freely")
    1
  end
end

def run(command: String, path: String) -> Int
  matrix = read_matrix(path)

  case command
  when "solve" then print_solution(matrix)
  when "det"
    puts(matrix.determinant())
    0
  when "inverse"
    inverse = matrix.inverse()
    puts(inverse)
    # The inverse proves itself: matrix * inverse must be exactly the identity.
    puts("check: matrix * inverse == identity: #{matrix * inverse == Matrix.identity(matrix.height())}")
    0
  else
    usage()
  end
end

def main(argv) -> Int
  return usage() unless argv.length() == 2

  # Bad numbers, ragged rows, a singular matrix or a wrong shape are all "bad
  # data" (65); an unreadable file is 66.
  begin
    run(argv[0], argv[1])
  rescue error: ArgumentError
    warn(error.message())
    65
  rescue error: IndexError
    warn(error.message())
    65
  rescue error: IOError
    warn(error.message())
    66
  end
end

exit(main(ARGV))
