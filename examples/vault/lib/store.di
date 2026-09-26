# The vault file: JSON holding a bcrypt check of the password, the salt,
# the sealed entries, and an HMAC over the entries so an edited file is
# detected even for entries nobody reads.
require "./crypto"

class Vault
  attr_reader path: String

  def initialize(path: String, data: Hash, key: String)
    @path = path
    @data = data
    @key = key
  end

  def self.create(path: String, password: String, cost: Int, rounds: Int) -> Vault
    salt = SecureRandom.hex(16)
    data = {
      "version": 1,
      "check": BCrypt.hash(password, cost),
      "salt": salt,
      "rounds": rounds,
      "entries": {},
      "mac": "",
    }
    vault = Vault.new(path, data, derive_key(password, salt, rounds))
    vault.save()
    vault
  end

  def self.open(path: String, password: String) -> Vault
    file = File.open(path, "r")
    text = ""
    begin
      text = file.read()
    ensure
      file.close()
    end
    data = JSON.parse(text)
    unless data is Hash && data["version"] == 1
      raise VaultError.new("#{path} is not a version 1 vault")
    end
    unless BCrypt.verify(password, data["check"])
      raise VaultError.new("wrong password")
    end
    Vault.new(path, data, derive_key(password, data["salt"], data["rounds"]))
  end

  def names() -> Array = self.entries().keys().sort()

  def put(name: String, secret: String)
    self.entries()[name] = seal(@key, secret)
    self.save()
  end

  def get(name: String) -> String
    sealed = self.entries()[name]
    raise VaultError.new("no entry named #{name}") if sealed == nil
    secret = unseal(@key, sealed)
    raise VaultError.new("entry #{name} failed to decrypt") if secret == nil
    secret
  end

  def remove(name: String)
    if self.entries().delete(name) == nil
      raise VaultError.new("no entry named #{name}")
    end
    self.save()
  end

  # True when the entries still match the HMAC written with them.
  def intact?() -> Bool = HMAC.verify(self.canonical(), @key, @data["mac"])

  def save()
    @data["mac"] = HMAC.sha256(@key, self.canonical())
    # File.publish refuses to replace an existing file and there is no
    # File.rename, so this rewrites in place rather than atomically.
    file = File.open(@path, "w")
    begin
      file.write(JSON.stringify(@data))
    ensure
      file.close()
    end
  end

  private

  def entries() -> Hash = @data["entries"]

  # The entries in a fixed order, so the same contents always give the
  # same MAC whatever order the JSON object came back in.
  def canonical() -> String
    self.names().map() do |name| "#{name}=#{self.entries()[name]}" end.join("\n")
  end
end
