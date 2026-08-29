require "../../../arel/lib/arel"

module ActiveRecord

# Reusable validator building blocks -- ordinary functions returning
# ordinary functions, matching Repository's own `validator(attributes,
# exclude_id) -> Array[String]` shape exactly (see repository.di's own
# comment) so nothing about Repository changes to use these. There is
# no `validates :field, ...`-style declarative macro here, same as
# everywhere else in this package: a model wires these up explicitly,
# e.g.
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
#
# Every `check` takes `(attributes, exclude_id)` -- `exclude_id` is the
# id of the record being validated on an #update (nil on #create, see
# repository.di's own Repository#create/#update), threaded through so
# `uniqueness` below can exclude a record's own row from its own
# uniqueness check. Every validator except `uniqueness` ignores it.
class Validators
  def self.presence(field: String, message = nil)
    def check(attributes, exclude_id)
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
    def check(attributes, exclude_id)
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
    def check(attributes, exclude_id)
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
    def check(attributes, exclude_id)
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
    def check(attributes, exclude_id)
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

  # Needs a real DB round trip, which Repository's own validator never
  # otherwise would -- resolved without any other Repository change by
  # closing over `db` directly, exactly like any other captured value
  # (see migration.di's own note on what closures here can and can't
  # do). `table`/`visitor` follow Repository's own constructor shape.
  #
  # On #update, Repository passes the record's own id as `exclude_id`
  # (see repository.di's own Repository#update), which this excludes
  # from the uniqueness check via `id_column != exclude_id` -- so a
  # record whose unique field is unchanged no longer conflicts with
  # itself. `id_column` defaults to "id", matching Repository's own
  # default; pass the real column name if a table's primary key is
  # named differently.
  def self.uniqueness(db, table: Arel::Table, field: String, visitor = nil, message = nil, id_column: String = "id")
    def check(attributes, exclude_id)
      value = attributes[field]
      predicate = table.column(field).eq(value)
      unless exclude_id == nil
        predicate = predicate.and_also(table.column(id_column).not_eq(exclude_id))
      end
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
    def check(attributes, exclude_id)
      errors = []
      index = 0
      while index < validators.length()
        validator = validators[index]
        errors = errors.concat(validator(attributes, exclude_id))
        index += 1
      end
      errors
    end
    check
  end
end

end
