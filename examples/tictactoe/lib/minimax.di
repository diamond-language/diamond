# Perfect play by exhaustive search: every position is small enough
# (at most 9 levels deep, at most 9! leaves) that there's no need for
# alpha-beta pruning or a transposition table -- the point here is the
# recursion and the in-place board mutation, not the search optimization.
require "./board"

# What a search returns for a position: how good it is, and which square
# to play to get that outcome (nil at a finished game, where nobody moves).
struct MoveResult(score: Int, move: Int | Nil)
end

# `score` is always from `maximizer`'s own point of view: 1 if `maximizer`
# forces a win with both sides playing optimally from here, -1 if
# `maximizer` loses, 0 for a draw -- regardless of whether `player` (whose
# actual turn it is at this node) equals `maximizer`. board is mutated in
# place and restored before returning, rather than copied at each level.
def minimax(board: Array, player: Symbol, maximizer: Symbol) -> MoveResult
  # Base cases: the game is over. A winner here can only be the player who
  # just moved, so it is a win for `maximizer` iff that player is
  # `maximizer`. A full board with no winner is a draw.
  winner = board_winner(board)
  return MoveResult.new(winner == maximizer ? 1 : -1, nil) if winner != nil
  return MoveResult.new(0, nil) if board_full?(board)

  best_score = nil
  best_move = nil
  position = 0

  # Try every empty square for the player to move.
  while position < 9
    if board[position] == nil
      # Play the move, evaluate the resulting position from the opponent's
      # side, then undo it so the board is exactly as we found it for the
      # next candidate square.
      board[position] = player
      result = minimax(board, other_player(player), maximizer)
      board[position] = nil

      # Keep the best result for whoever is moving at this node: maximizer
      # wants the highest score, the opponent wants the lowest. The first
      # candidate always wins (best_score is still nil); on a tie the
      # earlier square is kept.
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
