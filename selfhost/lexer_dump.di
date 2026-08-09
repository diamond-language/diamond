require "lexer"

path = gets()
source = File.open(path, "r").read()
lexer = Lexer.new(source)
loop do
  t = lexer.next_token()
  puts("#{t.kind()} #{t.start()} #{t.length()} #{t.line()} #{t.column()}")
  break if t.kind() == :eof
end
