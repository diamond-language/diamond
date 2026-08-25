# A small, leveled logger. Install via facet (see diamond.cut), then
# `require_cut "logger"` to bring in the Logger class.
#
#   log = Logger.new("gremlin", "info", nil, "json")
#   log.info("server.listening", {"port": 8080})
#   log.error("request.failed", {"error": error.message()})
#   # => [2026-08-25 12:00:00] INFO gremlin: listening on port 8080
#   # => [2026-08-25 12:00:01] ERROR gremlin: unhandled error in request handler: ...
#
# Replaces the ad-hoc `puts("gremlin: ...")` shape a few packages
# (gremlin's own request-handler error path, rack's own README example)
# had been hand-rolling -- one real component instead of every package
# reinventing "prefix a string and print it."
#
# Four levels, low to high severity: debug, info, warn, error. #level
# is the *minimum* level a call actually emits at -- a Logger.new(tag,
# level: "warn") silently drops #debug/#info calls, letting an app turn
# down verbosity without touching call sites. Every line is timestamped
# (Time.now().strftime, local time) and tagged with the Logger's own
# `tag` (the component name, e.g. "gremlin"), matching the prefix the
# ad-hoc puts() calls this replaces already used by hand. Text output is
# the backward-compatible default; JSON emits one object per line and
# accepts structured fields on every level method.
#
# `output`, if given, is anything responding to `.write(value)` the
# same way File/TCPSocket already do (docs/io.md) -- a real file to log
# to instead of the console, or a test double. Defaults to nil, meaning
# stdout via the ordinary `puts` builtin; `.write` itself never appends
# its own trailing newline (matching File/TCPSocket), so a real
# destination gets one added explicitly here instead.
class Logger
  def self.level_rank(name: String) -> Int
    if name == "debug"
      0
    elsif name == "info"
      1
    elsif name == "warn"
      2
    elsif name == "error"
      3
    else
      raise ArgumentError.new("Logger: unknown level '#{name}' -- expected debug, info, warn, or error")
    end
  end

  def initialize(tag: String, level: String = "info", output = nil, format: String = "text")
    @tag = tag
    @level = Logger.level_rank(level)
    @output = output
    if format != "text" && format != "json"
      raise ArgumentError.new("Logger: unknown format '#{format}' -- expected text or json")
    end
    @format = format
  end

  def debug(message, fields: Hash = {}) = self.emit("DEBUG", 0, message, fields)
  def info(message, fields: Hash = {}) = self.emit("INFO", 1, message, fields)
  def warn(message, fields: Hash = {}) = self.emit("WARN", 2, message, fields)
  def error(message, fields: Hash = {}) = self.emit("ERROR", 3, message, fields)

  private

  def emit(label: String, rank: Int, message, fields: Hash)
    if rank >= @level
      timestamp = Time.now().strftime("%Y-%m-%dT%H:%M:%S%z")
      line = if @format == "json"
        JSON.stringify(fields.merge({"timestamp": timestamp, "level": label.downcase(), "tag": @tag, "message": "#{message}"}))
      else
        "[#{Time.now().strftime("%Y-%m-%d %H:%M:%S")}] #{label} #{@tag}: #{message}"
      end
      if @output == nil
        puts(line)
      else
        @output.write(line + "\n")
      end
    end
  end
end
