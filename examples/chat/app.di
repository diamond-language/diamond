# Entry point. The wiring lives in boot.di so the code reads as a short
# script here.
require "./boot"

# Port from $CHAT_PORT (so tests can pick a free one), else 18095.
port = if ENV["CHAT_PORT"] == nil then 18095 else ENV["CHAT_PORT"].to_i() end

# Start the bot's supervised thread first, so it is ready when the first
# client sends a command.
Bot.start()
puts("chat listening on http://127.0.0.1:#{port}")
# One worker (so every connection shares the Room), with a 50ms tick that
# forwards bot replies. gremlin_serve returns after SIGINT/SIGTERM.
gremlin_serve(port, chat_handler, 1, 0.05, chat_tick)

# Reached after the server stops: shut the bot down cleanly.
Bot.stop()
