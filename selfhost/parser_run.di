require "parser"

path = gets()
source = File.open(path, "r").read()
puts(parse_and_run(source))
