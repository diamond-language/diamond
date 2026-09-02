# Recursively collects every *file* path under `folder` into `results`,
# in directory-entry order at each level (Dir.entries has no ordering
# guarantee of its own -- sorted here, a plain byte-lexicographic
# String sort_by, same as the caller's own final sort in Corpus.load
# below). Built directly on Dir.entries + File.directory? -- Diamond
# had neither at all before this session, forcing every previous
# version of this function to shell out to `find` via Process.run
# instead.
def collect_files_recursive(folder, results)
  entries = Dir.entries(folder).sort_by() do |name| name end
  i = 0
  while i < entries.length()
    full_path = File.join(folder, entries[i])
    if File.directory?(full_path)
      collect_files_recursive(full_path, results)
    else
      results.push(full_path)
    end
    i += 1
  end
end

# Assembles training text from a folder. Two source shapes:
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
class Corpus
  def self.load(folder, extensions, max_files, max_stories)
    all_paths = []
    collect_files_recursive(folder, all_paths)
    # Byte-lexicographic order -- for TinyStories' own zero-padded
    # filenames (data00.json..data50.json) this already matches
    # numeric order (confirmed directly: same-width zero-padded
    # numbers sort identically both ways); it wouldn't for
    # non-zero-padded numbering ("data2.json" would sort after
    # "data10.json"), a real limitation worth knowing if this ever
    # points at a differently-named dataset.
    paths = all_paths.sort_by() do |p| p end

    text = ""
    file_count = 0
    story_count = 0
    i = 0
    while i < paths.length()
      path = paths[i]
      if extensions.include?(File.extname(path))
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
