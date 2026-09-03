# PUBLISH is an ordinary command -- works on any RedisConnection, no
# special connection state involved (reopens RedisConnection, see
# connection.di).
class RedisConnection
  # Number of subscribers that received it (0 if no one's listening on
  # `channel` right now -- Redis pub/sub has no queueing or delivery
  # guarantee at all: a message published with nobody subscribed is
  # simply gone).
  def publish(channel, message) = self.command("PUBLISH", channel, message)
end

# A dedicated connection for the subscriber side of pub/sub -- kept
# entirely separate from RedisConnection because SUBSCRIBE (or
# PSUBSCRIBE) puts *that* connection into a mode where the server will
# only accept more (P)SUBSCRIBE/(P)UNSUBSCRIBE on it, not ordinary
# commands; mixing the two roles on one connection would make GET/SET/
# etc. silently stop working the moment anything ever subscribed to
# anything on it. #receive loops like WebSocketConnection#receive
# (packages/websocket) already does -- block for the next message,
# `while (event = sub.receive()) != nil` -- the same shape for the same
# reason (a long-lived push feed, not a request/response round trip).
class RedisSubscriber
  def initialize(socket)
    @socket = socket
  end

  def self.connect(host, port = 6379, options = nil)
    RedisSubscriber.new(TCPSocket.connect(host, port, options))
  end

  def subscribe(*channels)
    @socket.write(redis_encode_command(["SUBSCRIBE"].concat(channels)))
  end

  def psubscribe(*patterns)
    @socket.write(redis_encode_command(["PSUBSCRIBE"].concat(patterns)))
  end

  def unsubscribe(*channels)
    @socket.write(redis_encode_command(["UNSUBSCRIBE"].concat(channels)))
  end

  def punsubscribe(*patterns)
    @socket.write(redis_encode_command(["PUNSUBSCRIBE"].concat(patterns)))
  end

  # Blocks for the next push reply off this connection -- a
  # subscribe/unsubscribe confirmation (payload: the Int count of
  # channels/patterns now subscribed) or an actual published message
  # (payload: the published data). A pattern-matched message
  # ("pmessage", from a PSUBSCRIBE) carries one extra element (the
  # pattern that matched, ahead of the concrete channel it matched on)
  # -- both shapes normalize to the same Hash here, with "pattern" nil
  # for the plain (non-pattern) case rather than the caller needing to
  # branch on reply length itself.
  def receive()
    reply = redis_read_reply(@socket)
    if reply.length() == 4
      {"type": reply[0], "pattern": reply[1], "channel": reply[2], "payload": reply[3]}
    else
      {"type": reply[0], "pattern": nil, "channel": reply[1], "payload": reply[2]}
    end
  end

  def close()
    @socket.close()
  end
end
