class Greeter
  def greet(name, punctuation = "!")
    "hello #{name}#{punctuation}"
  end

  def collect(prefix, *values)
    prefix + values.join("-")
  end

  protected

  def family_secret() = "family"

  private

  def secret() = "secret"

  public

  def try_nonpublic(other)
    begin
      other.public_send(:family_secret)
    rescue error: TypeError
      error.message()
    end
  end
end

class FriendlyGreeter < Greeter
  def greet(name, punctuation = "!")
    "hi #{name}#{punctuation}"
  end
end

class DynamicReceiver
  def method_missing(name, arguments)
    "missing #{name}: #{arguments.join(",")}"
  end
end

class CustomSender
  def public_send(name) = "custom #{name}"
end

greeter = FriendlyGreeter.new()
puts(greeter.public_send(:greet, "Ada"))
puts(greeter.public_send("greet", "Grace", "?"))
puts(greeter.public_send(*[:collect, "values=", 1, 2, 3]))
puts([1, 2, 3].public_send(:length))
puts("diamond".public_send(:slice, 0, 3))
puts(DynamicReceiver.new().public_send(:unknown, "a", "b"))
puts(CustomSender.new().public_send(:answer))
puts(greeter.try_nonpublic(greeter))

begin
  greeter.public_send(:secret)
rescue error: TypeError
  puts(error.message())
end

begin
  greeter.public_send(42)
rescue error: TypeError
  puts(error.message())
end

nil
