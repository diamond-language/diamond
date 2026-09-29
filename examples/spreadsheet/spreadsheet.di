# spreadsheet: load a sheet of NAME = FORMULA lines, evaluate every
# cell, and print the grid -- or just one cell with --cell.
#
# A sheet file looks like:
#
#   A1 = 12
#   A2 = 30
#   A3 = 5
#   B1 = "total"
#   B2 = =SUM(A1:A3)
#
# What this shows: a recursive-descent formula parser (lib/parser.di,
# lib/lexer.di) building a sealed AST (lib/formula.di); memoized recursive
# evaluation over a dependency graph that is never built as a separate
# structure -- value_of() calling itself through eval_node() through
# value_of() again *is* the graph walk, with @cache making each cell pay
# for its own evaluation only once no matter how many other cells depend
# on it; and cycle detection (lib/sheet.di's @visiting stack) that catches
# a self-reference at any depth, not just A1 = =A1 directly.

require "./lib/cellref"
require "./lib/formula"
require "./lib/lexer"
require "./lib/sheet"

def usage() -> Int
  warn("usage: spreadsheet.di SHEETFILE [--cell NAME]")
  64
end

def load_sheet(path: String) -> Array
  sheet = Sheet.new()
  max_col = -1
  max_row = -1
  lines = File.open(path, "r").read().split("\n")
  line_number = 0
  while line_number < lines.length()
    trimmed = lines[line_number].strip()
    unless trimmed.empty?() || trimmed.start_with?("#")
      begin
        equals = trimmed.index_of("=")
        if equals == nil
          raise FormulaError.new("expected 'NAME = value'", 0)
        end
        name = trimmed.slice(0, equals).strip()
        raw_text = trimmed.slice(equals + 1, trimmed.length() - equals - 1).strip()
        col_row = cellref_to_col_row(name)
        sheet.set(name, raw_text)
        max_col = col_row[0] > max_col ? col_row[0] : max_col
        max_row = col_row[1] > max_row ? col_row[1] : max_row
      rescue error: FormulaError | CellRefError
        raise FormulaError.new("line #{line_number + 1}: #{error.message()}", 0)
      end
    end
    line_number += 1
  end
  [sheet, max_col, max_row]
end

def format_value(value: Float | String) -> String
  if value is String
    value
  else
    text = "#{value}"
    text.end_with?(".0") ? text.slice(0, text.length() - 2) : text
  end
end

def print_grid(sheet: Sheet, max_col: Int, max_row: Int)
  rendered = {}
  width = 1
  row = 0
  while row <= max_row
    column = 0
    while column <= max_col
      name = cellref_from_col_row(column, row)
      text = sheet.names().include?(name) ? format_value(sheet.value_of(name)) : ""
      rendered[name] = text
      width = text.length() > width ? text.length() : width
      column += 1
    end
    row += 1
  end
  row_label_width = "#{max_row + 1}".length()
  header = " " * (row_label_width + 1)
  column = 0
  while column <= max_col
    header = header + cellref_column_to_letters(column).rjust(width + 1)
    column += 1
  end
  puts(header)
  row = 0
  while row <= max_row
    line = "#{row + 1}".rjust(row_label_width) + " "
    column = 0
    while column <= max_col
      line = line + rendered[cellref_from_col_row(column, row)].rjust(width + 1)
      column += 1
    end
    puts(line)
    row += 1
  end
end

def main(argv) -> Int
  return usage() if argv.empty?()
  path = argv[0]
  cell = nil
  index = 1
  while index < argv.length()
    if argv[index] == "--cell" && index + 1 < argv.length()
      cell = argv[index + 1]
      index += 2
    else
      return usage()
    end
  end
  begin
    loaded = load_sheet(path)
    sheet = loaded[0]
    if cell == nil
      print_grid(sheet, loaded[1], loaded[2])
    else
      puts(format_value(sheet.value_of(cell)))
    end
    0
  rescue error: IOError
    warn(error.message())
    66
  rescue error: FormulaError | CellRefError | CircularReferenceError
    warn(error.message())
    65
  end
end

exit(main(ARGV))
