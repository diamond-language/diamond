class Packet
  attr_reader label
  attr_reader values

  def initialize(label, *values)
    @label = label
    @values = values
  end
end

packet = Packet.new(*["telemetry", 1, 2, 3])
puts(packet.label())
puts(packet.values().join(","))

module Metrics
  def self.combine(prefix, *values)
    "#{prefix}:#{values.join(",")}"
  end
end
puts(Metrics.combine(*["m", 4, 5, 6]))

class BaseFactory
  def self.describe(prefix, *values)
    "#{prefix}:#{values.join(",")}"
  end
end

class ChildFactory < BaseFactory
end
puts(ChildFactory.describe(*["child", 7, 8]))
nil
