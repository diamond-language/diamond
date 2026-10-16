# Support for test drivers that run a program under test as a child process and talk to it
# over its stdin and stdout and over TCP. Replaces the Python drivers: the dependency set for
# running the tests is the built `diamond` binary itself.
#
#   require "../../../tests/harness"
#
# Every wait here is bounded, so a hung child fails the test instead of the build.
module Harness
  # The program under test. Output is read a line at a time with a deadline; stderr is kept
  # so a failure can show it.
  class Child
    def initialize(argv: Array, env: Hash)
      @handle = Process.spawn(argv, {"env": env, "stdin": true})
      @out = ""
      @err = ""
      @open = true
    end

    def pid() = @handle.pid()

    # Moves whatever the child has written so far into the buffers, waiting up to
    # `timeout_ms` for something to arrive.
    def pump(timeout_ms: Int)
      IO.poll([@handle.stdout(), @handle.stderr()], [], timeout_ms)
      self.drain(@handle.stdout(), true)
      self.drain(@handle.stderr(), false)
    end

    def drain(stream, is_out: Bool)
      loop do
        chunk = nil
        begin
          chunk = stream.read(65536)
        rescue error: WouldBlockError
          break
        end
        break if chunk == nil
        if is_out then @out = @out + chunk else @err = @err + chunk end
      end
    end

    # The next line the child prints, within `timeout_ms`.
    def next_line(timeout_ms: Int)
      deadline = Time.monotonic() + timeout_ms / 1000.0
      loop do
        newline = @out.index("\n")
        if newline != nil
          text = @out[0, newline]
          @out = @out[newline + 1, @out.length() - newline - 1]
          return text.strip()
        end
        remaining = ((deadline - Time.monotonic()) * 1000).to_i()
        raise "timed out waiting for output from the program under test" if remaining <= 0
        self.pump(remaining < 50 ? remaining : 50)
        if !@handle.running?()
          self.pump(0)
          if @out.index("\n") == nil
            raise "the program under test exited while a line was expected"
          end
        end
      end
    end

    # Reads the next line and checks it.
    def line(expected: String, timeout_ms: Int)
      actual = self.next_line(timeout_ms)
      raise "expected #{expected.inspect()}, the program printed #{actual.inspect()}" if actual != expected
    end

    def send(text: String)
      @handle.stdin().write(text)
    end

    def terminate() = @handle.terminate()
    def kill() = @handle.kill()
    def running?() = @handle.running?()
    def wait() = @handle.wait()
    def errors() = @err

    # Releases the child whatever state it is in.
    def finish()
      if @handle.running?()
        @handle.kill()
      end
      @handle.wait()
    end
  end

  # A port the kernel just handed out and that is free again; something else may take it
  # before the program under test binds it, which `retry_port` accounts for.
  def self.reserve_port()
    listener = TCPServer.listen(0, host: "127.0.0.1")
    port = listener.port()
    listener.close()
    port
  end

  # Runs the block, retrying on a new port when the program under test could not bind the
  # one that was reserved (the block raises PortTaken then).
  class PortTaken < StandardError
  end

  def self.with_port(attempts: Int, &block)
    attempt = 0
    loop do
      attempt += 1
      begin
        return yield(Harness.reserve_port())
      rescue error: PortTaken
        raise error if attempt >= attempts
      end
    end
  end

  # Number of open descriptors of a process, or nil where /proc does not exist.
  def self.descriptor_count(pid: Int)
    path = "/proc/#{pid}/fd"
    return nil unless File.directory?(path)
    Dir.entries(path).length()
  end

  def self.sleep_ms(milliseconds: Int)
    IO.poll([], [], milliseconds)
  end

  # Polls `probe` (a block) until it is truthy, up to `timeout_ms`.
  def self.wait_for(timeout_ms: Int, message: String, &block)
    deadline = Time.monotonic() + timeout_ms / 1000.0
    loop do
      return true if yield()
      raise message if Time.monotonic() > deadline
      Harness.sleep_ms(10)
    end
  end
end
