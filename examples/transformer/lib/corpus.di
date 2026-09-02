# Assembles training text from a folder. Shells out to `find`
# (Process.run) rather than needing a Dir/glob facility -- Diamond has
# no directory-listing builtin at all (confirmed: no Dir class,
# nothing in DiamondOpCode). Two source shapes:
#
# - .txt/.md: the whole file's raw bytes, used directly.
# - .json: parsed (Diamond's own JSON.parse -- pure Diamond, recursive
#   descent, genuinely slow at real dataset scale: ~330ms/MB measured
#   directly against TinyStories_all_data, so a single ~140MB shard
#   takes on the order of 45s just to parse) and walked expecting a
#   top-level Array; each element contributes its "story" field if
#   it's a Hash that has one (the TinyStories format: `[{"story": "...",
#   "instruction": {...}, "summary": "...", "source": "..."}, ...]`),
#   or contributes itself directly if it's a plain String (a simpler
#   "array of strings" JSON corpus). Anything else is skipped.
#
# `max_files`: stop after this many *matching* files have been read
# (nil = no limit) -- the real cost driver at TinyStories' scale, since
# each file must be fully parsed before any of it can be discarded.
# `max_stories`: stop adding to the corpus once this many .json array
# elements have contributed text (nil = no limit; irrelevant to .txt/
# .md files, which always contribute their whole content) -- lets one
# already-parsed file (100k stories in TinyStories' own shards) supply
# a small, fast-to-iterate-on corpus without needing a second file.
# Both are meant for "start small while getting the model/hyper-
# parameters right," not a permanent ceiling -- raise or drop them
# once you actually want the full corpus.
class PathSort
  # The numeric substring in a path's basename (e.g. "data03.json" ->
  # 3), or 0 if it has none -- String has no comparison operator at
  # all in Diamond (confirmed directly), so sort_by needs an Int key
  # rather than sorting path strings themselves.
  def self.numeric_key(path)
    base = File.basename(path)
    digits = ""
    i = 0
    while i < base.length()
      ch = base[i]
      if ch.ord() >= 48 && ch.ord() <= 57
        digits = digits + ch
      end
      i += 1
    end
    if digits == "" then 0 else digits.to_i() end
  end
end

class Corpus
  def self.load(folder, extensions, max_files, max_stories)
    result = Process.run(["find", folder, "-type", "f"])
    if !result.success?()
      raise IOError.new("find failed for #{folder}: #{result.stderr()}")
    end
    # `find`'s own order is filesystem-dependent, not sorted (confirmed
    # directly: on this machine it returned data22.json before
    # data00.json) -- sorted here so max_files means "the first N
    # files in filename order" (data00, data01, ...), matching what
    # anyone asking for "just the first N shards" of a numbered dataset
    # actually means.
    paths = result.stdout().split("\n").sort_by() do |p| PathSort.numeric_key(p) end

    text = ""
    file_count = 0
    story_count = 0
    i = 0
    while i < paths.length()
      path = paths[i]
      if path != "" && extensions.include?(File.extname(path))
        if max_files != nil && file_count >= max_files
          i += 1
          next
        end
        ext = File.extname(path)
        if ext == ".json"
          puts("Corpus.load: parsing #{path} (this can take a while at real dataset scale)")
          file = File.open(path, "r")
          raw = file.read()
          file.close()
          parsed = JSON.parse(raw)
          j = 0
          while j < parsed.length() && (max_stories == nil || story_count < max_stories)
            entry = parsed[j]
            story_text = nil
            if entry is String
              story_text = entry
            elsif entry is Hash && entry["story"] != nil
              story_text = entry["story"]
            end
            if story_text != nil
              text = text + story_text + "\n\n"
              story_count += 1
            end
            j += 1
          end
        else
          file = File.open(path, "r")
          text = text + file.read() + "\n\n"
          file.close()
        end
        file_count += 1
      end
      i += 1
    end
    puts("Corpus.load: assembled #{file_count} file(s), #{story_count} JSON stor(y/ies), #{text.length()} bytes total from #{folder}")
    text
  end
end
