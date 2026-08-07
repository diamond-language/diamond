class Failure
end

class SpecificFailure < Failure
end

class OtherFailure
end

primitive = begin
  begin
    raise "text"
  rescue wrong: Int
    0
  end
rescue message: Int | String
  message + "!"
end

nominal = begin
  raise SpecificFailure.new()
rescue error: OtherFailure | Failure
  41
end

if primitive == "text!"
  nominal + 1
else
  0
end
