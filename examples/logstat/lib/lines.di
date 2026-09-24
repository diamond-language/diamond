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

def logstat_parse(text: String, number: Int) -> LogLine
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
  tag = if event["tag"] is String then event["tag"] else "-" end
  status = event["status"]
  duration = event["duration_ms"]
  if message == "request.completed" && status is Int && (duration is Float || duration is Int)
    RequestLine.new(level, tag, status, duration * 1.0)
  else
    EventLine.new(level, tag, message)
  end
end
