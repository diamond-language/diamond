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

def print_solution(matrix: Matrix) -> Int
  [kind, detail] = matrix.solve()
  case kind
  when :unique
    detail.each_with_index() do |value, i| puts("x#{i + 1} = #{value}") end
    0
  when :none
    puts("no solution: the equations contradict each other")
    1
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
