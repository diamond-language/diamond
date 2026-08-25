module LogViewer
  def self.metadata_fields() = ["timestamp", "level", "tag", "message"]

  # Known request/query fields get a stable, scannable order. Unknown fields
  # are appended afterward, so new application data is never dropped.
  def self.preferred_fields()
    ["request_id", "method", "path", "status", "duration_ms",
     "query_id", "phase", "operation", "orm", "builder", "sql",
     "bind_count", "rows", "affected", "error", "reason", "user_id",
     "session_id", "project_id", "task_id"]
  end

  def self.append_field(parts, record, key)
    if record.keys().include?(key)
      parts.push("#{key}=#{JSON.stringify(record[key])}")
    end
  end

  def self.format_record(record: Hash)
    timestamp = record["timestamp"]
    level = record["level"]
    tag = record["tag"]
    message = record["message"]
    timestamp = if timestamp == nil then "-" else "#{timestamp}" end
    level = if level == nil then "UNKNOWN" else "#{level}".upcase() end
    tag = if tag == nil then "-" else "#{tag}" end
    message = if message == nil then "-" else "#{message}" end

    fields = []
    preferred = LogViewer.preferred_fields()
    index = 0
    while index < preferred.length()
      LogViewer.append_field(fields, record, preferred[index])
      index += 1
    end

    skipped = LogViewer.metadata_fields().concat(preferred)
    keys = record.keys()
    index = 0
    while index < keys.length()
      key = keys[index]
      unless skipped.include?(key)
        LogViewer.append_field(fields, record, key)
      end
      index += 1
    end

    suffix = if fields.length() == 0 then "" else " " + fields.join(" ") end
    "#{timestamp} #{level} [#{tag}] #{message}#{suffix}"
  end

  # Invalid input remains visible so a mixed or damaged stream can be
  # diagnosed instead of silently losing lines.
  def self.format_line(line: String)
    begin
      [LogViewer.format_record(JSON.parse(line)), true]
    rescue error
      ["[unparsed] #{line}", false]
    end
  end
end
