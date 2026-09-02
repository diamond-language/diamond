module ActiveRecord

  # Raised by Repository#update when optimistic locking is configured
  # (see Repository's lock_column) and the row's lock column no longer
  # matches the caller's expected_lock_version -- someone else updated (or
  # deleted) this row first. `id` is the row this update targeted.
  class StaleObjectError < StandardError
    attr_reader message: String
    attr_reader id
    def initialize(id)
      @id = id
      @message = "attempted to update a stale object (id=#{id})"
    end
  end

end
