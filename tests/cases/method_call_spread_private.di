class Vault
  private

  def secret(value) = value
end

Vault.new().secret(*[1])
