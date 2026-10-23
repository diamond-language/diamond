# Cell names ("A1", "AA12") <-> zero-based (column, row) pairs, and the
# rectangular ranges ("A1:C3") a SUM/AVG/MIN/MAX call spans.

class CellRefError < StandardError
end

# "A" -> 0, "Z" -> 25, "AA" -> 26. Spreadsheet column letters are base 26
# but with no zero digit (A is 1, not 0), so each letter contributes 1-26
# and the final -1 converts to a zero-based index.
def cellref_letters_to_column(letters: String) -> Int
  column = 0
  i = 0

  # Horner's rule: shift what we have by one base-26 place, add the letter.
  while i < letters.length()
    column = column * 26 + (letters[i].upcase().ord() - "A".ord() + 1)
    i += 1
  end
  column - 1
end

# The inverse: 0 -> "A", 25 -> "Z", 26 -> "AA". Because there is no zero
# digit, each step first subtracts 1 so the remainder lands on 0-25 (A-Z),
# then peels off the LAST letter and prepends it.
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

  # Advance to the first digit. No letters ("12") or no digits ("AB") is not
  # a cell name.
  while i < name.length() && !cellref_digit?(name[i])
    i += 1
  end
  if i == 0 || i == name.length()
    raise CellRefError.new("not a cell reference: #{name}")
  end
  [name.slice(0, i), name.slice(i, name.length() - i)]
end

# True for the characters "0" to "9" (character codes 48 to 57).
def cellref_digit?(char: String) -> Bool
  code = char.ord()
  code >= 48 && code <= 57
end

# "B3" -> [1, 2]: zero-based [column, row]. A row of 0 ("A0") is rejected,
# because rows are numbered from 1 in the sheet's own notation.
def cellref_to_col_row(name: String) -> Array
  parts = cellref_split(name)
  column = cellref_letters_to_column(parts[0])
  row = parts[1].to_i() - 1
  if row < 0
    raise CellRefError.new("not a cell reference: #{name}")
  end
  [column, row]
end

# [1, 2] -> "B3".
def cellref_from_col_row(column: Int, row: Int) -> String
  "#{cellref_column_to_letters(column)}#{row + 1}"
end

# Every cell name in the rectangle from `from` to `to`, row-major, both
# corners included -- "A1:B1" and "B1:A1" name the same range.
def cellref_range(from: String, to: String) -> Array
  from_col_row = cellref_to_col_row(from)
  to_col_row = cellref_to_col_row(to)

  # Normalize the two corners into min/max, so the order they were written
  # in does not matter.
  min_col = from_col_row[0] < to_col_row[0] ? from_col_row[0] : to_col_row[0]
  max_col = from_col_row[0] > to_col_row[0] ? from_col_row[0] : to_col_row[0]
  min_row = from_col_row[1] < to_col_row[1] ? from_col_row[1] : to_col_row[1]
  max_row = from_col_row[1] > to_col_row[1] ? from_col_row[1] : to_col_row[1]

  # Walk the rectangle row by row: for each row, every column.
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
