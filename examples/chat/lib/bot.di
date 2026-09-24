# A chat bot on its own supervised thread. Threads share no memory, so it
# talks to the server only through two Channels: commands arrive on `inbox`,
# replies leave on `outbox`. `/crash` raises on purpose; the Supervisor starts
# a fresh attempt with the same channels, and the room is told about it.

# A number in 1..sides from the system CSPRNG (hex digits, since SecureRandom
# exposes bytes and hex rather than ranges). The modulo bias over 2^32 is
# negligible for dice.
def chat_roll(sides: Int) -> Int
  value = 0
  SecureRandom.hex(4).chars().each() do |digit|
    value = value * 16 + "0123456789abcdef".index_of(digit)
  end
  value % sides + 1
end

def chat_bot_reply(command)
  words = command["text"].split(" ")
  name = command["from"]
  verb = words[0]
  if verb == "/help"
    "Commands: /roll [sides], /time, /stats, /crash, /help"
  elsif verb == "/roll"
    sides = if words.length() > 1 then words[1].to_i() else 6 end
    if sides < 2 || sides > 1000
      "#{name}: /roll takes a number of sides from 2 to 1000"
    else
      "#{name} rolled #{chat_roll(sides)} (d#{sides})"
    end
  elsif verb == "/time"
    "Server time is #{Time.now().strftime("%H:%M:%S %Z")}"
  elsif verb == "/crash"
    raise RuntimeError.new("#{name} asked the bot to crash")
  else
    "#{name}: unknown command #{verb}; try /help"
  end
end

# The supervised child. A clean return (the server closed `inbox` on shutdown)
# ends it for good; an exception is a crash and gets restarted.
def chat_bot(inbox, outbox)
  loop do
    command = inbox.receive()
    break if command == nil
    outbox.send({"text": chat_bot_reply(command)})
  end
end

class Bot
  def self.start()
    @@inbox = Channel.new(64)
    @@outbox = Channel.new(64)
    @@supervisor = Supervisor.new()
    @@supervisor.add_child(chat_bot, @@inbox, @@outbox)
    @@restarts_seen = 0
  end

  # Returns false when the bot is backlogged, so a flood of commands can't
  # block the connection fiber that received them.
  def self.ask(name, text)
    begin
      @@inbox.try_send({"from": name, "text": text})
      true
    rescue error: WouldBlockError
      false
    end
  end

  def self.restarts() = @@supervisor.restart_count(0)

  # Called from gremlin's tick: forwards queued replies and announces
  # restarts. try_receive never blocks the server loop.
  def self.drain()
    loop do
      reply = nil
      begin
        reply = @@outbox.try_receive()
      rescue error: WouldBlockError
        break
      end
      Room.broadcast({"type": "bot", "text": reply["text"]})
    end
    restarts = Bot.restarts()
    if restarts != @@restarts_seen
      @@restarts_seen = restarts
      Room.broadcast({"type": "bot", "text": "Bot restarted by its supervisor (#{restarts} so far): #{@@supervisor.last_error(0)}"})
    end
  end

  # Closing the inbox lets the bot's blocking receive return nil, so stop()
  # can finish instead of waiting on a thread that never returns.
  def self.stop()
    @@inbox.close()
    @@supervisor.stop()
  end
end
