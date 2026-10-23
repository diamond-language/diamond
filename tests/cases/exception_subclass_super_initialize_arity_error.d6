class ValidationError < StandardError
  def initialize(a, b, c)
    super(a, b, c)
  end
end

begin
  ValidationError.new(1, 2, 3)
rescue error: ArgumentError
  "caught"
end
