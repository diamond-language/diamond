# Cookie parsing/serialization, base64url/hex helpers, and signed/
# encrypted cookie support -- built on top of Cipher.encrypt/.decrypt
# and HMAC.sha256/.verify (src/vm.c), the same OpenSSL-backed native
# builtins BCrypt/Digest/SecureRandom already are. See README.md for
# the full story and packages/rack for the middleware contract this is
# meant to plug into.
#
# One file per class/concern, matching packages/active_record's own
# split (its own lib/active_record.di has the full rationale) --
# possible since Diamond gained cross-file forward-referencing for
# classes. Order here follows each file's own dependency on the one
# before it (codec's free functions first, then the classes built on
# them) rather than being strictly required by the compiler.
require "./cookies/codec"
require "./cookies/signed_cookies"
require "./cookies/encrypted_cookies"
require "./cookies/cookie_session"
require "./cookies/csrf"
