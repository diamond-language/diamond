# A searchable list of records. Finders are answered by method_missing:
# find_by_<field>(value) returns the first match or nil, where_<field>(value)
# every match. Anything else is still a NoMethodError.
require "./model"

class Collection
  include Enumerable

  def initialize(records: Array)
    @records = records
  end

  # The one method Enumerable needs (map, select, count... come from it).
  def each(callback)
    @records.each(callback)
    self
  end

  def length() -> Int = @records.length()

  # Called when a method that does not exist is invoked on a Collection;
  # `name` is the method name and `args` the arguments. This is how
  # find_by_title("Dune") works without anyone defining find_by_title.
  def method_missing(name, args)
    text = "#{name}"

    # Only the two patterns with exactly one argument are handled; anything
    # else is reported as the usual NoMethodError.
    found = Regexp.new("^(find_by|where)_([a-z_]+)$").match(text)
    if found == nil || args.length() != 1
      raise NoMethodError.new("undefined method '#{name}' for Collection")
    end
    [_, kind, field] = found

    # Keep records that actually HAVE that field (respond_to?, so a typo in
    # the field name matches nothing instead of raising) and whose value
    # equals the argument. `public_send` calls the method by name.
    matches = @records.select() do |record|
      record.respond_to?(to_sym(field)) && record.public_send(field) == args[0]
    end

    # find_by gives the first match or nil; where gives them all.
    if kind == "find_by" then matches.first_or(nil) else Collection.new(matches) end
  end
end
