module Mjolnir

  # Raised for programmer mistakes (a typo'd field name), never for bad
  # user input -- that is what a Changeset's errors are for.
  class UnknownFieldError < StandardError
    def initialize(table: String, field)
      super("unknown field #{field} on #{table}")
    end
  end

  # Raised by Repo#insert! / #update! when the changeset is invalid.
  class InvalidChangesetError < StandardError
    def initialize(changeset: Changeset)
      @changeset = changeset
      super("invalid changeset for #{changeset.schema().table_name()}: #{changeset.errors()}")
    end
    def changeset() -> Changeset = @changeset
  end

  # An update or delete matched no row: it was removed since it was loaded.
  class StaleEntryError < StandardError
    def initialize(table: String, id)
      super("no #{table} row with id #{id}")
    end
  end

end
