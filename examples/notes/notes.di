# notes: a command-line notebook kept in one SQLite file.
#
#   diamond notes.di [--db FILE] COMMAND [ARGS...]
#
# Commands:
#   add TITLE [TAG...]   new note; the body is read from stdin
#   list [TAG]           every note, or those with TAG
#   show ID              one note in full
#   search QUERY         full-text search, best matches first
#   tag ID TAG...        add tags          untag ID TAG   remove one
#   rm ID                delete a note (and its tags)
#   import FILE          add notes from a JSON array, all or nothing
#   export               every note as JSON
#   stats                counts by tag and by month
#
# The database is --db FILE, else $NOTES_DB, else notes.db; it's created
# and migrated on first use. $NOTES_NOW overrides today's date (for tests).
# Exit status: 0 ok, 1 for an unknown note or rejected input, 64 for a
# usage error, 66 when a file can't be read.
require "./lib/store"

def usage() = "usage: notes [--db FILE] add|list|show|search|tag|untag|rm|import|export|stats ..."

def note_id(text: String) -> Int
  raise NotesError.new("not a note id: #{text}") unless Regexp.new("^[0-9]+$").match?(text)
  text.to_i()
end

def found(store: NoteStore, text: String) -> Note
  note = store.find(note_id(text))
  raise NotesError.new("no note #{text}") if note == nil
  note
end

def tag_text(tags: Array[String]) -> String
  if tags.empty?() then "" else "  [#{tags.join(", ")}]" end
end

def print_list(notes: Array[Note])
  puts("no notes") if notes.empty?()
  notes.each() do |note|
    puts("#{note.id().to_s().rjust(4, " ")}  #{note.created()}  #{note.title()}#{tag_text(note.tags())}")
  end
end

def read_body() -> String
  lines = []
  loop do
    line = gets()
    break if line == nil
    lines.push(line)
  end
  lines.join("\n")
end

def run(store: NoteStore, command: Array[String]) -> Int
  case command
  when ["add", title, *tags]
    id = store.add(title, read_body(), tags)
    puts("added #{id}")
  when ["list"] then print_list(store.list(nil))
  when ["list", tag] then print_list(store.list(tag))
  when ["show", id]
    note = found(store, id)
    puts("##{note.id()} #{note.title()}")
    puts("#{note.created()}#{tag_text(note.tags())}")
    puts("")
    puts(note.body())
  when ["search", *words]
    raise NotesError.new("search needs a query") if words.empty?()
    hits = store.search(words.join(" "))
    puts("no matches") if hits.empty?()
    hits.each() do |hit|
      puts("#{hit["id"].to_s().rjust(4, " ")}  #{hit["title"]}")
      puts("      #{hit["snippet"].gsub(Regexp.new("\\s+"), " ")}")
    end
  when ["tag", id, *tags]
    raise NotesError.new("tag needs at least one tag") if tags.empty?()
    note = found(store, id)
    store.tag_all(note.id(), tags)
    puts("tagged #{note.id()}")
  when ["untag", id, tag]
    note = found(store, id)
    raise NotesError.new("note #{id} has no tag #{tag}") unless store.untag(note.id(), tag)
    puts("untagged #{note.id()}")
  when ["rm", id]
    raise NotesError.new("no note #{id}") unless store.remove(note_id(id))
    puts("removed #{id}")
  when ["import", path]
    entries = JSON.parse(File.open(path, "r").read())
    raise NotesError.new("#{path}: expected a JSON array") unless entries is Array
    puts("imported #{store.import(entries)}")
  when ["export"]
    puts(JSON.stringify(store.list(nil).map() do |note|
      {"id": note.id(), "title": note.title(), "body": note.body(), "created": note.created(), "tags": note.tags()}
    end))
  when ["stats"]
    puts("#{store.count()} notes")
    puts("")
    puts("Tags")
    store.tag_counts().each() do |row| puts("  #{row["name"].ljust(12, " ")}#{row["count"]}") end
    puts("")
    puts("By month")
    store.monthly_counts().each() do |row| puts("  #{row["month"]}  #{row["count"]}") end
  else
    warn(usage())
    return 64
  end
  0
end

def main(args: Array[String]) -> Int
  path = ENV.fetch("NOTES_DB", "notes.db")
  if args.length() >= 2 && args[0] == "--db"
    path = args[1]
    args = args.slice(2, args.length() - 2)
  end
  if args.empty?()
    warn(usage())
    return 64
  end
  if args[0] == "--help"
    puts(usage())
    return 0
  end
  now = ENV.fetch("NOTES_NOW", Time.now().strftime("%Y-%m-%d"))
  store = nil
  begin
    store = NoteStore.new(path, now)
    run(store, args)
  rescue error: NotesError
    warn("notes: #{error.message()}")
    1
  rescue error: JSONError
    warn("notes: #{error.message()}")
    1
  rescue error: IOError
    warn("notes: #{error.message()}")
    66
  rescue error: SQLite3Error
    warn("notes: #{error.message()}")
    1
  ensure
    store.close() unless store == nil
  end
end

exit(main(ARGV))
