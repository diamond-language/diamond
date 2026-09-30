LIMIT = 10
VALUES = [1]
def global_limit() = LIMIT
module Settings
  EXTRA = 3
  class Config
    LIMIT = 20
    def limit() = LIMIT + EXTRA
    def self.limit() = LIMIT
    def self.global_values() = VALUES
  end
end
puts(global_limit())
puts(Settings::Config::LIMIT)
puts(Settings::Config.new().limit())
puts(Settings::Config.limit())
VALUES.push(2)
puts(Settings::Config.global_values())
