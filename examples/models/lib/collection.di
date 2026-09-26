# A searchable list of records. Finders are answered by method_missing:
# find_by_<field>(value) returns the first match or nil, where_<field>(value)
# every match. Anything else is still a NoMethodError.
require "./model"

class Collection
  include Enumerable

  def initialize(records: Array)
    @records = records
  end

  def each(callback)
    @records.each(callback)
    self
  end

  def length() -> Int = @records.length()

  def method_missing(name, args)
    text = "#{name}"
    found = Regexp.new("^(find_by|where)_([a-z_]+)$").match(text)
    if found == nil || args.length() != 1
      raise NoMethodError.new("undefined method '#{name}' for Collection")
    end
    [_, kind, field] = found
    matches = @records.select() do |record|
      record.respond_to?(to_sym(field)) && record.public_send(field) == args[0]
    end
    if kind == "find_by" then matches.first_or(nil) else Collection.new(matches) end
  end
end
