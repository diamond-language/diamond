module GraphQL

# Raised by a resolver to produce a specific, client-visible field
# error message (rather than a generic "internal error" for whatever
# StandardError the resolver happened to raise) -- the one error class
# this package expects application code to reach for directly. Caught
# by the executor at the same point any other resolver exception is
# caught; distinguished only so a resolver can choose its own message
# with intent instead of it being an implementation-detail leak.
class ExecutionError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

# Raised for a request-level problem the executor can't attribute to
# one field -- an unparseable document, an unknown operation name, a
# missing required variable, a variable/argument that doesn't coerce
# against its declared type. Schema#execute catches this at the top
# and turns it into the request-level `{"errors": [...]}` shape (no
# `"data"` key at all, per spec -- distinct from a field-level error,
# which still returns partial `"data"` alongside its own error entry).
class RequestError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

end
