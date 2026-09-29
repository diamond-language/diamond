# Cell names ("A1", "AA12") <-> zero-based (column, row) pairs, and the
# rectangular ranges ("A1:C3") a SUM/AVG/MIN/MAX call spans.

class CellRefError < StandardError
end

def cellref_letters_to_column(letters: String) -> Int
  column = 0
  i = 0
  while i < letters.length()
    column = column * 26 + (letters[i].upcase().ord() - "A".ord() + 1)
    i += 1
  end
  column - 1
end

def cellref_column_to_letters(column: Int) -> String
  n = column + 1
  letters = ""
  while n > 0
    n = n - 1
    letters = (("A".ord() + n % 26).chr()) + letters
    n = n / 26
  end
  letters
end

# Splits "AA12" into ("AA", "12"): letters, then the first digit onward.
def cellref_split(name: String) -> Array
  i = 0
  while i < name.length() && !cellref_digit?(name[i])
    i += 1
  end
  if i == 0 || i == name.length()
    raise CellRefError.new("not a cell reference: #{name}")
  end
  [name.slice(0, i), name.slice(i, name.length() - i)]
end

def cellref_digit?(char: String) -> Bool
  code = char.ord()
  code >= 48 && code <= 57
end

def cellref_to_col_row(name: String) -> Array
  parts = cellref_split(name)
  column = cellref_letters_to_column(parts[0])
  row = parts[1].to_i() - 1
  if row < 0
    raise CellRefError.new("not a cell reference: #{name}")
  end
  [column, row]
end

def cellref_from_col_row(column: Int, row: Int) -> String
  "#{cellref_column_to_letters(column)}#{row + 1}"
end

# Every cell name in the rectangle from `from` to `to`, row-major, both
# corners included -- "A1:B1" and "B1:A1" name the same range.
def cellref_range(from: String, to: String) -> Array
  from_col_row = cellref_to_col_row(from)
  to_col_row = cellref_to_col_row(to)
  min_col = from_col_row[0] < to_col_row[0] ? from_col_row[0] : to_col_row[0]
  max_col = from_col_row[0] > to_col_row[0] ? from_col_row[0] : to_col_row[0]
  min_row = from_col_row[1] < to_col_row[1] ? from_col_row[1] : to_col_row[1]
  max_row = from_col_row[1] > to_col_row[1] ? from_col_row[1] : to_col_row[1]
  names = []
  row = min_row
  while row <= max_row
    column = min_col
    while column <= max_col
      names.push(cellref_from_col_row(column, row))
      column += 1
    end
    row += 1
  end
  names
end
