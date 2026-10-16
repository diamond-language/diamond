# break/next from inside begin/rescue/ensure within a loop leave those blocks properly: ensure
# bodies run, once each and in order, and the handler stack stays consistent afterwards.
log = []

def trace(log, label)
  log.push(label)
end

def next_in_begin(log)
  i = 0
  while i < 3
    i += 1
    begin
      next if i == 1
      break if i == 3
      trace(log, "body #{i}")
    ensure
      trace(log, "ensure #{i}")
    end
  end
  "after loop"
end

def break_in_rescue(log)
  begin
    loop do
      begin
        raise IOError.new("x")
      rescue error: IOError
        trace(log, "rescued")
        break
      ensure
        trace(log, "inner ensure")
      end
    end
    "value after loop"
  ensure
    trace(log, "outer ensure")
  end
end

def nested(log)
  count = 0
  loop do
    count += 1
    begin
      begin
        break if count == 2
        next
      ensure
        trace(log, "inner #{count}")
      end
    ensure
      trace(log, "outer #{count}")
    end
  end
  count
end

def until_with_rescue(log)
  n = 0
  until n >= 5
    n += 1
    begin
      raise IOError.new("boom") if n % 2 == 1
      trace(log, "even #{n}")
    rescue error: IOError
      next
    ensure
      trace(log, "done #{n}")
    end
    break if n == 4
  end
  n
end

[next_in_begin(log), break_in_rescue(log), nested(log), until_with_rescue(log), log]
