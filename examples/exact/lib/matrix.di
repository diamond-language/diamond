# A dense matrix of Fractions with Gauss-Jordan elimination. Because every
# entry is exact, a determinant, an inverse, or a solution comes out as the
# exact fractions, never a float that is nearly right.
#
# `m[i]` is the i-th row (an Array of Fractions) and `m[i] = row` replaces
# it, so `m[i][j]` reads an entry and elimination can swap or rewrite whole
# rows with plain assignment.
require "./fraction"

class Matrix
  def initialize(rows: Array)
    unless rows.empty?()
      width = rows[0].length()
      rows.each() do |row|
        raise ArgumentError.new("ragged matrix: rows of #{width} and #{row.length()}") unless row.length() == width
      end
    end
    @rows = rows
  end

  def self.identity(size: Int) -> Matrix
    rows = []
    (0...size).each() do |i|
      rows.push((0...size).map() do |j| Fraction.new(if i == j then 1 else 0 end) end)
    end
    Matrix.new(rows)
  end

  def height() -> Int = @rows.length()
  def width() -> Int = if @rows.empty?() then 0 else @rows[0].length() end

  def [](index: Int) -> Array
    raise IndexError.new("row #{index} out of range 0...#{@rows.length()}") if index < 0 || index >= @rows.length()
    @rows[index]
  end

  def []=(index: Int, row: Array)
    raise IndexError.new("row #{index} out of range 0...#{@rows.length()}") if index < 0 || index >= @rows.length()
    raise ArgumentError.new("row of #{row.length()} entries in a matrix #{self.width()} wide") unless row.length() == self.width()
    @rows[index] = row
  end

  def dup_rows() -> Array
    @rows.map() do |row| row.map() do |f| f end end
  end

  def *(other: Matrix) -> Matrix
    unless self.width() == other.height()
      raise ArgumentError.new("cannot multiply #{self.height()}x#{self.width()} by #{other.height()}x#{other.width()}")
    end
    rows = []
    (0...self.height()).each() do |i|
      rows.push((0...other.width()).map() do |j|
        total = Fraction.new(0)
        (0...self.width()).each() do |k|
          total = total + @rows[i][k] * other[k][j]
        end
        total
      end)
    end
    Matrix.new(rows)
  end

  def ==(other)
    return false unless other is Matrix && other.height() == self.height() && other.width() == self.width()
    (0...self.height()).all?() do |i|
      (0...self.width()).all?() do |j| @rows[i][j] == other[i][j] end
    end
  end

  # Reduced row echelon form, plus the determinant factor it accumulated:
  # every row swap flips its sign and every pivot scaling multiplies it in.
  # Returns [reduced matrix, pivot column per pivot row, determinant factor].
  def reduce(columns: Int) -> Array
    work = Matrix.new(self.dup_rows())
    pivots = []
    factor = Fraction.new(1)
    row = 0
    col = 0
    while row < work.height() && col < columns
      pivot_row = nil
      (row...work.height()).each() do |r|
        pivot_row = r if pivot_row == nil && !work[r][col].zero?()
      end
      if pivot_row == nil
        col += 1
        next
      end
      if pivot_row != row
        held = work[row]
        work[row] = work[pivot_row]
        work[pivot_row] = held
        factor = factor.negate()
      end
      pivot = work[row][col]
      factor = factor * pivot
      work[row] = work[row].map() do |f| f / pivot end
      (0...work.height()).each() do |r|
        if r != row && !work[r][col].zero?()
          scale = work[r][col]
          work[r] = (0...work.width()).map() do |c| work[r][c] - scale * work[row][c] end
        end
      end
      pivots.push(col)
      row += 1
      col += 1
    end
    [work, pivots, factor]
  end

  def determinant() -> Fraction
    raise ArgumentError.new("determinant needs a square matrix, not #{self.height()}x#{self.width()}") unless self.height() == self.width()
    [reduced, pivots, factor] = self.reduce(self.width())
    if pivots.length() < self.width() then Fraction.new(0) else factor end
  end

  def inverse() -> Matrix
    raise ArgumentError.new("inverse needs a square matrix, not #{self.height()}x#{self.width()}") unless self.height() == self.width()
    n = self.height()
    joined = (0...n).map() do |i| @rows[i] + Matrix.identity(n)[i] end
    [reduced, pivots, factor] = Matrix.new(joined).reduce(n)
    raise ArgumentError.new("matrix is singular") if pivots.length() < n
    Matrix.new((0...n).map() do |i| reduced[i].slice(n, 2 * n) end)
  end

  # Treats the last column as the right-hand side of a linear system.
  # Returns [:unique, solution Array], [:none, nil] or [:many, free columns].
  def solve() -> Array
    unknowns = self.width() - 1
    [reduced, pivots, factor] = self.reduce(unknowns)
    (pivots.length()...reduced.height()).each() do |r|
      return [:none, nil] unless reduced[r][unknowns].zero?()
    end
    if pivots.length() < unknowns
      free = (0...unknowns).reject() do |c| pivots.include?(c) end
      return [:many, free]
    end
    [:unique, (0...unknowns).map() do |i| reduced[i][unknowns] end]
  end

  def to_s() -> String
    cells = @rows.map() do |row| row.map() do |f| f.to_s() end end
    widths = (0...self.width()).map() do |j|
      cells.map() do |row| row[j].length() end.max()
    end
    cells.map() do |row|
      "[ " + (0...row.length()).map() do |j| row[j].rjust(widths[j], " ") end.join("  ") + " ]"
    end.join("\n")
  end
end
