# Everyone currently connected, plus the last few chat lines so a new arrival
# sees some context. gremlin_serve runs one worker here, so every connection's
# fiber shares this heap and these class variables.
class Room
  def self.history_limit() = 20

  # The lists are created on first use (class variables start out nil).
  # Each member is a Hash {"ws": the connection, "name": display name}.
  def self.members()
    if @@members == nil then @@members = [] end
    @@members
  end

  def self.history()
    if @@history == nil then @@history = [] end
    @@history
  end

  # The display names, in join order.
  def self.names()
    Room.members().map() do |member|
      member["name"]
    end
  end

  # Is this name in use? (Names must be unique.)
  def self.taken?(name)
    Room.members().any?() do |member|
      member["name"] == name
    end
  end

  # Adds a member. The caller has already checked the name is free.
  def self.join(ws, name)
    Room.members().push({"ws": ws, "name": name})
  end

  # Returns whether `ws` was still a member, so a departure is announced once
  # even when both a failed broadcast and the connection's own fiber notice it.
  def self.leave(ws)
    before = Room.members().length()
    @@members = Room.members().select() do |member|
      member["ws"] != ws
    end
    @@members.length() < before
  end

  # Sends one event to every member. A connection that can't take the whole
  # frame immediately is closed and dropped rather than stalling the sender's
  # fiber; see websocket_try_send_text in the websocket cut.
  # Dropping a member produces a "left" event of its own, so events are
  # processed as a queue until nothing new is queued.
  def self.broadcast(event)
    pending = [event]

    while pending.length() > 0
      current = pending[0]
      pending = pending.drop(1)

      # Chat lines and bot replies are remembered for newcomers (only the
      # newest `history_limit`); joins and leaves are not.
      if current["type"] == "message" || current["type"] == "bot"
        Room.history().push(current)
        if Room.history().length() > Room.history_limit()
          @@history = Room.history().drop(Room.history().length() - Room.history_limit())
        end
      end

      # Serialize once, send to everyone, collecting any member that could not
      # take it.
      text = JSON.stringify(current)
      dropped = []
      Room.members().each() do |member|
        unless websocket_try_send_text(member["ws"], text)
          dropped.push(member)
        end
      end

      # Remove the failures. Each removal queues a "left" event of its own
      # (which may in turn drop more members), hence the queue.
      dropped.each() do |member|
        if Room.leave(member["ws"])
          pending.push({"type": "left", "name": member["name"], "members": Room.names()})
        end
      end
    end
  end
end
