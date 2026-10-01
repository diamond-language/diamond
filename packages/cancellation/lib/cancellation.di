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
