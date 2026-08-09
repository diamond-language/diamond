module Config
 NAMES = ["diamond"]
 def names() = NAMES
end
class Reader
 include Config
end
Reader.new().names()
