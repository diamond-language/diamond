require "./edit"

# A run of edits with its position in both files. Line numbers are 1-based;
# a count of 0 means the hunk touches no line of that file, and the start
# is then the line *before* the hunk, as in GNU diff.
struct Hunk(old_start: Int, old_count: Int, new_start: Int, new_count: Int, edits: Array[Edit])
end

# The "-3,4" half of a hunk header. A one-line range is written as just the
# start ("-3"), as in GNU diff.
def range_text(start: Int, count: Int) -> String
  if count == 1 then "#{start}" else "#{start},#{count}" end
end

# "@@ -old_start,old_count +new_start,new_count @@".
def hunk_header(hunk: Hunk) -> String
  "@@ -#{range_text(hunk.old_start(), hunk.old_count())} +#{range_text(hunk.new_start(), hunk.new_count())} @@"
end

# Groups an edit script into hunks with `context` unchanged lines around
# every change. Two changes share a hunk when the unchanged lines between
# them fit inside both their contexts.
def build_hunks(edits: Array[Edit], context: Int) -> Array[Hunk]
  # Pass 1: walk the whole script once, recording for every edit how many
  # lines of the old and new file come before it (needed for the hunk's
  # starting line numbers), and which edits are real changes.
  # Lines of each file consumed before edit i.
  old_before = []
  new_before = []
  old_seen = 0
  new_seen = 0
  changed = []

  edits.each_with_index() do |edit, index|
    old_before.push(old_seen)
    new_before.push(new_seen)

    # A Keep advances both files, a Delete only the old, an Insert only the
    # new; the last two are the "changes".
    case edit
    when Keep
      old_seen += 1
      new_seen += 1
    when Delete
      old_seen += 1
      changed.push(index)
    when Insert
      new_seen += 1
      changed.push(index)
    end
  end

  # Pass 2: group changes into [first, last] index ranges. A change joins the
  # previous group when at most 2*context unchanged lines separate them
  # (their context windows would touch or overlap); otherwise it starts a
  # new group.
  groups = []
  changed.each() do |index|
    if groups.empty?() || index - groups.last()[1] - 1 > 2 * context
      groups.push([index, index])
    else
      groups.last()[1] = index
    end
  end

  # Pass 3: turn each group into a Hunk: widen it by `context` Keep lines each
  # way (without running off either end of the script), count how many old
  # and new lines it covers, and compute where it starts. A side with zero
  # lines starts at the line BEFORE the hunk (GNU convention, see Hunk's
  # comment); otherwise the line after the lines already consumed.
  groups.map() do |group|
    first = [group[0] - context, 0].max()
    last = [group[1] + context, edits.length() - 1].min()
    slice = edits.slice(first, last - first + 1)
    old_count = slice.count() do |edit| edit.is_a?(Keep) || edit.is_a?(Delete) end
    new_count = slice.count() do |edit| edit.is_a?(Keep) || edit.is_a?(Insert) end
    old_start = if old_count == 0 then old_before[first] else old_before[first] + 1 end
    new_start = if new_count == 0 then new_before[first] else new_before[first] + 1 end
    Hunk.new(old_start, old_count, new_start, new_count, slice)
  end
end

# The hunk as text: its header, then each edit with its +/-/space prefix.
def render_hunk(hunk: Hunk) -> String
  lines = [hunk_header(hunk)]
  hunk.edits().each() do |edit|
    lines.push(edit_prefix(edit) + edit.text())
  end
  lines.join("\n")
end
