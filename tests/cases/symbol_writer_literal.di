# :name= is a Symbol literal naming a writer; :a == :a still compares.
class Record
  attr_accessor name
end
record = Record.new()
record.public_send(:name=, "Ada")
symbol = :a
[record.respond_to?(:name=), record.name(), :name=, symbol==:a, :ready?]
