# A bookmark collection: stores URLs in SQLite through mjolnir, refuses addresses the server must not
# fetch (network_safety), takes a bulk import as a multipart upload (multipart), and logs one JSON
# line per event (logger) that log_viewer can read back.
require_cut "database_config"
require_cut "mjolnir"
require_cut "multipart"
require_cut "network_safety"
require_cut "logger"
require_cut "log_viewer"

struct Bookmark(id: Int, url: String, title: String)
end

def build_bookmark(row) = Bookmark.new(row["id"], row["url"], row["title"])

class Bookmarks
  def initialize(repo, log)
    @repo = repo
    @log = log
    @schema = Mjolnir::Schema.new("bookmarks", {"id": :int, "url": :string, "title": :string}, build_bookmark)
  end

  # The connection comes from config/database.json, the entry named by `environment`.
  def self.open(environment: String, log)
    config = DatabaseConfig.load("config/database.json", environment)
    db = DatabaseConfig.open(config)
    db.execute("CREATE TABLE IF NOT EXISTS bookmarks (id INTEGER PRIMARY KEY, url TEXT NOT NULL UNIQUE, title TEXT NOT NULL)")
    Bookmarks.new(Mjolnir::Repo.new(db), log)
  end

  def changeset(attrs: Hash)
    set = Mjolnir::Changeset.cast(@schema, nil, attrs, ["url", "title"])
      .validate_required(["url", "title"])
      .unique_constraint("url")
    return set unless set.valid?()
    begin
      resolve_public_hostname(set.get_field("url"))
      set
    rescue error: ArgumentError
      set.add_error("url", error.message())
    end
  end

  # Ok(bookmark) or Err(changeset); either way one log line.
  def add(url, title)
    result = @repo.insert(self.changeset({"url": url, "title": title}))
    case result
    when Mjolnir::Ok
      @log.info("bookmark.added", {"id": result.value().id(), "url": url})
    when Mjolnir::Err
      @log.warn("bookmark.rejected", {"url": url, "errors": result.error().errors()})
    end
    result
  end

  # A multipart upload with a file field `list` holding "url title" lines. Returns how many were added
  # and how many were refused, or nil when the request is not multipart/form-data.
  def import(request: Hash)
    upload = multipart_parse(request)
    return nil if upload == nil
    file = upload["files"]["list"]
    return nil if file == nil
    added = 0
    refused = 0
    file["data"].split("\n").each() do |line|
      next if line.strip() == ""
      space = line.index_of(" ")
      url = if space == nil then line.strip() else line.slice(0, space) end
      title = if space == nil then url else line.slice(space + 1, line.length() - space - 1).strip() end
      case self.add(url, title)
      when Mjolnir::Ok then added += 1
      when Mjolnir::Err then refused += 1
      end
    end
    {"added": added, "refused": refused}
  end

  def all() = @repo.all(@schema.query().order_by("id"))
  def count() = @repo.count(@schema.query())
end

# The log file's lines, formatted the way `diamond-log` prints them.
def read_log(path: String)
  File.read(path).split("\n").select() do |line| line != "" end.map() do |line|
    LogViewer.format_line(line)[0]
  end
end
