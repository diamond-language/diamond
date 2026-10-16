# Mutates a compiled-program cache, re-signs it, and runs the program against it.
#
#   diamond forge_cache.di DIAMOND PROGRAM.di ROUNDS
#
# Run from the directory holding PROGRAM.di and the PROGRAM.dic its first run produced. The
# cache checksum only catches accidents, so each round corrupts a few bytes and then recomputes
# the checksum, which leaves the structural and bytecode validation as the only thing standing
# between the mutant and the VM. A mutant may be rejected (and recompiled), run to a normal
# exit, or run on forever; none may kill the process with a signal. Prints the number of
# mutants that did.
#
# The checksum is the cache format's body checksum (src/compiled_prelude.c). The header is the
# 8-byte magic, eleven 4-byte fingerprint fields, the 32-byte source hash, then the 8-byte
# little-endian checksum over everything after it. It is 64-bit wrapping arithmetic, which is
# why this relies on arbitrary-precision `&`. A change to the header layout moves these
# offsets; the program checks the unmodified cache against its own stored checksum first, so
# a stale offset fails loudly instead of quietly testing nothing.

def body_checksum(bytes: Array, from: Int) -> Int
  size = bytes.length() - from
  hash = (0x9e3779b97f4a7c15 ^ size) & 0xffffffffffffffff
  index = from
  while index + 8 <= bytes.length()
    word = bytes[index] | (bytes[index + 1] << 8) | (bytes[index + 2] << 16) |
      (bytes[index + 3] << 24) | (bytes[index + 4] << 32) | (bytes[index + 5] << 40) |
      (bytes[index + 6] << 48) | (bytes[index + 7] << 56)
    hash = ((hash ^ word) * 0xff51afd7ed558ccd) & 0xffffffffffffffff
    hash = hash ^ (hash >> 32)
    index += 8
  end
  tail = 0
  shift = 0
  while index < bytes.length()
    tail = tail | (bytes[index] << shift)
    shift += 8
    index += 1
  end
  hash = ((hash ^ tail) * 0xff51afd7ed558ccd) & 0xffffffffffffffff
  hash ^ (hash >> 29)
end

# A small LCG: every product stays below 2^63, so it is plain Int arithmetic.
def next_random(state: Int) -> Int
  (state * 1103515245 + 12345) % 2147483648
end

diamond = ARGV[0]
program = ARGV[1]
rounds = ARGV[2].to_i()
cache_path = program[0, program.length() - 3] + ".dic"
base = File.read(cache_path).bytes()

checksum_at = 8 + 11 * 4 + 32
body_at = checksum_at + 8

stored = 0
8.times() do |k|
  stored = stored | (base[checksum_at + k] << (8 * k))
end
if body_checksum(base, body_at) != stored
  puts("forge_cache: the cache header layout no longer matches this program")
  exit(2)
end

state = 20261004
crashes = 0
limit = base.length() < 20000 ? base.length() : 20000
rounds.times() do |round|
  mutant = base.dup()
  state = next_random(state)
  edits = [1, 1, 2, 4][state % 4]
  edits.times() do |edit|
    state = next_random(state)
    at = 100 + state % (limit - 100)
    state = next_random(state)
    if state % 2 == 0
      state = next_random(state)
      mutant[at] = state % 256
    else
      state = next_random(state)
      mutant[at] = mutant[at] ^ (1 << (state % 8))
    end
  end

  sum = body_checksum(mutant, body_at)
  8.times() do |k|
    mutant[checksum_at + k] = (sum >> (8 * k)) & 255
  end
  File.write(cache_path, mutant.map() do |byte| byte.chr() end.join())

  handle = Process.spawn([diamond, program])
  waited = 0
  while handle.running?() && waited < 5000
    IO.poll([], [], 20)
    waited += 20
  end
  if handle.running?()
    # A mutant may legitimately loop forever; only an unprovoked signal is a failure.
    handle.kill()
    handle.wait()
  elsif handle.wait() >= 128
    crashes += 1
  end
end

puts(crashes)
exit(crashes == 0 ? 0 : 1)
