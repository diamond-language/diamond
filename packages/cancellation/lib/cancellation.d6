module Cancellation
  class Cancelled < StandardError
  end
  class DeadlineExceeded < Cancelled
  end

  # Only the channel's closed bit is shared across VM heaps. No mutable
  # instance fields are relied upon for cross-thread communication.
  def self.duration(value)
    unless value is Int || value is Float
      raise ArgumentError.new("duration must be numeric")
    end
    if value < 0 || !value.to_f().finite?()
      raise ArgumentError.new("duration must be finite and nonnegative")
    end
    value
  end

  class Token
    def initialize(state, parent, deadline)
      @state = state
      @parent = parent
      @deadline = deadline
    end

    def checkpoint()
      if @state.closed?()
        raise Cancelled.new("operation cancelled")
      end
      if @parent != nil then @parent.checkpoint() end
      if @deadline != nil && Time.monotonic() >= @deadline
        raise DeadlineExceeded.new("operation deadline exceeded")
      end
    end

    def wait_channels()
      channels = [@state]
      if @parent != nil then channels = channels.concat(@parent.wait_channels()) end
      channels
    end

    def wait_deadline()
      deadline = @deadline
      if @parent != nil
        parent_deadline = @parent.wait_deadline()
        if parent_deadline != nil && (deadline == nil || parent_deadline < deadline)
          deadline = parent_deadline
        end
      end
      deadline
    end

    def sleep(seconds)
      Cancellation.duration(seconds)
      until_time = Time.monotonic() + seconds
      deadline = self.wait_deadline()
      deadline = until_time if deadline == nil || until_time < deadline
      channels = self.wait_channels()
      loop do
        self.checkpoint()
        break if Time.monotonic() >= until_time
        @state.wait_readable(channels, deadline)
      end
    end

    # Readiness is only a hint. Check cancellation on both sides of the wait,
    # including when a descriptor and the cancellation pipe wake together.
    def poll(readables, writables)
      self.checkpoint()
      result = IO.poll(readables, writables, {"cancellations": self.wait_channels(), "deadline": self.wait_deadline()})
      self.checkpoint()
      result
    end

    # Check cancellation before starting DNS and again before returning results.
    def resolve(host)
      self.checkpoint()
      addresses = DNS.resolve(host, self.wait_channels(), self.wait_deadline())
      self.checkpoint()
      addresses
    end

    def connect(host, port)
      self.checkpoint()
      unless port is Int then raise TypeError.new("connect port must be an Int") end
      if port < 1 || port > 65535 then raise TypeError.new("connect port must be between 1 and 65535") end
      addresses = self.resolve(host)
      last_error = nil
      addresses.each() do |address|
        self.checkpoint()
        begin
          return self.connect_address(address, port)
        rescue error: IOError
          last_error = error
        end
      end
      self.checkpoint()
      if last_error != nil then raise last_error end
      raise IOError.new("hostname has no supported TCP addresses")
    end

    # Return ownership only after both connection completion and a final
    # checkpoint. Failure/cancellation closes the in-progress descriptor.
    def connect_address(address, port)
      self.checkpoint()
      socket = nil
      transferred = false
      begin
        socket = TCPSocket.connect_nonblocking(address, port)
        loop do
          self.checkpoint()
          begin
            socket.finish_connect()
            self.checkpoint()
            transferred = true
            return socket
          rescue error: WouldBlockError
            self.poll([], [socket])
          end
        end
      ensure
        if socket != nil && !transferred then socket.close() end
      end
    end

    # DNS, TCP, and the TLS handshake all use this token's original deadline.
    # Successful application reads/writes retain TLSSocket's blocking contract.
    def connect_tls(host, port, options = nil)
      self.checkpoint()
      tcp = nil
      tls = nil
      transferred = false
      begin
        tcp = self.connect(host, port)
        tls = TLSSocket.start_handshake(tcp, host, options)
        loop do
          self.checkpoint()
          direction = tls.finish_handshake()
          if direction == nil
            self.checkpoint()
            transferred = true
            return tls
          elsif direction == "read"
            self.poll([tls], [])
          else
            self.poll([], [tls])
          end
        end
      ensure
        if tcp != nil then tcp.close() end
        if tls != nil && !transferred then tls.abort() end
      end
    end

    # Nonblocking TCP sockets only: a blocking File/TLS read cannot
    # safely be retried under this cooperative contract.
    def read(socket, count)
      unless socket is Socket then raise TypeError.new("expected a nonblocking Socket") end
      loop do
        self.checkpoint()
        begin
          return socket.read(count)
        rescue error: WouldBlockError
          self.poll([socket], [])
        end
      end
    end

    # Successful return means every byte was written. Cancellation can leave
    # a prefix on the wire; callers must not blindly retry the whole message.
    def write(socket, value)
      unless socket is Socket then raise TypeError.new("expected a nonblocking Socket") end
      unless value is String then raise TypeError.new("expected a String") end
      self.checkpoint()
      offset = 0
      while offset < value.length()
        self.checkpoint()
        begin
          written = socket.write(value.slice(offset, value.length() - offset))
          offset += written
        rescue error: WouldBlockError
          self.poll([], [socket])
        end
      end
      offset
    end

    def receive(channel)
      channels = self.wait_channels()
      deadline = self.wait_deadline()
      loop do
        self.checkpoint()
        begin
          return channel.try_receive()
        rescue error: WouldBlockError
          channel.wait_readable(channels, deadline)
        end
      end
    end

    def send(channel, value)
      channels = self.wait_channels()
      deadline = self.wait_deadline()
      loop do
        self.checkpoint()
        begin
          channel.try_send(value)
          return nil
        rescue error: WouldBlockError
          channel.wait_writable(channels, deadline)
        end
      end
    end
  end

  class Source
    def initialize(parent = nil, timeout = nil)
      if timeout != nil then Cancellation.duration(timeout) end
      @state = Channel.new(1)
      deadline = if timeout == nil then nil else Time.monotonic() + timeout end
      @token = Token.new(@state, parent, deadline)
    end
    def token() = @token
    def cancel() = @state.close()
  end

  # A child error cancels siblings immediately, even if join is currently
  # waiting for an earlier child. Cancellation itself is not a new failure.
  def self.task(source, callback, arguments)
    begin
      callback(source.token(), *arguments)
    rescue error: Cancelled
      nil
    rescue error
      source.cancel()
      raise error
    end
  end

  class Scope
    def initialize(parent = nil, timeout = nil)
      @source = Source.new(parent, timeout)
      @threads = []
      @closed = false
    end
    def token() = @source.token()
    def cancel() = @source.cancel()
    def spawn(callback, *arguments)
      if @closed then raise ArgumentError.new("scope is closed") end
      @source.token().checkpoint()
      thread = Thread.new(Cancellation.task, @source, callback, arguments)
      @threads.push(thread)
      nil
    end
    def join()
      @closed = true
      failure = nil
      failed = false
      @threads.each() do |thread|
        begin
          thread.join()
        rescue error
          failure = error unless failed
          failed = true
          @source.cancel()
        end
      end
      @threads = []
      if failed then raise failure end
    end
    def cleanup()
      begin
        self.close()
      rescue error
        return [false, error]
      end
      [true, nil]
    end
    def close()
      @source.cancel()
      self.join()
    end
  end

  # The body exception takes precedence, but every child is still joined.
  # Normal body completion joins children before leaving the scope.
  def self.scope(callback, timeout = nil)
    scope = Scope.new(nil, timeout)
    failure = nil
    failed = false
    result = nil
    begin
      result = callback(scope)
      scope.join()
      scope.token().checkpoint()
    rescue error
      failure = error
      failed = true
    ensure
      cleanup = scope.cleanup()
    end
    if failed then raise failure end
    if !cleanup[0] then raise cleanup[1] end
    result
  end
end
