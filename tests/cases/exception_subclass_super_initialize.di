class ValidationError < StandardError
  def initialize(message, field)
    super(message)
    @field = field
  end

  def field()
    @field
  end
end

begin
  raise ValidationError.new("bad input", "email")
rescue error: ValidationError
  error.message() + ":" + error.field()
end
