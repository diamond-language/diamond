# A dense matrix of Fractions with Gauss-Jordan elimination. Because every
# entry is exact, a determinant, an inverse, or a solution comes out as the
# exact fractions, never a float that is nearly right.
#
# `m[i]` is the i-th row (an Array of Fractions) and `m[i] = row` replaces
# it, so `m[i][j]` reads an entry and elimination can swap or rewrite whole
# rows with plain assignment.
require "./fraction"

class Matrix
  # Rows must all be the same length; the check rejects a "ragged" input
  # up front rather than failing mysteriously during elimination.
  def initialize(rows: Array)
    unless rows.empty?()
      width = rows[0].length()
      rows.each() do |row|
        raise ArgumentError.new("ragged matrix: rows of #{width} and #{row.length()}") unless row.length() == width
      end
    end
    @rows = rows
  end

  # The size x size identity: 1 on the diagonal, 0 elsewhere.
  def self.identity(size: Int) -> Matrix
    rows = []
    (0...size).each() do |i|
      rows.push((0...size).map() do |j| Fraction.new(if i == j then 1 else 0 end) end)
    end
    Matrix.new(rows)
  end

  def height() -> Int = @rows.length()
  def width() -> Int = if @rows.empty?() then 0 else @rows[0].length() end

  # Row access with explicit bounds errors. `m[i][j]` is `[]` applied twice;
  # `m[i] = row` calls `[]=`, which also checks the new row's width.
  def [](index: Int) -> Array
    raise IndexError.new("row #{index} out of range 0...#{@rows.length()}") if index < 0 || index >= @rows.length()
    @rows[index]
  end

  def []=(index: Int, row: Array)
    raise IndexError.new("row #{index} out of range 0...#{@rows.length()}") if index < 0 || index >= @rows.length()
    raise ArgumentError.new("row of #{row.length()} entries in a matrix #{self.width()} wide") unless row.length() == self.width()
    @rows[index] = row
  end

  # A copy of the row arrays, so elimination can work without changing the
  # original. (Fractions are immutable, so copying the arrays is enough.)
  def dup_rows() -> Array
    @rows.map() do |row| row.map() do |f| f end end
  end

  # Matrix product: entry (i, j) is row i of self dotted with column j of
  # other. Needs self's width to equal other's height.
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

  # Equal when same shape and every entry equal.
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

    # Work left to right over the first `columns` columns, building one
    # pivot (a leading 1 with zeros above and below) per row where possible.
    while row < work.height() && col < columns
      # Find a row at or below `row` with a non-zero entry in this column.
      # None means this column has no pivot, so move on without using a row.
      pivot_row = nil
      (row...work.height()).each() do |r|
        pivot_row = r if pivot_row == nil && !work[r][col].zero?()
      end
      if pivot_row == nil
        col += 1
        next
      end

      # Swap it into place (this flips the determinant's sign).
      if pivot_row != row
        held = work[row]
        work[row] = work[pivot_row]
        work[pivot_row] = held
        factor = factor.negate()
      end

      # Scale the pivot row so the pivot becomes exactly 1 (this multiplies
      # the determinant by the old pivot value).
      pivot = work[row][col]
      factor = factor * pivot
      work[row] = work[row].map() do |f| f / pivot end

      # Subtract multiples of the pivot row from every OTHER row to zero out
      # this column. Doing it for the rows above too is what makes this
      # Gauss-Jordan (a fully reduced form) rather than plain elimination.
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

  # The determinant falls out of the elimination: if every column found a
  # pivot it is the accumulated factor, otherwise the matrix is singular and
  # the determinant is 0.
  def determinant() -> Fraction
    raise ArgumentError.new("determinant needs a square matrix, not #{self.height()}x#{self.width()}") unless self.height() == self.width()
    [reduced, pivots, factor] = self.reduce(self.width())
    if pivots.length() < self.width() then Fraction.new(0) else factor end
  end

  # Reduces [A | I] (the matrix with an identity glued on the right). If A
  # reduces to the identity on the left, the right half has become the inverse.
  def inverse() -> Matrix
    raise ArgumentError.new("inverse needs a square matrix, not #{self.height()}x#{self.width()}") unless self.height() == self.width()
    n = self.height()
    joined = (0...n).map() do |i| @rows[i] + Matrix.identity(n)[i] end
    [reduced, pivots, factor] = Matrix.new(joined).reduce(n)

    # Fewer pivots than columns means no inverse exists.
    raise ArgumentError.new("matrix is singular") if pivots.length() < n

    # Keep only the right half.
    Matrix.new((0...n).map() do |i| reduced[i].slice(n, 2 * n) end)
  end

  # Treats the last column as the right-hand side of a linear system.
  # Returns [:unique, solution Array], [:none, nil] or [:many, free columns].
  def solve() -> Array
    unknowns = self.width() - 1

    # Reduce only the coefficient columns; the right-hand side rides along.
    [reduced, pivots, factor] = self.reduce(unknowns)

    # Rows below the last pivot have all-zero coefficients. If such a row has a
    # non-zero right-hand side it says "0 = something", a contradiction.
    (pivots.length()...reduced.height()).each() do |r|
      return [:none, nil] unless reduced[r][unknowns].zero?()
    end

    # Consistent but with a column that has no pivot: that unknown is free.
    if pivots.length() < unknowns
      free = (0...unknowns).reject() do |c| pivots.include?(c) end
      return [:many, free]
    end

    # Every unknown has a pivot, so the right-hand column IS the solution.
    [:unique, (0...unknowns).map() do |i| reduced[i][unknowns] end]
  end

  # Right-aligned columns in brackets. Cell text is built first so each
  # column's width (its longest entry) is known before padding.
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
