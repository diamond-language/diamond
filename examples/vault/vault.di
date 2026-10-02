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

# Opens with the password from the environment (not a command-line argument,
# which would show up in `ps` and shell history). `run` has already checked
# it is set.
def open_vault(path: String) -> Vault = Vault.open(path, ENV["VAULT_PASSWORD"])

def run(args: Array[String]) -> Int
  return usage() if args.length() < 2

  # The password comes from the environment only.
  password = ENV["VAULT_PASSWORD"]
  if password == nil || password.empty?()
    warn("vault: set VAULT_PASSWORD")
    return 64
  end

  # Split the arguments: the file, the command word, and any remaining
  # words, which the patterns below match by shape.
  [path, command, *rest] = args

  case [command, *rest]
  # init: the two cost settings can be lowered through the environment (the
  # tests do, to run quickly); the defaults are deliberately slow, since slow
  # is the point of key stretching.
  when ["init"]
    cost = ENV.fetch("VAULT_BCRYPT_COST", "12").to_i()
    rounds = ENV.fetch("VAULT_KDF_ROUNDS", "100000").to_i()
    Vault.create(path, password, cost, rounds)
    puts("created #{path}")
  # add: the secret is read from stdin so it never appears on the command line.
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
  # verify: check the whole-file MAC.
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

# A vault problem or a corrupt file is exit 1; an unreadable file is 66.
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
