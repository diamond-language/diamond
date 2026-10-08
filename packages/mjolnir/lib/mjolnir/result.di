module Mjolnir

  # What Repo writes return: Ok(entity) or Err(changeset). Sealed, so a
  # `case` over a result is checked for exhaustiveness at compile time.
  sealed class Result
  end

  class Ok < Result
    def initialize(value)
      @value = value
    end
    def value() = @value
    def ok?() -> Bool = true
    def err?() -> Bool = false
  end

  class Err < Result
    def initialize(error)
      @error = error
    end
    def error() = @error
    def ok?() -> Bool = false
    def err?() -> Bool = true
  end

end
