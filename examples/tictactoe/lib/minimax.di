# Perfect play by exhaustive search: every position is small enough
# (at most 9 levels deep, at most 9! leaves) that there's no need for
# alpha-beta pruning or a transposition table -- the point here is the
# recursion and the in-place board mutation, not the search optimization.
require "./board"

struct MoveResult(score: Int, move: Int | Nil)
end

# `score` is always from `maximizer`'s own point of view: 1 if `maximizer`
# forces a win with both sides playing optimally from here, -1 if
# `maximizer` loses, 0 for a draw -- regardless of whether `player` (whose
# actual turn it is at this node) equals `maximizer`. board is mutated in
# place and restored before returning, rather than copied at each level.
def minimax(board: Array, player: Symbol, maximizer: Symbol) -> MoveResult
  winner = board_winner(board)
  return MoveResult.new(winner == maximizer ? 1 : -1, nil) if winner != nil
  return MoveResult.new(0, nil) if board_full?(board)
  best_score = nil
  best_move = nil
  position = 0
  while position < 9
    if board[position] == nil
      board[position] = player
      result = minimax(board, other_player(player), maximizer)
      board[position] = nil
      maximizing_here = player == maximizer
      better = best_score == nil ||
        (maximizing_here ? result.score() > best_score : result.score() < best_score)
      if better
        best_score = result.score()
        best_move = position
      end
    end
    position += 1
  end
  MoveResult.new(best_score, best_move)
end
