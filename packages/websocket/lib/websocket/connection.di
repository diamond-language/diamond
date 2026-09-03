# A single, already-upgraded WebSocket connection -- everything past the
# handshake (packages/websocket/lib/websocket/handshake.di) belongs
# here: fragment reassembly and transparent ping/pong/close handling on
# top of frame.di's own single-frame encode/decode.
class WebSocketConnection
  def initialize(conn)
    @conn = conn
    @closed = false
  end

  def send_text(text)
    self.send_frame(websocket_opcode_text(), text)
  end

  def send_binary(data)
    self.send_frame(websocket_opcode_binary(), data)
  end

  def send_frame(opcode, payload)
    if @closed
      raise IOError.new("cannot write to a closed WebSocketConnection")
    end
    @conn.write(websocket_encode_frame(opcode, payload))
  end

  def closed?() = @closed

  # The raw, underlying connection (gremlin's own NonblockingConnection,
  # or any object with the same #read(n)/#write(value)/#close()
  # surface) -- an escape hatch for websocket_try_send_text
  # (broadcast.di), which needs to write to *another* connection's
  # socket directly from a completely different fiber's own call stack.
  # Not needed for anything else; every ordinary send goes through
  # #send_text/#send_binary above instead.
  def raw_socket() = @conn.socket()

  # An immediate, no-handshake close -- unlike #close below, this never
  # sends a Close frame or waits for one back. Only websocket_try_send_text
  # (broadcast.di) has a legitimate reason to reach for this: once it's
  # written part of a frame to this connection from another fiber and
  # then hit backpressure, this connection's own byte stream is
  # unrecoverably desynced (RFC 6455 framing has no way to resume mid-
  # frame), so the only safe thing left to do is close it outright, the
  # same way a real IRC server disconnects a client that can't keep up
  # rather than risk a corrupted stream.
  def force_close()
    @closed = true
    @conn.close()
  end

  # One fully reassembled message -- {"opcode": TEXT|BINARY, "data":
  # String} -- or `nil` once the close handshake has completed (the peer
  # sent a Close frame, this side echoed one back, and the underlying
  # connection has been closed). The same "nil means done" contract
  # gremlin_worker's own request/response loop already uses per
  # connection, so a typical echo/chat-style handler can just
  # `while (message = ws.receive()) != nil`.
  #
  # Ping/Pong are handled transparently here, never surfaced to the
  # caller -- RFC 6455 sections 5.5.2/5.5.3 require a Pong reply to
  # every Ping "as soon as is practical", and nothing above this layer
  # has any legitimate use for seeing one. A Ping/Pong (or a second,
  # unrelated Close) can legally arrive *between* the fragments of a
  # still-in-progress message (RFC 6455 section 5.4) -- `fragmenting`
  # below tracks message-in-progress state independently of the frame
  # loop itself for exactly that reason, rather than assuming the next
  # frame after a fragment is always its continuation.
  def receive()
    message_opcode = nil
    buffer = StringBuilder.new()
    fragmenting = false
    loop do
      frame = websocket_read_frame(@conn)
      opcode = frame["opcode"]
      if opcode == websocket_opcode_ping()
        self.send_frame(websocket_opcode_pong(), frame["payload"])
      elsif opcode == websocket_opcode_pong()
        nil
      elsif opcode == websocket_opcode_close()
        self.handle_close_frame(frame["payload"])
        return nil
      elsif opcode == websocket_opcode_continuation()
        unless fragmenting
          raise IOError.new("WebSocket continuation frame with no message in progress")
        end
        buffer.append(frame["payload"])
        if frame["fin"]
          return {"opcode": message_opcode, "data": buffer.to_s()}
        end
      else
        if fragmenting
          raise IOError.new("WebSocket new data frame received before previous fragmented message finished")
        end
        message_opcode = opcode
        buffer.append(frame["payload"])
        if frame["fin"]
          return {"opcode": message_opcode, "data": buffer.to_s()}
        else
          fragmenting = true
        end
      end
    end
  end

  # A Close frame the *peer* initiated: echo the first two bytes (the
  # status code, if the peer sent one at all -- a code-less Close is
  # valid too) back per RFC 6455 section 5.5.1 ("the application MUST
  # send a Close frame in response"), then close the underlying
  # connection. Guarded by @closed so a Close arriving after this side
  # already sent its own (see #close below, which reads frames itself
  # waiting for exactly this one) doesn't try to send a second reply
  # onto an already-completing close handshake.
  def handle_close_frame(payload)
    unless @closed
      code_bytes = if payload.length() >= 2 then payload.slice(0, 2) else "" end
      self.send_frame(websocket_opcode_close(), code_bytes)
      @closed = true
    end
    @conn.close()
  end

  # A normal, this-side-initiated close (`code` defaults to 1000,
  # "Normal Closure", RFC 6455 section 7.4.1): sends a Close frame, then
  # waits for the peer's own Close frame back (or the connection simply
  # dropping) before closing the socket -- giving the peer a chance to
  # finish whatever it was sending, the same graceful-drain spirit as
  # GremlinShutdown's own SIGTERM handling elsewhere in this stack.
  # Ping/Pong frames arriving during the wait are not specially handled
  # here (unlike #receive) -- once this side has already sent its own
  # Close, RFC 6455 section 5.5.1 only obliges it to wait for the peer's
  # Close in return, not to keep servicing the full protocol.
  def close(code = 1000, reason = "")
    unless @closed
      payload = "#{chr((code >> 8) & 255)}#{chr(code & 255)}#{reason}"
      self.send_frame(websocket_opcode_close(), payload)
      @closed = true
      begin
        loop do
          frame = websocket_read_frame(@conn)
          if frame["opcode"] == websocket_opcode_close()
            break
          end
        end
      rescue error: IOError
        nil
      end
      @conn.close()
    end
  end
end
