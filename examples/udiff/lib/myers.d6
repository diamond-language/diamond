require "./edit"

module Limits
  # The most cells the edit-distance trace may hold before the diff gives
  # up. The trace grows with the square of the number of changed lines, so
  # this only trips for two files with almost nothing in common.
  MAX_TRACE_CELLS = 40_000_000
end

# Diffs two files' lines with Myers' O(ND) algorithm, the one diff(1) and
# git use: cost grows with the number of changed lines D, not with the
# product of the file sizes, so a few edits in a big file are cheap.
#
# `old_keys` and `new_keys` are what lines are compared by (the lines
# themselves, or a normalized form of them for -i / -w); `old_lines` is
# what a Keep or Delete reports, `new_lines` what an Insert reports. The
# common head and tail are peeled off first.
def diff_lines(old_keys: Array[String], new_keys: Array[String],
               old_lines: Array[String], new_lines: Array[String]) -> Array[Edit]
  # Peel off the longest common prefix ("head")...
  limit = [old_keys.length(), new_keys.length()].min()
  head = 0
  while head < limit && old_keys[head] == new_keys[head]
    head += 1
  end

  # ...and common suffix ("tail"), bounded by `limit - head` so the two can
  # never overlap. Only what is left in between needs the real algorithm.
  tail = 0
  while tail < limit - head &&
        old_keys[old_keys.length() - 1 - tail] == new_keys[new_keys.length() - 1 - tail]
    tail += 1
  end
  rows = old_keys.length() - head - tail
  cols = new_keys.length() - head - tail

  # Stitch the answer together: head as Keeps, the computed middle, tail as
  # Keeps. Keeps report the OLD file's text (it equals the new text, or at
  # least matches it under the -i/-w comparison).
  edits = []
  head.times() do |k| edits.push(Keep.new(old_lines[k])) end
  middle_edits(old_keys, new_keys, old_lines, new_lines, head, rows, cols).each() do |edit|
    edits.push(edit)
  end
  tail.times() do |k| edits.push(Keep.new(old_lines[old_lines.length() - tail + k])) end
  edits
end

# Myers' shortest edit script for the `rows` old and `cols` new lines that
# start at `head`. v[offset + k] is the furthest x reached on diagonal
# k = x - y; each round d extends every diagonal -d, -d+2, ..., d by one
# edit and then slides down its "snake" of matching lines. The rounds'
# v arrays are kept so the path can be walked backwards afterwards.
def middle_edits(old_keys: Array[String], new_keys: Array[String],
                 old_lines: Array[String], new_lines: Array[String],
                 head: Int, rows: Int, cols: Int) -> Array[Edit]
  return [] if rows == 0 && cols == 0

  # The edit graph: x = lines consumed from the old file, y = from the new.
  # Moving right is a Delete, down is an Insert, and a diagonal step is free
  # (a matching line). Diagonal k is x - y. At most rows + cols edits are
  # ever needed (`reach`); `offset` shifts k so negative diagonals index
  # into the array, and `v` has room for k from -(reach+1) to reach+1.
  reach = rows + cols
  offset = reach + 1
  v = [0] * (2 * reach + 3)
  trace = []
  cells = 0
  final_d = -1
  d = 0

  # Forward pass: round d finds the furthest point reachable with exactly d
  # edits, on each diagonal, and stops at the first round that reaches the far
  # corner. That d is the minimum number of edits.
  while final_d < 0
    # Save a copy of v as it stood after round d - 1 (only the diagonals this
    # round can read), so the path can be walked back later. This is the
    # memory-hungry part, hence the size cap below.
    trace.push(v.slice(offset - d - 1, 2 * d + 3))
    cells += 2 * d + 3
    if cells > Limits::MAX_TRACE_CELLS
      raise DiffError.new("files are too different to compare")
    end

    k = -d
    while k <= d
      # Reach diagonal k from a neighbor: from k+1 by moving DOWN (an
      # insert, x unchanged), or from k-1 by moving RIGHT (a delete, x + 1).
      # Take whichever neighbor got further. The edge diagonals (-d and d)
      # have only one neighbor, and on a tie the code moves right.
      x = if k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])
        v[offset + k + 1]
      else
        v[offset + k - 1] + 1
      end
      y = x - k

      # Slide down the "snake": follow diagonal steps for as long as the
      # lines match (these cost no edits).
      while x < rows && y < cols && old_keys[head + x] == new_keys[head + y]
        x += 1
        y += 1
      end
      v[offset + k] = x

      # Reached the bottom-right corner: the d just completed is minimal.
      if x >= rows && y >= cols
        final_d = d
        break
      end

      # Only every other diagonal is reachable in a given round (parity), so
      # step by 2.
      k += 2
    end
    d += 1
  end

  # Backward pass: from the far corner, retrace the path round by round
  # using the saved snapshots, working out at each round which neighbor the
  # path came from (the same rule as the forward pass, applied to that
  # round's snapshot). Edits are collected last-first and reversed at the
  # end.
  backwards = []
  x = rows
  y = cols

  final_d.downto(1) do |round|
    before = trace[round]
    at = round + 1 # index of diagonal 0 in this round's snapshot
    k = x - y
    came_from = if k == -round || (k != round && before[at + k - 1] < before[at + k + 1])
      k + 1
    else
      k - 1
    end
    prev_x = before[at + came_from]
    prev_y = prev_x - came_from

    # Snake back to where the edit landed: (prev_x, prev_y + 1) for an
    # insertion, (prev_x + 1, prev_y) for a deletion.
    landed_x = if came_from == k + 1 then prev_x else prev_x + 1 end

    # The snake's matching lines are Keeps...
    while x > landed_x
      x -= 1
      backwards.push(Keep.new(old_lines[head + x]))
    end

    # ...then the single edit that started the round: coming from k+1 means
    # a downward move (an Insert of the new line at prev_y), from k-1 a
    # rightward move (a Delete of the old line at prev_x).
    if came_from == k + 1
      backwards.push(Insert.new(new_lines[head + prev_y]))
    else
      backwards.push(Delete.new(old_lines[head + prev_x]))
    end

    x = prev_x
    y = prev_y
  end

  # What remains is round 0's snake from the origin: the leading matches the
  # edits never touched.
  while x > 0
    x -= 1
    backwards.push(Keep.new(old_lines[head + x]))
  end
  backwards.reverse()
end
