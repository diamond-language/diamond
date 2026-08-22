module ActiveRecord

# Raised by Repository#create/#update when a configured
# validator reports at least one failure, before any SQL runs. `errors` is
# the Array of message Strings the validator returned; `message` joins
# them for the common case of just wanting one string to display or log.
class ValidationError < StandardError
  attr_reader message: String
  attr_reader errors: Array
  def initialize(errors: Array)
    @errors = errors
    @message = errors.join(", ")
  end
end

end
