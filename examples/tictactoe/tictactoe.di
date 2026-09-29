# tictactoe: perfect-play tic-tac-toe by exhaustive minimax.
#
# --self          play a full game, X and O both playing perfectly.
#                 Perfect play always draws -- this is the classic proof
#                 of that, by exhaustive search rather than by hand.
# --best BOARD    given a 9-character board (. empty, X, O; whoever has
#                 fewer marks moves next), print the best move and what
#                 it's worth.
require "./lib/board"
require "./lib/minimax"

def usage() -> Int
  warn("usage: tictactoe.di --self")
  warn("       tictactoe.di --best BOARD   (9 chars: . X O)")
  64
end

def play_self() -> Int
  board = [nil, nil, nil, nil, nil, nil, nil, nil, nil]
  player = :x
  puts(board_render(board))
  while board_winner(board) == nil && !board_full?(board)
    move = minimax(board, player, player).move()
    board[move] = player
    puts("")
    puts("#{player} plays #{move}")
    puts(board_render(board))
    player = other_player(player)
  end
  winner = board_winner(board)
  puts("")
  puts(winner == nil ? "draw" : "#{winner} wins")
  0
end

def whose_turn(board: Array) -> Symbol
  x_count = board.count() do |cell| cell == :x end
  o_count = board.count() do |cell| cell == :o end
  x_count == o_count ? :x : :o
end

def best_move(text: String) -> Int
  board = board_parse(text)
  if board_winner(board) != nil
    warn("that game is already over")
    return 65
  end
  if board_full?(board)
    warn("that board is already full")
    return 65
  end
  player = whose_turn(board)
  result = minimax(board, player, player)
  outcome = if result.score() == 1 then "#{player} wins with best play"
             elsif result.score() == 0 then "draw with best play"
             else "#{player} loses with best play" end
  puts("#{player} plays #{result.move()} -> #{outcome}")
  0
end

def main(argv) -> Int
  return usage() if argv.empty?()
  case argv[0]
  when "--self" then play_self()
  when "--best"
    return usage() unless argv.length() == 2
    begin
      best_move(argv[1])
    rescue error: BoardError
      warn(error.message())
      65
    end
  else
    usage()
  end
end

exit(main(ARGV))
