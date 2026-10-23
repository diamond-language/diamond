require "./edit"
require "./hunks"
require "./lines"

# The patch is malformed, or does not fit the file it is applied to.
class PatchError < StandardError
end

# Parses the hunks out of unified-diff text. The "---" / "+++" file header
# lines and anything before the first "@@" are ignored, so the output of
# `udiff`, `diff -u`, and `git diff` all read the same way.
def parse_patch(text: String) -> Array[Hunk]
  # Matches "@@ -a,b +c,d @@" (the ",b" and ",d" are optional and mean 1
  # when absent). Captures: 1=a, 2=b, 3=c, 4=d.
  header = Regexp.new("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@")
  hunks = []
  current = nil

  # Pass 1: a header line starts a new hunk; every other line after the first
  # header belongs to the current hunk. Lines before it (file headers, git's
  # "diff --git ...") fall through and are ignored.
  split_lines(text).each_with_index() do |line, index|
    found = header.match(line)

    if found != nil
      old_count = if found[2] == nil then 1 else found[2].to_i() end
      new_count = if found[4] == nil then 1 else found[4].to_i() end
      current = Hunk.new(found[1].to_i(), old_count, found[3].to_i(), new_count, [])
      hunks.push(current)
    elsif current != nil
      if line == ""
        # A blank context line that lost its leading space.
        current.edits().push(Keep.new(""))
      else
        # The first character is the marker; the rest is the line's text.
        body = line.slice(1, line.length() - 1)

        case line[0]
        when " " then current.edits().push(Keep.new(body))
        when "-" then current.edits().push(Delete.new(body))
        when "+" then current.edits().push(Insert.new(body))
        when "\\" then nil # "\ No newline at end of file"
        else
          raise PatchError.new("line #{index + 1}: unexpected '#{line}' in a hunk")
        end
      end
    end
  end

  # Pass 2: check that each hunk's header counts agree with the lines it
  # actually has. A mismatch means the patch was truncated or hand-edited
  # wrongly, and is much better caught here than as a confusing failure
  # while applying.
  hunks.each_with_index() do |hunk, index|
    old_lines = hunk.edits().count() do |edit| !edit.is_a?(Insert) end
    new_lines = hunk.edits().count() do |edit| !edit.is_a?(Delete) end
    if old_lines != hunk.old_count() || new_lines != hunk.new_count()
      raise PatchError.new("hunk #{index + 1} says #{hunk_header(hunk)} but has #{old_lines} old and #{new_lines} new lines")
    end
  end
  hunks
end

# Applies hunks to a file's lines and returns the new lines. Every Keep and
# Delete must match the file exactly where the hunk says it is; a hunk that
# doesn't fit fails the whole patch, and nothing is half-applied.
def apply_hunks(lines: Array[String], hunks: Array[Hunk]) -> Array[String]
  # `position` is how far into the old file we have read. `result` is built
  # fresh and only returned at the end, which is why a failure part-way
  # leaves nothing half-applied.
  result = []
  position = 0

  hunks.each_with_index() do |hunk, index|
    # Convert the hunk's 1-based start to a 0-based index (a zero-line side
    # already names the line before, so it needs no -1). Hunks must be in
    # order and inside the file.
    start = if hunk.old_count() == 0 then hunk.old_start() else hunk.old_start() - 1 end
    if start < position || start > lines.length()
      raise PatchError.new("hunk #{index + 1} (#{hunk_header(hunk)}) is out of order or past the end of the file")
    end

    # Copy the untouched lines between the previous hunk and this one.
    lines.slice(position, start - position).each() do |line| result.push(line) end
    position = start

    # Replay the hunk. Keep and Delete lines must match the file exactly at
    # `position` (the check that catches a patch applied to the wrong file or
    # version); a Keep is copied through, a Delete just skips the line, and an
    # Insert adds text without consuming any.
    hunk.edits().each() do |edit|
      case edit
      when Keep, Delete
        if position >= lines.length() || lines[position] != edit.text()
          found = if position >= lines.length() then "end of file" else "'#{lines[position]}'" end
          raise PatchError.new("hunk #{index + 1} does not match at line #{position + 1}: expected '#{edit.text()}', found #{found}")
        end
        result.push(edit.text()) if edit.is_a?(Keep)
        position += 1
      when Insert
        result.push(edit.text())
      end
    end
  end

  # After the last hunk, copy the rest of the file.
  lines.slice(position, lines.length() - position).each() do |line| result.push(line) end
  result
end
