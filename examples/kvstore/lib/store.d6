# The data: a Hash of key -> value, a Hash of key -> expiry (Unix seconds),
# and an append-only log that makes it durable. Each log line is a JSON
# array, so keys and values may hold any bytes JSON can carry.
#
#   ["set", "user:1", "ada", null]      value, no expiry
#   ["set", "token", "abc", 1790000000]  value expiring at that epoch
#   ["del", "user:1"]

# The in-memory tables are the source of truth for reads; the log exists
# only to rebuild them after a restart. Every change is appended to the log
# BEFORE a reply is sent, so an acknowledged write survives a crash.
class Store
  attr_reader log_path: String

  def initialize(log_path: String)
    @log_path = log_path
    @values = {}
    @expires = {}

    # How many lines the log currently holds (versus how many keys are live);
    # compaction triggers when the log has grown far past the live data.
    @log_lines = 0
    @log = nil
  end

  # Replays the log (if any), then opens it for appending.
  def load() -> Int
    # Replay each logged operation in order, to rebuild the state as it was.
    if File.exist?(@log_path)
      file = File.open(@log_path, "r")
      begin
        loop do
          line = file.gets()
          break if line == nil
          next if line.strip().empty?()
          self.apply(JSON.parse(line))
          @log_lines += 1
        end
      ensure
        file.close()
      end
    end

    # Then reopen for appending, and drop keys that expired while the server
    # was down.
    @log = File.open(@log_path, "a")
    self.sweep(Time.utc_now().to_f())
    @values.length()
  end

  # Reads: an expired key is deleted lazily at the moment it is touched, so
  # it is never returned.
  def get(key: String, now: Float) -> String | Nil
    self.expire_if_due(key, now)
    @values[key]
  end

  # `expires_at` is a Unix time, or nil for no expiry. Setting a key
  # replaces any old expiry.
  def set(key: String, value: String, expires_at)
    @values[key] = value
    if expires_at == nil then @expires.delete(key) else @expires[key] = expires_at end
    self.append(["set", key, value, expires_at])
  end

  # True if the key existed (and was live) to be deleted.
  def delete(key: String, now: Float) -> Bool
    self.expire_if_due(key, now)
    return false unless @values.include_key?(key)
    @values.delete(key)
    @expires.delete(key)
    self.append(["del", key])
    true
  end

  # Seconds left before `key` expires: -1 without an expiry, -2 if absent.
  def ttl(key: String, now: Float) -> Int
    self.expire_if_due(key, now)
    return -2 unless @values.include_key?(key)
    expiry = @expires[key]
    return -1 if expiry == nil
    # Round up, so a key with 0.2 seconds left reports 1 and not 0 (0 would
    # look expired).
    (expiry - now).ceil()
  end

  # Live keys starting with `prefix`, sorted. Sweeps first so expired keys
  # are not listed.
  def keys(prefix: String, now: Float) -> Array
    self.sweep(now)
    @values.keys().select() do |key| key.start_with?(prefix) end.sort()
  end

  def size(now: Float) -> Int
    self.sweep(now)
    @values.length()
  end

  # Drops every expired key. Called on reads that scan and periodically by
  # the server loop, so memory doesn't hold dead keys forever.
  def sweep(now: Float)
    due = @expires.keys().select() do |key| @expires[key] <= now end
    due.each() do |key| self.expire_if_due(key, now) end
  end

  # Rewrites the log as one line per live key, via a temporary file renamed
  # over the old log, once it holds more than twice the lines needed.
  def compact_if_bloated(now: Float) -> Bool
    self.sweep(now)

    # Not worth it unless the log is well over twice the live size (the +16
    # keeps tiny stores from compacting constantly).
    return false if @log_lines <= 2 * @values.length() + 16

    # Write the new log beside the old one, then swap it in. Renaming is
    # atomic, so a crash leaves either the complete old log or the complete
    # new one.
    temporary = "#{@log_path}.compact"
    out = File.open(temporary, "w")
    begin
      @values.keys().each() do |key|
        out.write(JSON.stringify(["set", key, @values[key], @expires[key]]) + "\n")
      end
    ensure
      out.close()
    end

    @log.close()
    File.rename(temporary, @log_path)
    @log = File.open(@log_path, "a")
    @log_lines = @values.length()
    true
  end

  # Closes the log (it was never opened if `load` was not called).
  def close()
    @log.close() unless @log == nil
  end

  private

  # Applies one logged record to the in-memory state (without writing it back
  # to the log: this is the replay path).
  def apply(record: Array)
    case record
    when ["set", key, value, expires_at]
      @values[key] = value
      if expires_at == nil then @expires.delete(key) else @expires[key] = expires_at end
    when ["del", key]
      @values.delete(key)
      @expires.delete(key)
    else
      raise ArgumentError.new("bad log record #{JSON.stringify(record)}")
    end
  end

  # Adds one record to the log and flushes, so it reaches the file before the
  # caller replies to the client.
  def append(record: Array)
    @log.write(JSON.stringify(record) + "\n")
    @log.flush()
    @log_lines += 1
  end

  # If the key's expiry has passed, delete it for real, and log the deletion
  # so a restart does not bring it back.
  def expire_if_due(key: String, now: Float)
    expiry = @expires[key]
    return nil if expiry == nil || expiry > now
    @values.delete(key)
    @expires.delete(key)
    self.append(["del", key])
    nil
  end
end
