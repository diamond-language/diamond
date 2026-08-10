class Vault
  def check(x)
    if x > 10
      return 1
    end
    0
  end
end
Vault.new().check(20) + Vault.new().check(5)
