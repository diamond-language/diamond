class Vault
 attr_writer ready
 attr_predicate ready
 private ready?
 def reveal() = self.ready?()
end
vault=Vault.new()
vault.ready=(true)
vault.reveal()
