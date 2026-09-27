# udiff: compare two text files as a unified diff, and apply one.
#
#   diamond udiff.di [-U N] [-i] [-w] [-q] [--stat] OLD NEW
#   diamond udiff.di --apply [-R] [-o OUT] PATCH FILE
#
# The first form prints a unified diff of OLD -> NEW. -U N sets the lines of
# context (default 3), -i ignores case, -w ignores all whitespace, -q only
# says whether the files differ, and --stat prints a change count instead of
# the diff. The second form applies a unified diff (from udiff, diff -u, or
# git diff) to FILE, printing the result or writing it to OUT; -R applies
# the patch backwards.
#
# Exit status: 0 when the files are the same (or the patch applied), 1 when
# they differ, 2 for a usage error, an unreadable file, or a patch that
# doesn't fit.
require "./lib/lines"
require "./lib/myers"
require "./lib/hunks"
require "./lib/patch"

def usage() = "usage: udiff [-U N] [-i] [-w] [-q] [--stat] OLD NEW\n       udiff --apply [-R] [-o OUT] PATCH FILE"

def read_lines(path: String) -> Array[String] = split_lines(File.read(path))

def normalize(lines: Array[String], ignore_case: Bool, ignore_space: Bool) -> Array[String]
  return lines unless ignore_case || ignore_space
  lines.map() do |line|
    text = line
    text = text.downcase() if ignore_case
    text = text.gsub(Regexp.new("\\s+"), "") if ignore_space
    text
  end
end

def write_lines(lines: Array[String], path: String | Nil)
  text = if lines.empty?() then "" else lines.join("\n") + "\n" end
  if path == nil
    print(text)
  else
    File.write(path, text)
  end
end

def run_diff(old_path: String, new_path: String, context: Int,
             ignore_case: Bool, ignore_space: Bool, brief: Bool, stat: Bool) -> Int
  old_lines = read_lines(old_path)
  new_lines = read_lines(new_path)
  edits = diff_lines(normalize(old_lines, ignore_case, ignore_space),
                     normalize(new_lines, ignore_case, ignore_space),
                     old_lines, new_lines)
  hunks = build_hunks(edits, context)
  return 0 if hunks.empty?()
  if brief
    puts("Files #{old_path} and #{new_path} differ")
  elsif stat
    added = edits.count() do |edit| edit.is_a?(Insert) end
    removed = edits.count() do |edit| edit.is_a?(Delete) end
    puts("#{hunks.length()} hunk#{if hunks.length() == 1 then "" else "s" end}, #{added} insertion#{if added == 1 then "" else "s" end}(+), #{removed} deletion#{if removed == 1 then "" else "s" end}(-)")
  else
    puts("--- #{old_path}")
    puts("+++ #{new_path}")
    hunks.each() do |hunk| puts(render_hunk(hunk)) end
  end
  1
end

def run_apply(patch_path: String, file_path: String, reverse: Bool, out_path: String | Nil) -> Int
  hunks = parse_patch(File.read(patch_path))
  if hunks.empty?()
    warn("udiff: #{patch_path}: no hunks found")
    return 2
  end
  if reverse
    hunks = hunks.map() do |hunk|
      Hunk.new(hunk.new_start(), hunk.new_count(), hunk.old_start(), hunk.old_count(),
               hunk.edits().map() do |edit| reverse_edit(edit) end)
    end
  end
  write_lines(apply_hunks(read_lines(file_path), hunks), out_path)
  0
end

def main(args: Array[String]) -> Int
  context = 3
  ignore_case = false
  ignore_space = false
  brief = false
  stat = false
  apply = false
  reverse = false
  out_path = nil
  files = []
  index = 0
  while index < args.length()
    arg = args[index]
    case arg
    when "-U"
      index += 1
      if index >= args.length() || !Regexp.new("^\\d+$").match?(args[index])
        warn("udiff: -U needs a number of lines\n#{usage()}")
        return 2
      end
      context = args[index].to_i()
    when "-o"
      index += 1
      if index >= args.length()
        warn("udiff: -o needs a file\n#{usage()}")
        return 2
      end
      out_path = args[index]
    when "-i" then ignore_case = true
    when "-w" then ignore_space = true
    when "-q" then brief = true
    when "--stat" then stat = true
    when "--apply" then apply = true
    when "-R" then reverse = true
    else
      if arg.start_with?("-") && arg != "-"
        warn("udiff: unknown option #{arg}\n#{usage()}")
        return 2
      end
      files.push(arg)
    end
    index += 1
  end
  if files.length() != 2
    warn(usage())
    return 2
  end
  begin
    if apply
      run_apply(files[0], files[1], reverse, out_path)
    else
      run_diff(files[0], files[1], context, ignore_case, ignore_space, brief, stat)
    end
  rescue error: IOError
    warn("udiff: #{error.message()}")
    2
  rescue error: DiffError
    warn("udiff: #{error.message()}")
    2
  rescue error: PatchError
    warn("udiff: #{error.message()}")
    2
  end
end

exit(main(ARGV))
