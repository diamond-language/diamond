class Vault
  def initialize(code)
    @code = code
  end

  protected

  def code()
    @code
  end

  public

  def same_code?(other)
    self.code() == other.code()
  end
end

class DerivedVault < Vault
  def read_peer(other)
    other.code()
  end
end

class Intruder
  def read(vault)
    vault.code()
  end
end

left = Vault.new(7)
right = Vault.new(7)
derived = DerivedVault.new(9)
puts(left.same_code?(right))
puts(derived.read_peer(left))
puts(left.respond_to?(:code))

begin
  left.code()
rescue error: TypeError
  puts(error.message())
end

begin
  Intruder.new().read(left)
rescue error: TypeError
  puts(error.message())
end

class NamedVisibility
  def token()
    42
  end

  protected token

  def read(other)
    other.token()
  end
end

named = NamedVisibility.new()
puts(named.read(named))
nil
