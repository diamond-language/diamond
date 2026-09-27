require "./edit"
require "./hunks"
require "./lines"

class PatchError < StandardError
end

# Parses the hunks out of unified-diff text. The "---" / "+++" file header
# lines and anything before the first "@@" are ignored, so the output of
# `udiff`, `diff -u`, and `git diff` all read the same way.
def parse_patch(text: String) -> Array[Hunk]
  header = Regexp.new("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@")
  hunks = []
  current = nil
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
  result = []
  position = 0
  hunks.each_with_index() do |hunk, index|
    start = if hunk.old_count() == 0 then hunk.old_start() else hunk.old_start() - 1 end
    if start < position || start > lines.length()
      raise PatchError.new("hunk #{index + 1} (#{hunk_header(hunk)}) is out of order or past the end of the file")
    end
    lines.slice(position, start - position).each() do |line| result.push(line) end
    position = start
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
  lines.slice(position, lines.length() - position).each() do |line| result.push(line) end
  result
end
