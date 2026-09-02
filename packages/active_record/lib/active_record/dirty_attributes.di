module ActiveRecord

  # Explicit change tracking for a plain attributes Hash -- there is no
  # model base class here to instrument arbitrary setters on (mapper builds
  # whatever class the caller wants, opaque to this package, the same
  # reason validator/before_save/after_save are ordinary functions rather
  # than a DSL). Wrap a loaded row (or any Hash of known attribute values),
  # mutate it through `[]=`, then ask #changed?/#changes before deciding to
  # call Repository#update -- #changes returns exactly the Hash #update
  # already expects, with no repository integration needed:
  #
  #   row = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
  #   dirty = DirtyAttributes.new(row)
  #   dirty["country"] = "England"
  #   repository.update(db, row["id"], dirty.changes()) if dirty.changed?()
  #
  # `[]`/`[]=` overloading (docs/syntax.md's "Operator overloading"
  # section) -- ordinary bracket syntax, not a `#get`/`#set` pair. Comparison
  # is `==`, so it's value equality for the ordinary Int/Float/String/Bool/Nil
  # attribute values this is meant for, but identity equality if an attribute
  # value is itself an Array/Hash -- the same distinction Diamond's own `==`
  # already draws everywhere else.
  class DirtyAttributes
    def initialize(original: Hash)
      @original = original
      @current = {}
      keys = original.keys()
      index = 0
      while index < keys.length()
        key = keys[index]
        @current[key] = original[key]
        index += 1
      end
    end

    def [](key) = @current[key]

    def []=(key, value)
      @current[key] = value
    end

    def attribute_changed?(key) -> Bool = @original[key] != @current[key]

    def changed_keys() -> Array
      keys = @current.keys()
      result = []
      index = 0
      while index < keys.length()
        key = keys[index]
        if self.attribute_changed?(key)
          result.push(key)
        end
        index += 1
      end
      result
    end

    def changed?() -> Bool = self.changed_keys().length() > 0

    def changes() -> Hash
      result = {}
      changed = self.changed_keys()
      index = 0
      while index < changed.length()
        key = changed[index]
        result[key] = @current[key]
        index += 1
      end
      result
    end

    def to_h() -> Hash
      copy = {}
      keys = @current.keys()
      index = 0
      while index < keys.length()
        key = keys[index]
        copy[key] = @current[key]
        index += 1
      end
      copy
    end
  end

end
