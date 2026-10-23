# The vault file: JSON holding a bcrypt check of the password, the salt,
# the sealed entries, and an HMAC over the entries so an edited file is
# detected even for entries nobody reads.
require "./crypto"

# An open vault: its file path, the parsed JSON, and the derived key.
# Entries are encrypted individually, so listing names needs no decryption.
class Vault
  attr_reader path: String

  def initialize(path: String, data: Hash, key: String)
    @path = path
    @data = data
    @key = key
  end

  # A new empty vault. The password itself is never stored: `check` is a
  # bcrypt hash (to verify the password on open), and the encryption key is
  # derived separately from the password and salt each time.
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

  # Opens an existing vault, checking the file format and the password
  # BEFORE deriving the (slow) key, so a wrong password fails fast.
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

  # Entry names, sorted (also the order the MAC is computed in).
  def names() -> Array = self.entries().keys().sort()

  # Encrypts and stores a secret, replacing any entry of that name, and saves
  # the file immediately.
  def put(name: String, secret: String)
    self.entries()[name] = seal(@key, secret)
    self.save()
  end

  # Decrypts one entry. A failure to decrypt (rather than a missing entry)
  # means the entry was tampered with or the key is wrong.
  def get(name: String) -> String
    sealed = self.entries()[name]
    raise VaultError.new("no entry named #{name}") if sealed == nil
    secret = unseal(@key, sealed)
    raise VaultError.new("entry #{name} failed to decrypt") if secret == nil
    secret
  end

  # Deletes an entry; `delete` returns nil if there was none.
  def remove(name: String)
    if self.entries().delete(name) == nil
      raise VaultError.new("no entry named #{name}")
    end
    self.save()
  end

  # True when the entries still match the HMAC written with them. GCM already
  # protects each entry against tampering; this catches changes GCM cannot,
  # such as an entry being deleted or swapped for an older copy of itself.
  def intact?() -> Bool = HMAC.verify(self.canonical(), @key, @data["mac"])

  # Writes a temporary file beside the vault, then renames it over the
  # vault, so a crash mid-save leaves the old vault intact rather than a
  # half-written one.
  def save()
    # Refresh the MAC to cover the current entries, then write via a temp
    # file with a random suffix.
    @data["mac"] = HMAC.sha256(@key, self.canonical())
    temporary = "#{@path}.tmp-#{SecureRandom.hex(4)}"
    file = File.open(temporary, "w")
    begin
      file.write(JSON.stringify(@data))
    ensure
      file.close()
    end

    # Renaming is atomic on the same filesystem: readers see either the old
    # file or the new one, never a partial write.
    File.rename(temporary, @path)
  end

  private

  # The entries Hash inside the file data.
  def entries() -> Hash = @data["entries"]

  # The entries in a fixed order, so the same contents always give the
  # same MAC whatever order the JSON object came back in.
  def canonical() -> String
    self.names().map() do |name| "#{name}=#{self.entries()[name]}" end.join("\n")
  end
end
