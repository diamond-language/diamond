class NonblockingConnection
  def initialize(socket)
    @socket = socket
    @buffer = ""
    @eof = false
  end

  def socket() = @socket

  # Blocks (via yield, not the OS thread) until either more data has
  # arrived or the peer has closed -- never returns with nothing to show
  # for it. Every read/write path below funnels through this and #write's
  # own retry loop, so WouldBlockError never escapes NonblockingConnection
  # itself.
  def fill_more()
    loop do
      begin
        chunk = @socket.read(4096)
        if chunk == nil
          @eof = true
        else
          @buffer = @buffer + chunk
        end
        return nil
      rescue error: WouldBlockError
        yield
      end
    end
  end

  def gets()
    loop do
      newline = @buffer.index_of("\n")
      unless newline == nil
        line = @buffer.slice(0, newline)
        if line.length() > 0 && line.slice(line.length() - 1, 1) == "\r"
          line = line.slice(0, line.length() - 1)
        end
        @buffer = @buffer.slice(newline + 1, @buffer.length())
        return line
      end
      if @eof
        if @buffer.length() == 0
          return nil
        end
        line = @buffer
        @buffer = ""
        return line
      end
      self.fill_more()
    end
  end

  # Same convention as File#read(n): reads up to n bytes and returns
  # whatever it got, including "" if the peer hit EOF with nothing left --
  # never nil (that's #gets' own EOF signal, not #read's).
  def read(n)
    while @buffer.length() < n && @eof == false
      self.fill_more()
    end
    took = @buffer.length()
    if took > n
      took = n
    end
    result = @buffer.slice(0, took)
    @buffer = @buffer.slice(took, @buffer.length())
    result
  end

  def write(value)
    remaining = value
    while remaining.length() > 0
      begin
        written = @socket.write(remaining)
        remaining = remaining.slice(written, remaining.length())
      rescue error: WouldBlockError
        yield
      end
    end
    nil
  end

  def close()
    @socket.close()
  end
end
