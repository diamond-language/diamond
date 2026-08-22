module ActiveRecord

# Neither Arel nor the database drivers expose a transaction API
# themselves (BEGIN/COMMIT/ROLLBACK are ordinary SQL statements a caller
# runs through the same #execute(sql) every write in this package already
# uses -- see docs/io.md). This is that one missing piece: run `callback`,
# commit on a normal return, roll back and re-raise on any exception.
class Transaction
  def self.run(db, callback: Callable[0])
    db.execute("BEGIN")
    begin
      result = callback()
      db.execute("COMMIT")
      result
    rescue error: StandardError
      db.execute("ROLLBACK")
      raise error
    end
  end

  # For use *inside* an already-running #run (or any transaction the
  # caller already opened itself) -- databases don't support nesting a
  # real BEGIN/COMMIT, so this runs `callback` inside a named SAVEPOINT
  # instead, releasing it on a normal return or rolling back to it (not
  # the whole outer transaction) on any exception. Verified directly
  # that SAVEPOINT/RELEASE SAVEPOINT/ROLLBACK TO SAVEPOINT are identical
  # syntax and semantics across all three supported dialects (SQLite,
  # PostgreSQL, MariaDB), so this needs no visitor/dialect parameter the
  # way Repository does.
  #
  # Deliberately a separate, explicitly-called method rather than #run
  # auto-detecting nesting -- there is no ambient "am I already inside a
  # transaction on this connection" state exposed to Diamond code to
  # detect that with, and guessing from some other signal would be
  # exactly the kind of implicit magic this package avoids everywhere
  # else. The caller already knows whether it's nested; say so.
  #
  #   ActiveRecord::Transaction.run(db) do
  #     repository.create(db, {"name": "Ada"})
  #     ActiveRecord::Transaction.run_nested(db, "before_grace") do
  #       repository.create(db, {"name": "Grace"})
  #       raise RuntimeError.new("oops")
  #     end
  #   end
  #   # => "Ada" is committed, "Grace" is not -- the outer transaction
  #   # itself is untouched by the inner rollback.
  def self.run_nested(db, savepoint_name: String, callback: Callable[0])
    self.validate_savepoint_name(savepoint_name)
    db.execute("SAVEPOINT #{savepoint_name}")
    begin
      result = callback()
      db.execute("RELEASE SAVEPOINT #{savepoint_name}")
      result
    rescue error: StandardError
      db.execute("ROLLBACK TO SAVEPOINT #{savepoint_name}")
      raise error
    end
  end

  # SAVEPOINT/RELEASE/ROLLBACK TO take a bare identifier, not a bind
  # parameter -- there is no `?` placeholder form for a savepoint name in
  # any of the three dialects this checked directly, the same reason a
  # table or column name can't be bound either. Restricting it to
  # ASCII letters/digits/underscore, not starting with a digit, before
  # ever interpolating it into SQL text closes off that injection
  # surface entirely, the same spirit as Arel's own identifier quoting
  # (though a plain reject-anything-else check here, not quote-and-escape,
  # since a savepoint name has no legitimate reason to contain anything
  # else in the first place).
  def self.validate_savepoint_name(name: String)
    if name.length() == 0
      raise ArgumentError.new("savepoint name cannot be empty")
    end
    # Diamond's `>=`/`<=` aren't defined for String -- compared by
    # ordinal codepoint (#ord) instead, not by the character itself.
    characters = name.chars()
    first_code = characters[0].ord()
    if first_code >= 48 && first_code <= 57
      raise ArgumentError.new("savepoint name cannot start with a digit: #{name}")
    end
    index = 0
    while index < characters.length()
      code = characters[index].ord()
      valid = (code >= 97 && code <= 122) || (code >= 65 && code <= 90) ||
        (code >= 48 && code <= 57) || code == 95
      unless valid
        raise ArgumentError.new(
          "savepoint name must contain only letters, digits, and underscores: #{name}")
      end
      index += 1
    end
  end
end

end
