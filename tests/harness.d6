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
      @all_out = ""
      @all_err = ""
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
        if is_out
          @out = @out + chunk
          @all_out = @all_out + chunk
        else
          @err = @err + chunk
          @all_err = @all_err + chunk
        end
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

    def send(text: String) = @handle.stdin().write(text)

    def terminate() = @handle.terminate()
    def kill() = @handle.kill()
    def running?() = @handle.running?()
    def wait() = @handle.wait()
    def errors() = @all_err

    # Everything the program has printed so far, on each stream (not consumed by next_line).
    def stdout_text()
      self.pump(0)
      @all_out
    end

    def stderr_text()
      self.pump(0)
      @all_err
    end

    # Waits until `text` appears on stdout or stderr.
    def wait_for_text(text: String, timeout_ms: Int)
      deadline = Time.monotonic() + timeout_ms / 1000.0
      loop do
        self.pump(20)
        return true if @all_out.include?(text) || @all_err.include?(text)
        raise "the program under test exited before printing #{text.inspect()}" unless @handle.running?()
        raise "timed out waiting for #{text.inspect()}" if Time.monotonic() > deadline
      end
    end

    # The exit code, waiting at most `timeout_ms`; nil when it is still running.
    def wait_exit(timeout_ms: Int)
      deadline = Time.monotonic() + timeout_ms / 1000.0
      loop do
        self.pump(0)
        return @handle.wait() unless @handle.running?()
        return nil if Time.monotonic() > deadline
        Harness.sleep_ms(10)
      end
    end

    # Releases the child whatever state it is in.
    def finish()
      if @handle.running?()
        @handle.kill()
      end
      @handle.wait()
    end
  end

  # A long-running program whose output goes to a log file (nothing has to keep draining a pipe),
  # for servers under test. `log_text()` is everything it has printed, stdout and stderr in order.
  class Service
    def initialize(argv: Array, env: Hash, cwd, log_path: String)
      @log_path = log_path
      options = {"env": env, "stdout": log_path, "stderr_to_stdout": true}
      options["cwd"] = cwd if cwd != nil
      @handle = Process.spawn(argv, options)
    end

    def pid() = @handle.pid()
    def running?() = @handle.running?()
    def terminate() = @handle.terminate()
    def kill() = @handle.kill()
    def log_text() = File.exist?(@log_path) ? File.read(@log_path) : ""

    # The exit code, waiting at most `timeout_ms`; nil while it is still running.
    def wait_exit(timeout_ms: Int)
      deadline = Time.monotonic() + timeout_ms / 1000.0
      loop do
        return @handle.wait() unless @handle.running?()
        return nil if Time.monotonic() > deadline
        Harness.sleep_ms(10)
      end
    end

    # Releases the program whatever state it is in.
    def finish()
      @handle.kill() if @handle.running?()
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

  # One request over a fresh connection; [status, body]. `body` is a String sent as JSON, or nil.
  # Raises IOError when the server is not accepting connections.
  def self.http(port: Int, method: String, path: String, body)
    socket = TCPSocket.connect("127.0.0.1", port,
      {"connect_timeout_ms": 2000, "read_timeout_ms": 2000, "write_timeout_ms": 2000})
    begin
      head = "#{method} #{path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
      if body != nil
        head = head + "Content-Type: application/json\r\nContent-Length: #{body.length()}\r\n"
      end
      socket.write(head + "\r\n" + (body == nil ? "" : body))
      socket.flush()
      status_line = socket.gets()
      raise IOError.new("no response") if status_line == nil
      status = status_line.split(" ")[1].to_i()
      length = 0
      loop do
        header = socket.gets()
        break if header == nil || header.strip() == ""
        if header.downcase().start_with?("content-length:")
          length = header.split(":")[1].strip().to_i()
        end
      end
      text = length > 0 ? socket.read(length) : ""
      [status, text == nil ? "" : text]
    ensure
      socket.close()
    end
  end

  def self.http_get(port: Int, path: String) = Harness.http(port, "GET", path, nil)

  # Number of open descriptors of a process, or nil where /proc does not exist.
  def self.descriptor_count(pid: Int)
    path = "/proc/#{pid}/fd"
    return nil unless File.directory?(path)
    Dir.entries(path).length()
  end

  def self.sleep_ms(milliseconds: Int) = IO.poll([], [], milliseconds)

  # Polls `probe` (a block) until it is truthy, up to `timeout_ms`.
  def self.wait_for(timeout_ms: Int, message: String, &block)
    deadline = Time.monotonic() + timeout_ms / 1000.0
    loop do
      return true if yield()
      raise message if Time.monotonic() > deadline
      Harness.sleep_ms(10)
    end
  end

  # One HTTPS request over a fresh connection, trusting `ca_file`. Returns [status, headers, body]:
  # headers is a Hash with lower-case names, and the body is read by Content-Length (or to EOF).
  # `headers_in` is a Hash of request headers; `body` a String or nil.
  def self.https(host: String, port: Int, ca_file: String, method: String, path: String, body, headers_in)
    socket = TLSSocket.connect(host, port,
      {"ca_file": ca_file, "connect_timeout_ms": 30000, "read_timeout_ms": 30000, "write_timeout_ms": 30000})
    begin
      head = "#{method} #{path} HTTP/1.1\r\nHost: #{host}\r\nConnection: close\r\n"
      headers_in.each() do |name, value|
        head = head + "#{name}: #{value}\r\n"
      end
      head = head + "Content-Length: #{body.length()}\r\n" if body != nil
      socket.write(head + "\r\n" + (body == nil ? "" : body))
      status_line = socket.gets()
      raise IOError.new("no response") if status_line == nil
      status = status_line.split(" ")[1].to_i()
      headers = {}
      loop do
        line = socket.gets()
        break if line == nil || line.strip() == ""
        colon = line.index(":")
        next if colon == nil
        headers[line[0, colon].strip().downcase()] = line[colon + 1, line.length() - colon - 1].strip()
      end
      text = nil
      if headers.key?("content-length")
        length = headers["content-length"].to_i()
        text = length > 0 ? socket.read(length) : ""
      else
        text = socket.read()
      end
      [status, headers, text == nil ? "" : text]
    ensure
      socket.close()
    end
  end

  # One plain-HTTP request to 127.0.0.1:`port` with the headers given and an optional body:
  # [status, body].
  def self.http_with_headers(port: Int, method: String, path: String, body, headers_in)
    socket = TCPSocket.connect("127.0.0.1", port,
      {"connect_timeout_ms": 2000, "read_timeout_ms": 2000, "write_timeout_ms": 2000})
    begin
      head = "#{method} #{path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
      headers_in.each() do |name, value|
        head = head + "#{name}: #{value}\r\n"
      end
      head = head + "Content-Length: #{body.length()}\r\n" if body != nil
      socket.write(head + "\r\n" + (body == nil ? "" : body))
      socket.flush()
      status_line = socket.gets()
      raise IOError.new("no response") if status_line == nil
      length = nil
      loop do
        line = socket.gets()
        break if line == nil || line.strip() == ""
        length = line.split(":")[1].strip().to_i() if line.downcase().start_with?("content-length:")
      end
      text = length == nil ? socket.read() : (length > 0 ? socket.read(length) : "")
      [status_line.split(" ")[1].to_i(), text == nil ? "" : text]
    ensure
      socket.close()
    end
  end

  # Removes a directory and everything in it; a symlink is removed, never followed.
  def self.remove_tree(path: String)
    Dir.entries(path).each() do |name|
      child = path + "/" + name
      if File.directory?(child) && !File.symlink?(child)
        Harness.remove_tree(child)
      else
        File.delete(child)
      end
    end
    Dir.delete(path)
  end

  # A new temporary directory.
  def self.make_temp_dir() = Process.run(["mktemp", "-d"]).stdout().strip()

  # [0, 1, ..., count - 1].
  def self.sequence(count: Int)
    items = []
    count.times() do |i|
      items.push(i)
    end
    items
  end

  # Creates `path` and any missing parents.
  def self.mkdir_p(path: String)
    return nil if File.directory?(path)
    parent = File.dirname(path)
    Harness.mkdir_p(parent) unless parent == path || File.directory?(parent)
    Dir.mkdir(path)
  end

  # `command` run to completion; the Process::Result. `env` and `cwd` are optional (nil).
  def self.run(argv: Array, env, cwd)
    options = {}
    options["env"] = env if env != nil
    options["cwd"] = cwd if cwd != nil
    Process.run(argv, options)
  end
end
