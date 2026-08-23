require "../../../arel/lib/arel"

module ActiveRecord

# Reusable validator building blocks -- ordinary functions returning
# ordinary functions, matching Repository's own `validator(attributes) ->
# Array[String]` shape exactly (see repository.di's own comment) so
# nothing about Repository changes to use these. There is no `validates
# :field, ...`-style declarative macro here, same as everywhere else in
# this package: a model wires these up explicitly, e.g.
#
#   validator = ActiveRecord::Validators.combine([
#     ActiveRecord::Validators.presence("name"),
#     ActiveRecord::Validators.length("name", maximum: 100),
#     ActiveRecord::Validators.format("email", Regexp.new("^[^@]+@[^@]+$")),
#   ])
#   ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id",
#     nil, validator)
#
# Each `self.xxx` below returns a nested `def` closing over its own
# arguments (field name, options, message) -- the same closure-returning
# pattern packages/rack's rack_terminal_wrap and packages/gremlin's
# spawn_connection already use, confirmed to work identically for a
# nested def inside a `def self.x` singleton method (unlike calling
# `self.foo()` from inside such a closure, which does not work -- see
# migration.di's own comment for why; none of the closures below need to
# call back into Validators itself, so that pitfall doesn't apply here).
class Validators
  def self.presence(field: String, message = nil)
    def check(attributes)
      value = attributes[field]
      if value == nil || value == ""
        text = if message == nil then "#{field} is required" else message end
        [text]
      else
        []
      end
    end
    check
  end

  # `minimum`/`maximum` apply to `value.length()` -- String, Array, and
  # Hash all support #length(), so this works for any of them; `nil`
  # is not itself a length failure (that's #presence's job) -- absent
  # values pass #length silently, same "one check, one concern" split
  # real ActiveRecord uses.
  def self.length(field: String, minimum = nil, maximum = nil, message = nil)
    def check(attributes)
      value = attributes[field]
      if value == nil
        return []
      end
      len = value.length()
      if minimum != nil && len < minimum
        text = if message == nil then "#{field} is too short (minimum #{minimum})" else message end
        return [text]
      end
      if maximum != nil && len > maximum
        text = if message == nil then "#{field} is too long (maximum #{maximum})" else message end
        return [text]
      end
      []
    end
    check
  end

  # Diamond has no is_a?/class() runtime type check (confirmed directly --
  # neither exists), so "numeric" is duck-typed instead: `value + 0`
  # raises TypeError for anything that isn't Int/Float (a String, an
  # Array, nil, ...), caught here and turned into a validation failure
  # rather than propagating. nil fails by the same path (`nil + 0` also
  # raises TypeError) -- absence is #presence's job, not this one's,
  # matching real ActiveRecord's own default (allow_nil: false).
  def self.numericality(field: String, message = nil)
    def check(attributes)
      value = attributes[field]
      valid = true
      begin
        value + 0
      rescue error: TypeError
        valid = false
      end
      if valid
        []
      else
        text = if message == nil then "#{field} must be numeric" else message end
        [text]
      end
    end
    check
  end

  # `pattern` is an ordinary Regexp; matched with #match? (no capture
  # allocation needed just to check a match). nil fails -- there is
  # nothing for a pattern to match against.
  def self.format(field: String, pattern, message = nil)
    def check(attributes)
      value = attributes[field]
      if value == nil || !pattern.match?(value)
        text = if message == nil then "#{field} is invalid" else message end
        [text]
      else
        []
      end
    end
    check
  end

  def self.inclusion(field: String, values: Array, message = nil)
    def check(attributes)
      value = attributes[field]
      if values.include?(value)
        []
      else
        text = if message == nil then "#{field} is not included in the list" else message end
        [text]
      end
    end
    check
  end

  # Needs a real DB round trip, which Repository's own validator(attributes)
  # never receives (confirmed: called as @validator(attributes), no db) --
  # resolved without any Repository change by closing over `db` directly,
  # exactly like any other captured value (see migration.di's own note on
  # what closures here can and can't do). `table`/`visitor` follow
  # Repository's own constructor shape.
  #
  # Only correct for #create. Repository's validator has no access to the
  # row's own id (or whether this is a #create or #update at all), so on
  # #update this also flags a record whose unique field is unchanged as
  # conflicting with itself -- a real Repository-level constraint, not
  # something this works around. Document this at the call site if a
  # model actually uses #update with a uniqueness-validated field.
  def self.uniqueness(db, table: Arel::Table, field: String, visitor = nil, message = nil)
    def check(attributes)
      value = attributes[field]
      predicate = table.column(field).eq(value)
      rows = Arel.from(table).where(predicate).take(1).to_a(db, visitor)
      if rows.length() > 0
        text = if message == nil then "#{field} has already been taken" else message end
        [text]
      else
        []
      end
    end
    check
  end

  # Concatenates every sub-validator's own error Array into one -- the
  # combinator standing in for what multiple `validates` calls would do
  # declaratively in real ActiveRecord.
  def self.combine(validators: Array)
    def check(attributes)
      errors = []
      index = 0
      while index < validators.length()
        validator = validators[index]
        errors = errors.concat(validator(attributes))
        index += 1
      end
      errors
    end
    check
  end
end

end
