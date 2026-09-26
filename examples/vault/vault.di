# vault: an encrypted secrets file.
#
#   VAULT_PASSWORD=... diamond vault.di FILE init
#   VAULT_PASSWORD=... diamond vault.di FILE add NAME     (secret on stdin)
#   VAULT_PASSWORD=... diamond vault.di FILE get NAME
#   VAULT_PASSWORD=... diamond vault.di FILE list | rm NAME | verify
#
# Exit status: 0 ok, 1 an error from the vault (wrong password, unknown
# entry, tampering), 64 usage, 66 file can't be read.
require "./lib/store"

def usage() -> Int
  warn("usage: vault FILE (init | add NAME | get NAME | list | rm NAME | verify)")
  64
end

def open_vault(path: String) -> Vault = Vault.open(path, ENV["VAULT_PASSWORD"])

def run(args: Array[String]) -> Int
  return usage() if args.length() < 2
  password = ENV["VAULT_PASSWORD"]
  if password == nil || password.empty?()
    warn("vault: set VAULT_PASSWORD")
    return 64
  end
  [path, command, *rest] = args
  case [command, *rest]
  when ["init"]
    cost = ENV.fetch("VAULT_BCRYPT_COST", "12").to_i()
    rounds = ENV.fetch("VAULT_KDF_ROUNDS", "100000").to_i()
    Vault.create(path, password, cost, rounds)
    puts("created #{path}")
  when ["add", name]
    secret = gets()
    return usage() if secret == nil
    open_vault(path).put(name, secret)
    puts("stored #{name}")
  when ["get", name]
    puts(open_vault(path).get(name))
  when ["list"]
    open_vault(path).names().each() do |name| puts(name) end
  when ["rm", name]
    open_vault(path).remove(name)
    puts("removed #{name}")
  when ["verify"]
    unless open_vault(path).intact?()
      warn("vault: #{path} has been modified outside vault")
      return 1
    end
    puts("ok")
  else
    return usage()
  end
  0
end

def main() -> Int
  begin
    run(ARGV)
  rescue error: VaultError | JSONError
    warn("vault: #{error.message()}")
    1
  rescue error: IOError
    warn("vault: #{error.message()}")
    66
  end
end

exit(main())
