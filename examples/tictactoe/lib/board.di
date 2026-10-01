# A board is a 9-element Array of :x, :o, or nil, positions 0-8 read
# left-to-right, top-to-bottom.

class BoardError < StandardError
end

def other_player(player: Symbol) -> Symbol
  player == :x ? :o : :x
end

# The eight winning triples of board positions: three rows, three columns,
# then the two diagonals. board_winner scans these instead of hard-coding
# eight separate comparisons.
WIN_LINES = [[0, 1, 2], [3, 4, 5], [6, 7, 8],
             [0, 3, 6], [1, 4, 7], [2, 5, 8],
             [0, 4, 8], [2, 4, 6]]

# Returns :x or :o if that player owns a whole winning line, else nil.
def board_winner(board: Array) -> Symbol | Nil
  # Find the first line whose three cells are all the same non-empty mark.
  # The `a != nil` check matters: three empty cells are "equal" but are
  # not a win.
  line = WIN_LINES.find() do |positions|
    a = board[positions[0]]
    a != nil && a == board[positions[1]] && a == board[positions[2]]
  end

  # `find` gives back the winning line (or nil); any cell on it names the
  # winner.
  line == nil ? nil : board[line[0]]
end

def board_full?(board: Array) -> Bool
  !board.include?(nil)
end

def board_cell_char(value: Symbol | Nil) -> String
  if value == nil then "." elsif value == :x then "X" else "O" end
end

# Draws the board as three lines of three characters, e.g. "X . O".
def board_render(board: Array) -> String
  rows = []
  row = 0

  # One iteration per board row; row N covers positions 3N, 3N+1, 3N+2.
  while row < 3
    cells = [board_cell_char(board[row * 3]), board_cell_char(board[row * 3 + 1]),
              board_cell_char(board[row * 3 + 2])]
    rows.push(cells.join(" "))
    row += 1
  end

  rows.join("\n")
end

# Reads a 9-character string ("." empty, X, O, any case) into a board.
# Raises BoardError on a wrong length or any other character.
def board_parse(text: String) -> Array
  unless text.length() == 9
    raise BoardError.new("a board is exactly 9 characters (. for empty, X, O)")
  end

  board = []
  index = 0

  # Convert each character in turn. The push comes before the validity
  # check, but that is harmless: an invalid character raises on the very
  # next statement, so the half-built board is discarded.
  while index < 9
    char = text[index].upcase()
    board.push(char == "." ? nil : (char == "X" ? :x : (char == "O" ? :o : nil)))
    unless char == "." || char == "X" || char == "O"
      raise BoardError.new("unexpected '#{text[index]}' at position #{index}")
    end
    index += 1
  end

  board
end
