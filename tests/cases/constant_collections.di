LIMIT = 10
OPTIONS = {"limit": 1}
module Settings
  LIMIT = 30
  class Config
    LIMIT = 20
    def values() = [1, 2].map() do |n|
      LIMIT + n
    end
  end
  class Other
    def value() = LIMIT
    def qualified() = Config::LIMIT
  end
end
puts(Settings::Config.new().values())
puts(Settings::Other.new().value())
puts(Settings::Other.new().qualified())
OPTIONS["limit"] = 2
def option() = OPTIONS["limit"]
puts(option())
puts(LIMIT)
