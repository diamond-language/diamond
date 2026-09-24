require "./boot"

port = if ENV["CHAT_PORT"] == nil then 18095 else ENV["CHAT_PORT"].to_i() end
Bot.start()
puts("chat listening on http://127.0.0.1:#{port}")
# One worker (so every connection shares the Room), with a 50ms tick that
# forwards bot replies. gremlin_serve returns after SIGINT/SIGTERM.
gremlin_serve(port, chat_handler, 1, 0.05, chat_tick)
Bot.stop()
