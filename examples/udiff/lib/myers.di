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
  limit = [old_keys.length(), new_keys.length()].min()
  head = 0
  while head < limit && old_keys[head] == new_keys[head]
    head += 1
  end
  tail = 0
  while tail < limit - head &&
        old_keys[old_keys.length() - 1 - tail] == new_keys[new_keys.length() - 1 - tail]
    tail += 1
  end
  rows = old_keys.length() - head - tail
  cols = new_keys.length() - head - tail

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
  reach = rows + cols
  offset = reach + 1
  v = [0] * (2 * reach + 3)
  trace = []
  cells = 0
  final_d = -1
  d = 0
  while final_d < 0
    # What each diagonal reached in round d - 1, for the walk back.
    trace.push(v.slice(offset - d - 1, 2 * d + 3))
    cells += 2 * d + 3
    if cells > Limits::MAX_TRACE_CELLS
      raise DiffError.new("files are too different to compare")
    end
    k = -d
    while k <= d
      x = if k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])
        v[offset + k + 1]
      else
        v[offset + k - 1] + 1
      end
      y = x - k
      while x < rows && y < cols && old_keys[head + x] == new_keys[head + y]
        x += 1
        y += 1
      end
      v[offset + k] = x
      if x >= rows && y >= cols
        final_d = d
        break
      end
      k += 2
    end
    d += 1
  end

  # Walk from the end back to (0, 0), collecting edits last-first.
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
    while x > landed_x
      x -= 1
      backwards.push(Keep.new(old_lines[head + x]))
    end
    if came_from == k + 1
      backwards.push(Insert.new(new_lines[head + prev_y]))
    else
      backwards.push(Delete.new(old_lines[head + prev_x]))
    end
    x = prev_x
    y = prev_y
  end
  while x > 0
    x -= 1
    backwards.push(Keep.new(old_lines[head + x]))
  end
  backwards.reverse()
end
