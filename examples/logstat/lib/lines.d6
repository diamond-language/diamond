# One input line, classified. LogLine is sealed, so a `case` over it must
# handle every subclass or fail to compile -- adding a fourth kind of line
# makes every unhandled `case` an error instead of a silent gap.
sealed class LogLine
end

# A finished HTTP request, as logged by gremlin/registry-style servers:
# {"message": "request.completed", "status": 200, "duration_ms": 1.5, ...}
class RequestLine < LogLine
  def initialize(level: String, tag: String, status: Int, duration_ms: Float)
    @level = level
    @tag = tag
    @status = status
    @duration_ms = duration_ms
  end
  def level() = @level
  def tag() = @tag
  def status() = @status
  def duration_ms() = @duration_ms
end

# Any other structured log object with a level and a message.
class EventLine < LogLine
  def initialize(level: String, tag: String, message: String)
    @level = level
    @tag = tag
    @message = message
  end
  def level() = @level
  def tag() = @tag
  def message() = @message
end

# A line that isn't a JSON log object; kept with its line number for the report.
class Unparsed < LogLine
  def initialize(number: Int, reason: String)
    @number = number
    @reason = reason
  end
  def number() = @number
  def reason() = @reason
end

# Classifies one input line. Never raises: anything wrong with the line
# becomes an Unparsed value, so one bad line cannot stop a long run.
def logstat_parse(text: String, number: Int) -> LogLine
  # Three successive checks, each with its own reason: valid JSON, a JSON
  # object (not an array or a bare string), and having the two fields every
  # log line must carry. `is Hash`/`is String` test the runtime type.
  event = nil
  begin
    event = JSON.parse(text)
  rescue error: JSONError
    return Unparsed.new(number, "not JSON")
  end
  unless event is Hash then return Unparsed.new(number, "not a JSON object") end

  level = event["level"]
  message = event["message"]
  unless level is String && message is String
    return Unparsed.new(number, "missing level or message")
  end

  # `tag` is optional ("-" when absent or not text). Note `status` and
  # `duration` can be nil here; the checks in the `if` below handle that.
  tag = if event["tag"] is String then event["tag"] else "-" end
  status = event["status"]
  duration = event["duration_ms"]

  # A completed request needs an integer status and a numeric duration. A
  # whole-number duration may be written without a decimal point (2, not
  # 2.0), so both Int and Float are accepted; `* 1.0` normalizes to Float.
  # Anything else with a message is a plain event.
  if message == "request.completed" && status is Int && (duration is Float || duration is Int)
    RequestLine.new(level, tag, status, duration * 1.0)
  else
    EventLine.new(level, tag, message)
  end
end
