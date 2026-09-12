#ifndef DIAMOND_VM_H
#define DIAMOND_VM_H

#include "value.h"
#include "object.h"

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

enum {
    /* Absolute 16-bit jump-target boundary. Function bytecode/source maps
     * grow dynamically up to this limit instead of reserving the entire
     * range in every DiamondFunction. */
    DIAMOND_MAX_CODE = UINT16_MAX,
    /* CONSTANT already carries a 16-bit operand. Storage grows dynamically. */
    DIAMOND_MAX_CONSTANTS = UINT16_MAX,
    /* Function indices are 16-bit bytecode operands. Function records and the
     * pointer table that indexes them grow dynamically; this constant is the
     * wire-format boundary, not a preallocated storage size. */
    DIAMOND_MAX_FUNCTIONS = UINT16_MAX,
    DIAMOND_MAX_FUNCTION_NAME = 64,
    DIAMOND_MAX_STRING_CONSTANTS = UINT16_MAX,
    /* Compile-time literal-constant buffer size only (DiamondStringConstant,
     * the source-text constant pool) -- unrelated to the runtime String
     * object's own dynamic allocation (DiamondString), which has no such
     * cap. Was 255; raised after repeatedly hitting it in practice on
     * genuinely ordinary literals (a multi-line SQL CREATE TABLE
     * statement, a GraphQL query) that had to be manually split into an
     * Array + .join() to work around it -- see skindicate.dia's own
     * setup_db.di and winamp_api.di for real examples this fix
     * eliminates the workaround for. */
    DIAMOND_MAX_STRING_LENGTH = 4095,
    DIAMOND_MAX_CLASSES = 180,
    DIAMOND_MAX_INTERFACES = 32,
    DIAMOND_MAX_MODULES = 32,
    DIAMOND_MAX_TYPE_SETS = UINT16_MAX,
    DIAMOND_NO_TYPE_SET = UINT16_MAX,
    DIAMOND_MAX_UNION_TYPES = 8,
    /* The real ceiling: every wire-format argument/parameter count
     * (DIAMOND_OP_CALL's call_argument_count, INVOKE/NEW/SUPER's argc,
     * DiamondFunction/DiamondMethod's arity/required_arity, DiamondMethod's
     * bound_value_count) is a plain uint8_t operand, so 255 is already what
     * the bytecode itself can carry -- this was previously an arbitrary
     * compile-time/runtime 16 short of that, not a real representation
     * limit. Used both for compile-time parameter/argument-list parsing
     * buffers (src/compiler.c) and for the runtime argument-marshaling
     * buffers this same call ultimately flows through (src/vm.c) -- one
     * shared cap so the two sides can't drift out of sync with each other. */
    DIAMOND_MAX_ARGUMENTS = 255,
    /* Distinct from DIAMOND_MAX_ARGUMENTS above on purpose: a *call*
     * argument list only ever needs register-range bookkeeping (cheap to
     * widen fully, see that constant's own comment), but a *declared*
     * parameter list's name/type-set storage is baked directly into every
     * DiamondFunction/DiamondInterfaceMethod struct -- and DiamondFunction
     * is already documented (its own scope_locals field, below) as
     * "already ~152KB, and there can be up to DIAMOND_MAX_FUNCTIONS of
     * them" -- so this stays a deliberately modest fixed bump (16 -> 32)
     * rather than matching DIAMOND_MAX_ARGUMENTS's full 255 and adding
     * ~15KB to every function regardless of how many parameters it
     * actually declares. A variadic function's *effective* argument count
     * is unaffected by this cap either way -- see DIAMOND_MAX_ARGUMENTS. */
    DIAMOND_MAX_DECLARED_PARAMETERS = 32,
    DIAMOND_MAX_METHODS = 256,
    DIAMOND_MAX_FIELDS = 64,
    DIAMOND_MAX_NAMESPACE_CONSTANTS = 128,
    /* Compile-time ceiling and READ_SHORT-encoded bytecode operand range
     * (big-endian 16-bit, matching the JUMP-target/function-index
     * convention) -- not a fixed per-call runtime array size. run_chunk
     * (src/vm.c) sizes its actual `registers` array as a VLA to each
     * function's own live_register_count instead, so a typical small
     * function's call frame stays cheap regardless of how high this cap
     * is; only a function that actually needs this many registers pays
     * for it, in that one call. See docs/roadmap.md. */
    DIAMOND_REGISTER_COUNT = 4096,
    /* Per DiamondFunction, not per scope -- a function's own top-level
     * locals/parameters *and* every `rescue`-bound name across every
     * rescue clause nested in it all accumulate into the same flat
     * list (DiamondFunction.scope_locals below), so this needs
     * headroom beyond the ordinary 64-local-at-a-time compile-time cap
     * (DIAMOND_MAX_LOCALS, just below) for a function with several
     * rescue clauses. Kept deliberately smaller than that cap would
     * suggest: DiamondFunction is already ~152KB and there can be up
     * to DIAMOND_MAX_FUNCTIONS of them (see that constant's own
     * comment) -- every byte added here multiplies by both. */
    DIAMOND_MAX_SCOPE_LOCALS = 32,
    /* Position-sensitive local type changes retained only for LSP receiver
     * resolution. A pathological function may lose late facts, never fail
     * compilation. */
    DIAMOND_MAX_SCOPE_TYPE_FACTS = 128,
    /* How many locals (parameters plus ordinary declarations) can be in
     * scope at once at compile time, src/compiler.c's Compiler.locals[].
     * Lives here rather than staying compiler.c-local: DIAMOND_OP_DEBUGGER
     * (below) bakes each in-scope local's (name, register) pair into its
     * own bytecode operand data at debugger()/breakpoint() call sites --
     * see parse_debugger_call in compiler.c and the DEBUGGER case in
     * run_chunk -- so both sides need to agree on the same fixed-size
     * table cap. */
    DIAMOND_MAX_LOCALS = 64,
};

typedef enum DiamondOpCode : uint8_t {
    DIAMOND_OP_CONSTANT,
    DIAMOND_OP_STRING,
    DIAMOND_OP_SYMBOL,
    DIAMOND_OP_NIL,
    DIAMOND_OP_BOOL,
    DIAMOND_OP_MOVE,
    DIAMOND_OP_ADD,
    DIAMOND_OP_ADD_INT,
    DIAMOND_OP_SUBTRACT,
    DIAMOND_OP_MULTIPLY,
    DIAMOND_OP_DIVIDE,
    DIAMOND_OP_SUBTRACT_INT,
    DIAMOND_OP_MULTIPLY_INT,
    DIAMOND_OP_DIVIDE_INT,
    DIAMOND_OP_LESS,
    DIAMOND_OP_LESS_EQUAL,
    DIAMOND_OP_GREATER,
    DIAMOND_OP_GREATER_EQUAL,
    DIAMOND_OP_NEGATE,
    DIAMOND_OP_EQUAL,
    DIAMOND_OP_NOT_EQUAL,
    DIAMOND_OP_EQUAL_INT,
    DIAMOND_OP_NOT_EQUAL_INT,
    DIAMOND_OP_LESS_INT,
    DIAMOND_OP_LESS_EQUAL_INT,
    DIAMOND_OP_GREATER_INT,
    DIAMOND_OP_GREATER_EQUAL_INT,
    DIAMOND_OP_JUMP,
    DIAMOND_OP_JUMP_IF_FALSE,
    DIAMOND_OP_CALL,
    DIAMOND_OP_CALL_TYPED,
    DIAMOND_OP_CLOSURE,
    DIAMOND_OP_CALL_CLOSURE,
    DIAMOND_OP_GET_CAPTURE,
    DIAMOND_OP_GET_CAPTURE_CELL,
    DIAMOND_OP_SET_CAPTURE,
    DIAMOND_OP_BOX_LOCAL,
    DIAMOND_OP_GET_CELL,
    DIAMOND_OP_SET_CELL,
    DIAMOND_OP_NEW,
    DIAMOND_OP_INVOKE,
    DIAMOND_OP_INVOKE_MONO,
    DIAMOND_OP_INVOKE_TYPED,
    DIAMOND_OP_SUPER,
    DIAMOND_OP_GET_IVAR,
    DIAMOND_OP_SET_IVAR,
    DIAMOND_OP_GET_IVAR_NAME,
    DIAMOND_OP_SET_IVAR_NAME,
    DIAMOND_OP_GET_NAMESPACE_CONSTANT,
    DIAMOND_OP_SET_NAMESPACE_CONSTANT,
    DIAMOND_OP_CHECK_TYPE,
    DIAMOND_OP_ARRAY,
    DIAMOND_OP_INDEX_GET,
    DIAMOND_OP_INDEX_SET,
    DIAMOND_OP_HASH,
    DIAMOND_OP_NOT,
    DIAMOND_OP_JUMP_IF_TRUE,
    DIAMOND_OP_RETURN,
    DIAMOND_OP_RAISE,
    DIAMOND_OP_PUSH_RESCUE,
    DIAMOND_OP_POP_RESCUE,
    DIAMOND_OP_PUSH_ENSURE,
    DIAMOND_OP_RUN_ENSURE,
    DIAMOND_OP_END_ENSURE,
    DIAMOND_OP_IS_TYPE,
    DIAMOND_OP_ARGUMENT_PROVIDED,
    DIAMOND_OP_TO_STRING,
    DIAMOND_OP_YIELD,
    DIAMOND_OP_REDEFINE_METHOD,
    DIAMOND_OP_FIBER_NEW,
    DIAMOND_OP_PRINT,
    DIAMOND_OP_GETS,
    DIAMOND_OP_FILE_OPEN,
    DIAMOND_OP_FILE_DELETE,
    DIAMOND_OP_TCP_CONNECT,
    DIAMOND_OP_TCP_LISTEN,
    DIAMOND_OP_TCP_LISTEN_NONBLOCK,
    DIAMOND_OP_IO_POLL,
    DIAMOND_OP_UDP_BIND,
    DIAMOND_OP_UDP_OPEN,
    DIAMOND_OP_SIGNAL_TRAP,
    DIAMOND_OP_TLS_CONNECT,
    DIAMOND_OP_TLS_LISTEN,
    DIAMOND_OP_REGEXP_NEW,
    DIAMOND_OP_CHR,
    DIAMOND_OP_TO_FLOAT,
    DIAMOND_OP_TO_INT,
    DIAMOND_OP_TO_SYMBOL,
    DIAMOND_OP_MATH_UNARY,
    DIAMOND_OP_MATH_BINARY,
    DIAMOND_OP_PROGRAM_BUILDER_NEW,
    DIAMOND_OP_THREAD_NEW,
    /* Appended at the end, not grouped next to GET/SET_NAMESPACE_CONSTANT
     * above despite the similar shape, specifically to avoid shifting
     * every opcode declared after an insertion point -- ProgramBuilder-
     * based tests (see tests/cases/program_builder_call_declared_
     * function.di) and the self-hosted compiler bootstrap both encode
     * raw numeric opcode values, not symbolic names, so DiamondOpCode's
     * existing numbering is effectively a stable ABI within this
     * codebase, not just an implementation detail. */
    DIAMOND_OP_GET_CVAR,
    DIAMOND_OP_SET_CVAR,
    DIAMOND_OP_SQLITE3_OPEN,
    DIAMOND_OP_CHECK_DESTRUCTURE_COUNT,
    DIAMOND_OP_TIME_MONOTONIC,
    DIAMOND_OP_TIME_NOW,
    DIAMOND_OP_TIME_AT,
    DIAMOND_OP_SHIFT_LEFT,
    DIAMOND_OP_PROCESS_RUN,
    /* Process.spawn(argv) -- like Process.run, but returns a live
     * DIAMOND_OBJECT_PROCESS_HANDLE immediately rather than blocking
     * until the child exits: no output capture, no wait, just a
     * spawned child with two O_NONBLOCK stdout/stderr streams and a
     * pid. See docs/io.md. */
    DIAMOND_OP_PROCESS_SPAWN,
    DIAMOND_OP_DEBUGGER,
    DIAMOND_OP_ARGV,
    DIAMOND_OP_ENV,
    DIAMOND_OP_MODULO,
    /* `<=>` -- unlike LESS/LESS_EQUAL/GREATER/GREATER_EQUAL/EQUAL, the
     * result is an Int (-1/0/1) or Nil, never a Bool, and an unorderable
     * pair returns Nil rather than raising -- see vm.c's own handler
     * comment. Deliberately no _INT quickening variant (see the
     * Comparable/`<=>` design doc): not a hot inner-loop operator the
     * way `<`/`==` are. */
    DIAMOND_OP_COMPARE,
    DIAMOND_OP_POSTGRES_OPEN,
    DIAMOND_OP_MYSQL_OPEN,
    /* `ClassName.define_method(name, callable)` -- redefine_method's
     * add-a-new-slot counterpart: appends a new DiamondMethod entry
     * (DIAMOND_MAX_METHODS headroom already exists in the fixed-size
     * `methods[]` array; this is the first opcode to grow method_count
     * itself rather than just repointing an existing entry's
     * function_index). Same four safety checks as REDEFINE_METHOD
     * (String name, Callable value, zero captures, owner_class matches),
     * minus the arity-must-match check (nothing to match against yet) --
     * see vm.c's own handler comment. */
    DIAMOND_OP_DEFINE_METHOD,
    /* Loads a DIAMOND_VALUE_CLASS literal (a compile-time-known class
     * index, not a heap object) into a register -- used only to populate
     * the implicit `self` slot (register 0) of a class-owned singleton
     * method call with the *literal* class named at the call site, e.g.
     * `Author.find(db, 1)` loads Author's own class_index even when
     * `find` is inherited from Model, so `self` inside `find`'s body
     * reflects the actual receiver rather than Model (find's owner_class).
     * See DIAMOND_OP_INVOKE_SELF_METHOD below for what makes that useful. */
    DIAMOND_OP_LOAD_CLASS,
    /* `self.method_name(...)` written inside a class-owned singleton
     * method body -- the one place a Class value (register 0/self) is
     * ever dispatched against dynamically rather than resolved at compile
     * time: looks up method_name by name against self's actual
     * class_index, walking its superclass chain (mirrors lookup_method's
     * exact algorithm, vm.c), then calls it. This is what lets a method
     * shared on a base class (e.g. Model#self.find calling
     * self.repository()) reach whichever subclass actually received the
     * original call, even though find's own body is compiled once on
     * Model. Bare (non-self.) calls to a sibling singleton method are
     * deliberately NOT changed by this -- they keep resolving statically,
     * exactly as before; only the explicit self.foo(...) form is virtual.
     * See docs/design.md for the full scope (module namespace singletons
     * and bare calls are unaffected; Class values aren't general-purpose
     * runtime values). */
    DIAMOND_OP_INVOKE_SELF_METHOD,
    /* BCrypt.hash(password, cost) / BCrypt.verify(password, digest) --
     * both fixed-arity (no optional-argument support at this hand-rolled
     * class-call parse layer, see parse_bcrypt_call's own comment), backed
     * by libxcrypt's crypt_gensalt_rn/crypt_r (src/vm.c is the only file
     * allowed to include <crypt.h>, same confinement rule object.h already
     * documents for OpenSSL). */
    DIAMOND_OP_BCRYPT_HASH,
    DIAMOND_OP_BCRYPT_VERIFY,
    /* SecureRandom.bytes(n) / SecureRandom.hex(n) -- OpenSSL RAND_bytes,
     * already linked for TLS. */
    DIAMOND_OP_SECURE_RANDOM_BYTES,
    DIAMOND_OP_SECURE_RANDOM_HEX,
    /* Digest.sha256(data) / HMAC.sha256(key, data) -- lowercase hex
     * SHA-256 via the already-linked OpenSSL libcrypto. */
    DIAMOND_OP_DIGEST_SHA256,
    DIAMOND_OP_HMAC_SHA256,
    /* HMAC.verify(data, key, signature) -- recomputes the HMAC-SHA256 hex
     * digest and compares against `signature` with OpenSSL's
     * CRYPTO_memcmp (constant-time), the same primitive bcrypt_verify_
     * helper already uses -- for verifying a signed cookie/token without
     * a timing side-channel on the comparison itself. */
    DIAMOND_OP_HMAC_VERIFY,
    /* Digest.sha1(data) / HMAC.sha1(key, data) -- lowercase hex SHA-1,
     * mirroring the sha256 pair above. SHA-1 is cryptographically weak
     * for new signing use -- this exists because TOTP (RFC 6238) mandates
     * HMAC-SHA1 by spec, not as a general-purpose recommendation. */
    DIAMOND_OP_DIGEST_SHA1,
    DIAMOND_OP_HMAC_SHA1,
    /* Cipher.encrypt(key, plaintext) / Cipher.decrypt(key, blob) --
     * AES-256-GCM via the already-linked OpenSSL EVP_CIPHER API. `key`
     * must be exactly 32 raw bytes. `encrypt` returns one binary-safe
     * String: a fresh random 12-byte nonce, the 16-byte GCM tag, then the
     * ciphertext. `decrypt` returns nil (not an exception) on any
     * failure -- wrong key, tampered/truncated blob -- matching how a
     * forged cookie should be handled by calling code, the same
     * reasoning BCrypt.verify's own comment already gives for a
     * malformed digest. */
    DIAMOND_OP_CIPHER_ENCRYPT,
    DIAMOND_OP_CIPHER_DECRYPT,
    /* Gzip.compress(data) / Gzip.decompress(data, max_size) -- gzip-
     * wrapped deflate via the already-linked zlib. `decompress`'s
     * `max_size` bounds the decompressed output, checked incrementally
     * (not after the fact) against a "zip bomb" -- a small,
     * attacker-controlled input decompressing to an unbounded output. */
    DIAMOND_OP_GZIP_COMPRESS,
    DIAMOND_OP_GZIP_DECOMPRESS,
    /* Base64.encode(data) / Base64.decode(data) -- standard (RFC 4648)
     * alphabet with padding, via the already-linked OpenSSL
     * EVP_EncodeBlock/EVP_DecodeBlock. Needed for HTTP Basic auth
     * (`Authorization: Basic <base64>`). */
    DIAMOND_OP_BASE64_ENCODE,
    DIAMOND_OP_BASE64_DECODE,
    /* ClassName.compile_method(name, params, body_source) -- compiles a
     * new method body from a source string at runtime and returns a
     * Callable, meant to be installed via the existing
     * DIAMOND_OP_DEFINE_METHOD (ClassName.define_method(name, callable)).
     * See docs/design.md's "Runtime method synthesis" section. */
    DIAMOND_OP_COMPILE_METHOD,
    /* exit(code = 0) -- an immediate, whole-process libc exit(), not a
     * raised/rescuable Diamond exception: no `ensure` block anywhere on
     * the call stack runs, and (Thread.new spawning real OS threads
     * sharing one process, per docs/threads.md) every other thread stops
     * too. A validation failure (non-Int, or outside 0..255) still raises
     * an ordinary rescuable TypeError/ArgumentError first -- only a valid
     * code actually terminates. See docs/design.md's "exit()" section. */
    DIAMOND_OP_EXIT,
    /* Prologue of a `*name` (variadic) parameter's owning function/
     * method/closure -- collects every argument beyond the fixed
     * (non-variadic) parameter count into a fresh Array and stores it in
     * the variadic parameter's own register. Its third operand preserves
     * that many trailing arguments in the registers immediately after the
     * variadic slot, allowing `*arguments, &block` to keep the block out of
     * the collected Array. Reads the call's original,
     * un-clamped `arguments`/`argument_count` (still live in run_chunk's
     * own scope, see docs/design.md's "Splat/variadic parameters"
     * section), not the callee's registers -- those only ever receive
     * up to `register_count` copied values now (see run_chunk's own
     * bounds fix, same section). */
    DIAMOND_OP_COLLECT_VARIADIC,
    /* `foo(*array)` -- call-site spread, the caller-side counterpart to
     * DIAMOND_OP_COLLECT_VARIADIC. Unlike DIAMOND_OP_CALL, there's no
     * compile-time argument count operand at all: the Array's own
     * runtime `count` becomes the call's argument_count, and its own
     * backing storage (already a contiguous DiamondValue* -- an Array's
     * normal in-memory shape) is passed straight to run_chunk as
     * `arguments`, no copy needed. Deliberately narrow first slice: only
     * a bare top-level function call, and only when the spread argument
     * is the call's *sole* argument -- see docs/design.md's "Call-site
     * spread" section. */
    DIAMOND_OP_CALL_SPREAD,
    /* `case subject; when pattern` matching. Range patterns test inclusion,
     * Regexp patterns search String subjects, class patterns accept instances
     * of that class/subclasses, and every other value retains `==` semantics. */
    DIAMOND_OP_CASE_MATCH,
    /* Non-raising exact Array shape predicate used by binding patterns. */
    DIAMOND_OP_CASE_ARRAY_SHAPE,
    /* Copies Array elements from a fixed start index into a fresh Array. */
    DIAMOND_OP_ARRAY_REST,
    /* Non-raising Hash type and required-key predicates for case patterns. */
    DIAMOND_OP_CASE_HASH_SHAPE,
    DIAMOND_OP_CASE_HASH_HAS,
    /* Copies Hash entries whose keys are absent from an exclusion Array. */
    DIAMOND_OP_HASH_REST,
    /* Middle-rest Array extraction and suffix-relative pattern indexing. */
    DIAMOND_OP_ARRAY_MIDDLE,
    DIAMOND_OP_ARRAY_SUFFIX,
    /* Raising required-key check for Hash destructuring assignment. */
    DIAMOND_OP_CHECK_HASH_KEY,
    /* `receiver.method(*array)` for user-defined instance methods. */
    DIAMOND_OP_INVOKE_SPREAD,
    /* `callable(*array)` for closure values. */
    DIAMOND_OP_CALL_CLOSURE_SPREAD,
    /* `ClassName.new(*array)` for constructor calls. */
    DIAMOND_OP_NEW_SPREAD,
    /* `Namespace.singleton(*array)` with an optional class self slot. */
    DIAMOND_OP_CALL_SINGLETON_SPREAD,
    /* Builds one argument Array from fixed prefix, spread, and suffix values.
     * The suffix-count high bit omits a final explicit `&nil` block. */
    DIAMOND_OP_BUILD_SPREAD_ARGS,
    /* Explicit type bindings for `function[T](*arguments)`. */
    DIAMOND_OP_CALL_TYPED_SPREAD,
    /* Explicit type bindings for `receiver.method[T](*arguments)`. */
    DIAMOND_OP_INVOKE_TYPED_SPREAD,
    /* Explicit type bindings for `Namespace.method[T](*arguments)`. */
    DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD,
    /* Top-level `function(*arguments, keyword: value)` runtime slot merge. */
    DIAMOND_OP_CALL_KEYWORD_SPREAD,
    DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD,
    /* Keyword-bearing user dispatches carry a positional Array plus
     * name/register pairs resolved against the runtime target. Their keyword
     * count high bit adds one trailing block register. */
    DIAMOND_OP_INVOKE_KEYWORDS,
    DIAMOND_OP_INVOKE_TYPED_KEYWORDS,
    DIAMOND_OP_CALL_CLOSURE_KEYWORDS,
    DIAMOND_OP_NEW_KEYWORDS,
    DIAMOND_OP_CALL_SINGLETON_KEYWORDS,
    DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS,
    /* `File.dirname`/`.basename`/`.extname`/`.absolute?`/`.expand_path`
     * -- selector byte picks the operation, see DiamondFilePathFunction.
     * `File.join` is a separate opcode below, not folded in here, since
     * it alone takes a variable number of String arguments rather than
     * a fixed one or two. */
    DIAMOND_OP_FILE_PATH,
    /* `File.join(*parts)` -- `parts` are `count` contiguous registers
     * starting at `base`, the same "one opcode, variadic args passed as
     * a contiguous register run" shape DIAMOND_OP_THREAD_NEW already
     * uses for its own variadic argument list. */
    DIAMOND_OP_FILE_JOIN,
    /* Time.parse(string), appended to preserve all existing opcode values. */
    DIAMOND_OP_TIME_PARSE,
    /* Time.utc/local/fixed(...), also appended for opcode stability. */
    DIAMOND_OP_TIME_BUILD,
    /* Dir.entries(path) -- every name in a directory (opendir/readdir),
     * excluding "." and "..", as an Array of Strings. No recursion, no
     * glob patterns -- just the one primitive Diamond had none of at
     * all before this (confirmed: no Dir class, nothing in this enum);
     * build anything more elaborate (a recursive walk, a glob) out of
     * this in Diamond itself. */
    DIAMOND_OP_DIR_ENTRIES,
    /* Int-only bitwise operators (>>/&/|/^) -- matching DIAMOND_OP_SHIFT_
     * LEFT's own scope exactly: no bignum support, not user-overloadable
     * (no invoke_operator_method dispatch), no quickening. `<<` already
     * existed (also doubling as Array#<<'s append); these four fill out
     * the rest of the set it was always missing. >> is arithmetic
     * (sign-extending), matching Ruby's own Integer#>> -- well-defined
     * for a negative left-hand side under C23 (this project's own
     * -std=c23), not the classic "implementation-defined" UB concern an
     * older C standard would have here. */
    DIAMOND_OP_SHIFT_RIGHT,
    DIAMOND_OP_BITWISE_AND,
    DIAMOND_OP_BITWISE_OR,
    DIAMOND_OP_BITWISE_XOR,
    /* `value.class()` -- dest, source. Returns a String naming
     * `source`'s own runtime type (format_value_type's own output,
     * src/vm.c -- the exact bare name already used in every "expected
     * X, got Y" type-error message, e.g. "String", "Int",
     * "RuntimeError" for a user class instance). Compiler-recognized
     * (src/compiler.c's own parse_invoke), not a real method on any
     * class -- works uniformly on every value, including native kinds
     * with no DiamondClass of their own at all. */
    DIAMOND_OP_CLASS_NAME,
    /* Tensor.zeros(rows, cols) -- a new all-zero DiamondTensor. Prototype
     * scope: see DiamondTensor's own comment in object.h. */
    DIAMOND_OP_TENSOR_ZEROS,
    /* Tensor.from_array(nested_array) -- nested_array is an Array of
     * Arrays of Int/Float, all rows the same length. */
    DIAMOND_OP_TENSOR_FROM_ARRAY,
    /* Tensor.random(rows, cols, seed) -- deterministic (same seed ->
     * same values) uniform-random Tensor in [-1, 1), generated directly
     * in C rather than round-tripping through a Diamond-level nested
     * Array like Tensor.from_array requires -- weight init at any real
     * model size (a vocab_size x d_model embedding table alone can be
     * millions of elements) was the actual measured bottleneck in
     * examples/transformer, not #matmul itself. */
    DIAMOND_OP_TENSOR_RANDOM,
    /* Channel.new(capacity) -- see docs/threads.md's Channels section.
     * Appended here, not grouped next to DIAMOND_OP_THREAD_NEW above,
     * for the same stable-numbering reason this enum's own comment
     * gives for DIAMOND_OP_GET_CVAR onward. */
    DIAMOND_OP_CHANNEL_NEW,
    DIAMOND_OP_COUNT,
} DiamondOpCode;

/* Selector for DIAMOND_OP_FILE_PATH -- one opcode for this small family
 * of fixed-arity pure path-string utilities, the same "one opcode + a
 * selector byte" shape DIAMOND_OP_MATH_UNARY/_BINARY already use for
 * sqrt/sin/cos/tan/pow, rather than one new opcode per method. */
typedef enum {
    DIAMOND_FILE_PATH_DIRNAME,
    DIAMOND_FILE_PATH_BASENAME,
    DIAMOND_FILE_PATH_EXTNAME,
    DIAMOND_FILE_PATH_ABSOLUTE,
    DIAMOND_FILE_PATH_EXPAND,
    /* File.directory?(path) -- unlike ABSOLUTE (a pure string check),
     * this is real I/O (stat()); a stat() failure (doesn't exist,
     * permission denied, ...) reads as false, not an error -- matches
     * Ruby's own File.directory? contract, and is what lets a Diamond-
     * level recursive directory walk (Dir.entries + this) use it in a
     * plain condition without needing to rescue anything first. */
    DIAMOND_FILE_PATH_DIRECTORY,
} DiamondFilePathFunction;

typedef enum DiamondMathFunction : uint8_t {
    DIAMOND_MATH_SQRT,
    DIAMOND_MATH_SIN,
    DIAMOND_MATH_COS,
    DIAMOND_MATH_TAN,
    DIAMOND_MATH_POW,
    DIAMOND_MATH_EXP,
    DIAMOND_MATH_LOG,
    DIAMOND_MATH_TANH,
} DiamondMathFunction;

typedef enum DiamondTypeId : uint8_t {
    DIAMOND_TYPE_INT,
    DIAMOND_TYPE_FLOAT,
    DIAMOND_TYPE_STRING,
    DIAMOND_TYPE_BOOL,
    DIAMOND_TYPE_NIL,
    DIAMOND_TYPE_ARRAY,
    DIAMOND_TYPE_HASH,
    DIAMOND_TYPE_CALLABLE,
    DIAMOND_TYPE_SIZED,
    DIAMOND_TYPE_SYMBOL,
    DIAMOND_TYPE_CLASS_BASE,
    /* Class ids occupy CLASS_BASE..CLASS_BASE+MAX_CLASSES-1 (10..189).
     * Keep generic variables and interfaces above that full advertised
     * range: the old 96/128 split caused programs with more than 86 classes
     * to reinterpret ordinary class ids as generic/interface ids. */
    DIAMOND_TYPE_VARIABLE_BASE = 192,
    DIAMOND_TYPE_INTERFACE_BASE = 224,
} DiamondTypeId;

enum { DIAMOND_INLINE_CACHE_COUNT = 64 };
enum { DIAMOND_INLINE_CACHE_WIDTH = 4 };
/* INT, TERM, HUP -- see docs/io.md's signals section for why this
 * specific small, fixed set rather than every signal name POSIX knows
 * about. */
enum { DIAMOND_SIGNAL_COUNT = 3 };

typedef enum DiamondBuiltinClass : uint8_t {
    DIAMOND_CLASS_EXCEPTION,
    DIAMOND_CLASS_STANDARD_ERROR,
    DIAMOND_CLASS_RUNTIME_ERROR,
    DIAMOND_CLASS_TYPE_ERROR,
    DIAMOND_CLASS_ARGUMENT_ERROR,
    DIAMOND_CLASS_INDEX_ERROR,
    DIAMOND_CLASS_ZERO_DIVISION_ERROR,
    DIAMOND_CLASS_RANGE_ERROR,
    DIAMOND_CLASS_SYSTEM_STACK_ERROR,
    DIAMOND_CLASS_FIBER_ERROR,
    DIAMOND_CLASS_IO_ERROR,
    DIAMOND_CLASS_REGEXP_ERROR,
    DIAMOND_CLASS_WOULD_BLOCK_ERROR,
    DIAMOND_CLASS_THREAD_ERROR,
    DIAMOND_CLASS_SQLITE3_ERROR,
    DIAMOND_CLASS_POSTGRES_ERROR,
    DIAMOND_CLASS_MYSQL_ERROR,
    DIAMOND_CLASS_NO_METHOD_ERROR,
    DIAMOND_CLASS_JSON_ERROR,
    DIAMOND_BUILTIN_CLASS_COUNT,
} DiamondBuiltinClass;

typedef struct DiamondStringConstant {
    char chars[DIAMOND_MAX_STRING_LENGTH + 1];
    size_t length;
} DiamondStringConstant;

typedef struct DiamondTypeMember {
    uint8_t id;
    uint16_t argument_set;
    uint16_t second_argument_set;
    uint8_t callable_arity;
    uint16_t callable_return_set;
    bool callable_parameters_typed;
    uint16_t callable_parameter_sets[16];
} DiamondTypeMember;

typedef struct DiamondTypeSet {
    DiamondTypeMember members[DIAMOND_MAX_UNION_TYPES];
    uint8_t count;
    /* True for advisory unions synthesized by compiler control-flow joins,
     * rather than written as a source annotation. They can prove a compatible
     * annotation but must retain the historical runtime-check fallback when
     * they do not satisfy one; see emit_type_check. */
    bool inferred;
} DiamondTypeSet;

typedef struct DiamondChunk DiamondChunk;

typedef struct DiamondMethod {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint16_t function_index;
    uint8_t arity;
    uint8_t required_arity;
    /* Mirrors DiamondFunction.has_variadic (same meaning) -- duplicated
     * here for the same reason arity/required_arity already are: fast
     * dispatch-time arity checking without following function_index back
     * to the full DiamondFunction first. */
    bool has_variadic;
    bool included;
    bool is_private;
    bool is_protected;
    bool needs_receiver;
    /* Non-null only for a method installed by ClassName.compile_method +
     * .define_method (src/vm.c's DIAMOND_OP_COMPILE_METHOD/
     * DIAMOND_OP_DEFINE_METHOD): function_index above is meaningful only
     * relative to *this* chunk, not the chunk the receiving instance's
     * own class lives in -- the same "function_index is only meaningful
     * relative to whichever chunk owns it" fact DiamondInstance.owner
     * already exists to handle for ProgramBuilder-adopted instances.
     * nullptr (the default for every ordinary, source-declared method)
     * means "use the receiver's own owner chunk / the ambient chunk,
     * exactly as before" -- zero behavior change there. Kept alive
     * forever via vm->adopted_programs, the same lifetime mechanism
     * ProgramBuilder's own adopt mode already uses. */
    const DiamondChunk *source_chunk;
    /* compile_method's bound_values, copied from the installing
     * DiamondClosure's own fields of the same name (see DiamondClosure's
     * own comment, src/object.h) -- nullptr/0 for every ordinary method.
     * arity/required_arity above already exclude these (a caller of the
     * installed method never supplies them); dispatch appends
     * bound_values[0..bound_value_count) after the caller's own explicit
     * arguments before entering the function. */
    const DiamondValue *bound_values;
    uint8_t bound_value_count;
} DiamondMethod;

typedef struct DiamondInterfaceMethod {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t arity;
    uint16_t parameter_type_sets[DIAMOND_MAX_DECLARED_PARAMETERS];
    uint16_t return_type_set;
} DiamondInterfaceMethod;

typedef struct DiamondInterface {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint32_t declaration_line;
    uint32_t declaration_column;
    size_t declaration_start;
    DiamondInterfaceMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    const DiamondTypeSet *type_sets;
    /* True only for an entry seeded by diamond_compile's own throwaway
     * discovery pass and not yet "claimed" by the real second pass --
     * compile_interface's own name-collision check (src/compiler.c) reads
     * this to tell "this is my own pre-registered slot, reuse it" apart
     * from a genuine duplicate declaration. Never true outside that one
     * seeding window; a normal single compile (the discovery pass
     * itself, or any DiamondInterface built directly by the
     * ProgramBuilder native bridge in src/vm.c) never sets it. */
    bool declared_by_discovery;
} DiamondInterface;

typedef struct DiamondModule {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint32_t declaration_line;
    uint32_t declaration_column;
    size_t declaration_start;
    DiamondMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    DiamondMethod singleton_methods[DIAMOND_MAX_METHODS];
    size_t singleton_method_count;
    /* Mirrors Compiler's own next_function_claim (src/compiler.c), scoped
     * per-module instead of program-wide: how many of this module's
     * discovery-pass-populated singleton_methods[] entries (module_
     * function's exported form, and `def self.x` inside a module) the
     * real pass has re-visited and refreshed in place so far. Lets an
     * earlier-compiled module_function sibling see a later one's already-
     * correct (same source order in both passes) descriptor instead of
     * an empty table -- see compile_module's own comment on why
     * singleton_methods, unlike methods[]/fields[], is never wiped on
     * reopen. Reset to 0 exactly once, alongside that reopen. */
    size_t next_singleton_claim;
    char fields[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t field_count;
    /* See DiamondInterface's own copy of this field just above. */
    bool declared_by_discovery;
} DiamondModule;

typedef struct DiamondClass DiamondClass;

typedef struct DiamondShape {
    const DiamondClass *class;
    uint8_t field_count;
} DiamondShape;

struct DiamondClass {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    /* 1-based source position of the class's own name token in `class
     * Name` -- lsp/definition.c and lsp/document_symbol.c's only reason
     * for existing. Nothing in the VM itself reads these.
     * declaration_start is the matching byte offset into the *compiled*
     * source buffer (core prelude + require-bundled user source), which
     * lsp/definition.c needs to resolve a symbol pulled in from a
     * required file back to that file's own path via
     * diamond_resolve_diagnostic_location's segment table -- the same
     * byte-offset-to-file mapping a compile error already gets. */
    uint32_t declaration_line;
    uint32_t declaration_column;
    size_t declaration_start;
    uint8_t superclass;
    DiamondMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    DiamondMethod singleton_methods[DIAMOND_MAX_METHODS];
    size_t singleton_method_count;
    char fields[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t field_count;
    /* LSP-only conservative receiver metadata: 0 unseen, 1 every compiled
     * assignment agreed on field_known_class, 2 unknown/conflicting. */
    uint8_t field_type_status[DIAMOND_MAX_FIELDS];
    uint8_t field_known_class[DIAMOND_MAX_FIELDS];
    DiamondShape shapes[DIAMOND_MAX_FIELDS + 1];
    /* Class variable *names* only -- compile-time, pointer-free metadata
     * exactly like `fields` above, so it costs nothing extra in
     * Thread.new's whole-DiamondProgram memcpy clone (see docs/threads.md).
     * The actual per-variable *values* are runtime, per-VM state and live
     * on DiamondVm instead (see its own class_variables field below) --
     * same split `namespace_constants` already uses between this
     * class's own name table and DiamondVm's namespace_constants[]
     * value array, and for the same reason: a DiamondValue can hold a
     * GC object pointer, and DiamondProgram's tables are raw-memcpy'd
     * across a Thread boundary into a completely separate heap, so no
     * DiamondValue can ever live in them. */
    char class_variables[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t class_variable_count;
    /* See DiamondInterface's own copy of this field (this file, above). */
    bool declared_by_discovery;
};

/* One local variable or parameter's name and the byte range (in the
 * *compiled* buffer -- core prelude + require-bundled user source,
 * same coordinate system declaration_start above already uses) it's
 * actually in scope for. Ordinarily that's the rest of its declaring
 * function's body (valid_end == the function's own closing position),
 * narrower for a name bound by a `rescue` clause (valid_end == that
 * clause's own end). lsp/'s only reason for existing (completion,
 * docs/lsp.md) -- nothing else in the VM reads this. */
typedef struct DiamondScopeLocal {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    size_t valid_start;
    size_t valid_end;
    uint16_t reg;
    /* Fallback compiler type snapshot for this local. Position-sensitive
     * assignment facts below take precedence for receiver resolution. */
    uint8_t known_type;
    /* Fallback compiler known_type_sets[reg] for this local's register --
     * meaningful only relative to the *owning function's own* type_sets[]
     * table (a set index means nothing against any other function's), unlike
     * known_type which is globally self-describing. -1 (matching the
     * compiler's own "no set" sentinel) when there isn't one -- e.g. an
     * ordinary single-class local. Besides explicit annotations, control-flow
     * joins can synthesize this set when all paths have representable known
     * types (`cond ? Dog.new() : Cat.new()`, for example). */
    int32_t known_type_set;
} DiamondScopeLocal;

typedef struct DiamondScopeTypeFact {
    /* Register identity is stable because compiler allocation is monotonic;
     * effective_start makes lookup choose the last assignment at or before
     * the cursor without retaining an AST. */
    uint16_t reg;
    size_t effective_start;
    uint8_t known_type;
    int32_t known_type_set;
} DiamondScopeTypeFact;

typedef struct DiamondFunction {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    /* 1-based source position of the function/method's own name token
     * in its declaring `def` -- lsp/definition.c and lsp/document_
     * symbol.c's only reason for existing. Not to be confused with
     * lines[]/columns[] below, which are per-bytecode-offset arrays for
     * runtime stack traces, a completely different thing despite the
     * similar names. declaration_start is the matching byte offset,
     * see DiamondClass's own copy of this comment for why. */
    uint32_t declaration_line;
    uint32_t declaration_column;
    size_t declaration_start;
    /* Byte offset in the *compiled* buffer of the position right after
     * this function's own closing `end` (or, for an endless `def
     * foo()=expr` or the top-level program itself, the equivalent point
     * with no `end` token at all) -- record_scope_locals's own
     * `valid_end` argument at every one of its call sites (src/
     * compiler.c), copied here too so a function's body extent is known
     * even when it has zero parameters and zero locals (so
     * scope_locals itself is empty and can't answer "does this function
     * contain byte offset X" on its own). lsp/receiver.c's only reason
     * for existing -- finding which function's body a bare `self` token
     * lexically falls inside, to resolve `self.foo(...)`'s receiver
     * class via that function's own owner_class. */
    size_t body_end;
    uint8_t *code;
    uint32_t *lines;
    uint32_t *columns;
    size_t code_count;
    size_t code_capacity;
    DiamondValue *constants;
    size_t constant_count;
    size_t constant_capacity;
    DiamondStringConstant *strings;
    size_t string_count;
    size_t string_capacity;
    DiamondTypeSet *type_sets;
    size_t type_set_count;
    size_t type_set_capacity;
    /* True only for a function built by diamond_function_copy (src/
     * compiler.c) -- code/lines/columns/constants/strings/type_sets
     * above all point into ONE combined malloc'd block instead of 6
     * separate ones (that function's own comment explains why: cuts a
     * clone from 6 small allocations to 1, which matters wherever many
     * functions get cloned per call -- seed_program_from_template,
     * clone_program_from_chunk for Thread.new, diamond_program_read_
     * compiled). Only `code` is the block's real, freeable base pointer
     * in that case; the other 5 are interior pointers realloc()/free()
     * must never see directly. diamond_program_free checks this before
     * freeing a function's arrays, and diamond_function_reserve_code/
     * _constants/_strings/_type_sets assert it's false before ever
     * growing one of these arrays in place -- a combined-allocated
     * function is a fixed-size, immutable snapshot for its entire
     * lifetime, never grown after diamond_function_copy returns. */
    bool owns_combined_buffer;
    uint8_t arity;
    uint8_t required_arity;
    /* Set only by a `*name` parameter. It occupies the final slot unless a
     * trailing explicit block parameter follows it. */
    bool has_variadic;
    /* The final public parameter was declared with `&name`. Tooling uses this
     * to preserve source-level signature shape. */
    bool has_block_parameter;
    uint8_t owner_class;
    bool nested;
    uint8_t capture_count;
    uint16_t return_type_set;
    /* LSP-only best-effort return type, inferred from an unannotated
     * function/method's own body_result (compile_definition, src/
     * compiler.c) the same way compile_block already infers a block's
     * return_type_set when it has no explicit `-> Type` annotation.
     * Deliberately a *separate* field rather than folding into
     * return_type_set itself: that field is load-bearing for real
     * compile-time semantics (structural interface conformance, generic
     * instantiation/substitution -- see its own call sites), so widening
     * when it gets populated would risk changing what type-checks,
     * not just what an editor can show. inferred_return_type_set has
     * exactly one reader: lsp/receiver.c's call-chain resolution, as a
     * fallback when return_type_set itself is DIAMOND_NO_TYPE_SET. */
    uint16_t inferred_return_type_set;
    uint16_t parameter_type_sets[DIAMOND_MAX_DECLARED_PARAMETERS];
    /* Declared public parameter names. Dynamic keyword calls retain names in
     * bytecode and resolve them here after target selection. Hidden self
     * slots are deliberately excluded. */
    char parameter_names[DIAMOND_MAX_DECLARED_PARAMETERS][DIAMOND_MAX_FUNCTION_NAME];
    char type_variables[8][DIAMOND_MAX_FUNCTION_NAME];
    uint8_t type_variable_count;
    bool uses_instance_state;
    /* A function slot copied from diamond_compile's discovery pass and not
     * yet claimed by the real pass. Preserving every discovery-time index
     * keeps early CALL operands and class/module method tables stable. */
    bool declared_by_discovery;
    /* High-water mark of allocate_register() within this function body.
     * Safe as an exact zero-init/GC-scan bound only because register
     * allocation is monotonic per function body (never recycled). */
    uint16_t register_count;
    /* Every local/parameter declared directly in this function's own
     * body (not a nested `def`'s -- that gets its own DiamondFunction
     * and its own scope_locals), see DiamondScopeLocal above. Also
     * covers the top-level program's own locals: top-level code
     * compiles into program->entry, a DiamondFunction like any other
     * (compile_definition's own at_top_level check, src/compiler.c). */
    DiamondScopeLocal scope_locals[DIAMOND_MAX_SCOPE_LOCALS];
    size_t scope_local_count;
    DiamondScopeTypeFact scope_type_facts[DIAMOND_MAX_SCOPE_TYPE_FACTS];
    size_t scope_type_fact_count;
} DiamondFunction;

/* arity/required_arity above count an implicit self slot for a genuine
 * class/module method -- compile_definition (src/compiler.c) reserves
 * register 0 for self and folds it into both counts for any owner_class
 * that is a real class index or the UINT8_MAX-1 module-method sentinel
 * (search that function for "direct_class_member" and "nested_in_
 * singleton_method"); the caller-visible parameter_type_sets/parameter_
 * names arrays are still indexed from the first *declared* parameter, 0,
 * with no such offset (their own comment: "Hidden self slots are
 * deliberately excluded"). A `closure` (captures_self, owner_class==
 * UINT8_MAX-2) also reserves register 0 for self but does NOT fold it
 * into arity -- self arrives via capture, not an implicit call-time
 * argument, so its arity already means exactly what a caller sees. An
 * ordinary function/nested def with no self at all (owner_class==
 * UINT8_MAX) has nothing to subtract either. Any code that treats
 * arity/required_arity as "how many arguments does a generic Callable
 * *value* holding this function actually take" -- as opposed to code
 * that already knows it's dispatching a real method and supplies self
 * separately -- must subtract this offset first, or it double-counts
 * self as a real parameter. Found the hard way: a nested `def` written
 * directly inside a `def self.x` singleton method (the nested_in_
 * singleton_method patch-factory shape above) got owner_class set to
 * the enclosing class specifically so redefine_method's own exact-match
 * check keeps working, but every *other* consumer of such a closure --
 * passing it to Array#find, storing it and calling it as a Callable
 * value -- only ever supplies its own real, self-less arguments. */
static inline uint8_t diamond_function_self_offset(const DiamondFunction *fn) {
    return (fn->owner_class!=UINT8_MAX&&fn->owner_class!=(uint8_t)(UINT8_MAX-2))?1:0;
}

struct DiamondChunk {
    const char *name;
    const uint8_t *code;
    const uint32_t *lines;
    const uint32_t *columns;
    size_t code_count;
    const DiamondValue *constants;
    size_t constant_count;
    const DiamondStringConstant *strings;
    size_t string_count;
    const DiamondTypeSet *type_sets;
    size_t type_set_count;
    DiamondFunction *const *functions;
    size_t function_count;
    const DiamondClass *classes;
    size_t class_count;
    const DiamondInterface *interfaces;
    size_t interface_count;
    const DiamondModule *modules;
    size_t module_count;
    const uint16_t *parameter_type_sets;
    uint8_t type_variable_count;
    uint8_t parameter_offset;
    const DiamondTypeBinding *type_variable_bindings;
    uint16_t register_count;
    /* Mirrors DiamondFunction/DiamondMethod.has_variadic for whichever
     * function/method run_chunk is currently executing -- omitted (so
     * false) at every DiamondChunk literal that isn't a real, possibly-
     * variadic user function/method dispatch (Fiber/Thread bootstrap,
     * ProgramBuilder-adopted programs, the top-level program entry,
     * etc.), which is every construction site except the handful of
     * actual call/invoke opcode handlers in src/vm.c. */
    bool has_variadic;
    /* Index into `classes` of the class named "Range" (lib/core.di),
     * resolved once at the end of diamond_compile (by name, never
     * hardcoded -- confirmed directly that its index can shift with
     * conservative prelude module selection, so nothing may assume a
     * fixed value), UINT8_MAX if not found (a scratch/partial compile
     * without the prelude). DIAMOND_OP_INDEX_GET/SET's only reason for
     * existing -- recognizing a Range receiver for arr[range] slicing
     * without a native VM value kind for Range at all. */
    uint8_t range_class_index;
};

typedef enum DiamondVmStatus : uint8_t {
    DIAMOND_VM_OK,
    DIAMOND_VM_INVALID_BYTECODE,
    DIAMOND_VM_TYPE_ERROR,
    DIAMOND_VM_INTEGER_OVERFLOW,
    DIAMOND_VM_DIVISION_BY_ZERO,
    DIAMOND_VM_ARITY_ERROR,
    DIAMOND_VM_STACK_OVERFLOW,
    DIAMOND_VM_OUT_OF_MEMORY,
    DIAMOND_VM_INDEX_ERROR,
    DIAMOND_VM_EXCEPTION,
    DIAMOND_VM_YIELDED,
    DIAMOND_VM_YIELD_WITHOUT_FIBER,
    DIAMOND_VM_FIBER_NOT_RESUMABLE,
    DIAMOND_VM_IO_ERROR,
    DIAMOND_VM_REGEXP_ERROR,
    /* Raised by .read(n)/.write(value) on a non-blocking Socket (returned
     * from a TCPServer.listen_nonblocking listener's .accept()) when the
     * underlying read(2)/write(2) would otherwise block -- EAGAIN/
     * EWOULDBLOCK, not a real error. Rescuable as WouldBlockError so a
     * poll-driven caller can catch it and yield rather than treating it
     * like any other IOError. See docs/io.md. */
    DIAMOND_VM_WOULD_BLOCK,
    /* Raised when a ProgramBuilder-constructed DiamondProgram fails while
     * executing under .run() -- a distinct status from DIAMOND_VM_TYPE_ERROR
     * (which covers builder API misuse: bad argument types, out-of-range
     * indices) specifically because the two need different rescue targets:
     * "you called the builder wrong" vs. "the program you built didn't
     * work." See docs/roadmap.md. */
    DIAMOND_VM_PROGRAM_ERROR,
    /* Thread.new/.join failures that aren't the spawned thread's own
     * Diamond-level exception re-raised (see run_chunk's THREAD_NEW case
     * and .join()'s dispatch, src/vm.c): pthread_create failure, the
     * DIAMOND_MAX_THREADS cap, or a child thread that hit an internal VM
     * failure rather than a clean, re-raisable exception. See
     * docs/threads.md. */
    DIAMOND_VM_THREAD_ERROR,
    /* SQLite3.open/#execute/#query/#close failures: a bad path, a
     * malformed statement, a bind/step error, or more than one
     * semicolon-separated statement passed to a single call. Message is
     * always sqlite3_errmsg(db) (or a locally-detected condition like
     * the multi-statement guard), surfaced as SQLite3Error -- see
     * exception_class_for_status. */
    DIAMOND_VM_SQLITE3_ERROR,
    /* PostgreSQL.open/#execute/#query/#close failures: an unreachable
     * host or bad conninfo, a malformed statement, a bind/exec error, or
     * a #last_insert_row_id() call before any sequence was used this
     * session (SELECT lastval() itself raising). Message is always
     * PQerrorMessage(conn) (open failures) or
     * PQresultErrorMessage(res)/a locally-detected condition (everything
     * else), surfaced as PostgreSQLError -- see exception_class_for_status. */
    DIAMOND_VM_POSTGRES_ERROR,
    /* MySQL.open/#execute/#query/#close failures: an unreachable host or
     * bad credentials, a malformed statement, a bind/exec error, or the
     * placeholder-count mismatch check mysql_stmt_param_count enables.
     * Message is always mysql_error(conn) (open failures) or
     * mysql_stmt_error(stmt)/a locally-detected condition (everything
     * else), surfaced as MySQLError -- see exception_class_for_status. */
    DIAMOND_VM_MYSQL_ERROR,
    /* An ordinary instance method call found no method of that name on
     * the receiver's class (superclass chain included) and the class has
     * no method_missing of its own either -- see method_missing_helper's
     * own comment (src/vm.c) for the full fallback shape and its
     * deliberately narrow scope (this one dispatch site only). Surfaced
     * as NoMethodError, a real descriptive exception instead of the bare
     * generic TypeError this replaced. */
    DIAMOND_VM_NO_METHOD_ERROR,
    /* String#parse_json failures: malformed input at the native JSON
     * parser (src/vm.c's json_parse_value and friends), replacing
     * lib/core/json_codec.di's own pure-Diamond JSONCodec#parse for
     * JSON.parse's hot path. Message is always a specific parse-position
     * diagnostic, surfaced as JSONError -- see exception_class_for_status.
     * JSONError itself moved from a lib/core/json_codec.di class
     * declaration to a builtin (DIAMOND_CLASS_JSON_ERROR) so native code
     * can raise it the same generic way every other native error class
     * here already does. */
    DIAMOND_VM_JSON_ERROR,
} DiamondVmStatus;

typedef struct DiamondMethodCacheEntry {
    const DiamondClass *receiver_class;
    const DiamondMethod *method;
} DiamondMethodCacheEntry;

typedef struct DiamondMethodCache {
    const uint8_t *site;
    DiamondMethodCacheEntry entries[DIAMOND_INLINE_CACHE_WIDTH];
    uint8_t entry_count;
    uint8_t next_replace;
    size_t hits;
    size_t misses;
} DiamondMethodCache;

typedef struct DiamondFieldCacheEntry {
    const DiamondShape *input_shape;
    const DiamondShape *output_shape;
    bool materialized;
} DiamondFieldCacheEntry;

typedef struct DiamondFieldCache {
    const uint8_t *site;
    DiamondFieldCacheEntry entries[DIAMOND_INLINE_CACHE_WIDTH];
    uint8_t entry_count;
    uint8_t next_replace;
} DiamondFieldCache;

typedef enum DiamondFiberState : uint8_t {
    DIAMOND_FIBER_NEW,
    DIAMOND_FIBER_RUNNABLE,
    DIAMOND_FIBER_RUNNING,
    DIAMOND_FIBER_SUSPENDED,
    DIAMOND_FIBER_COMPLETED,
    DIAMOND_FIBER_FAILED,
} DiamondFiberState;

typedef struct DiamondVm DiamondVm;

typedef struct DiamondFiber {
    DiamondFiberState state;
    const DiamondChunk *chunk;
    DiamondValue result;
    DiamondVmStatus status;
    DiamondVm *vm;
    ucontext_t context;
    ucontext_t *resume_target;
    void *stack;
    size_t stack_size;
    void *native_frames;
    DiamondValue resume_value;
    const DiamondClosure *entry_closure;
    DiamondChunk program_tables;
    void *resumer_frames;
    DiamondFiber *resumer_fiber;
} DiamondFiber;

typedef enum DiamondFiberStatus : uint8_t {
    DIAMOND_FIBER_OK,
    DIAMOND_FIBER_INVALID_STATE,
} DiamondFiberStatus;

typedef struct DiamondFiberQueue {
    DiamondFiber **items;
    size_t count;
    size_t capacity;
    size_t head;
} DiamondFiberQueue;

struct DiamondVm {
    /* The nursery -- every allocate_* helper always links a fresh object
     * here (never onto old_objects directly). bytes_allocated/next_gc
     * are unchanged in meaning from the pre-generational collector:
     * total live bytes across *both* generations, driving the existing
     * doubling major-collection threshold (major collection walks both
     * lists, exactly as before this field split). */
    DiamondObject *young_objects;
    DiamondObject *old_objects;
    size_t bytes_allocated;
    size_t next_gc;
    /* Nursery threshold: minor collection triggers once bytes_allocated
     * has grown by at least this many bytes since the last minor-or-
     * major collection (a snapshot/delta, not a separately-incremented
     * running counter -- every allocate_* site already bumps
     * bytes_allocated, and nothing but allocation ever *grows* it
     * between collections, so the delta against a snapshot recorded at
     * the end of the last collection is exactly "how much new stuff was
     * allocated since then," with no need to touch every allocation
     * site a second time). Fixed, not doubling like next_gc -- a
     * nursery should stay small and cheap to keep minor collections
     * frequent, unlike the major threshold, which deliberately grows
     * with the live set to avoid re-collecting a large heap too often.
     * 1MiB, tuned against bench/gc_churn's session_churn.di after card
     * marking landed: 64KiB (the pre-card-marking placeholder) triggered
     * a minor collection roughly every 4 iterations of that benchmark's
     * workload, which dominated wall time even with cheap per-collection
     * cost; 1MiB cut total minor-collection time by two orders of
     * magnitude with no further gain from going to 4MiB. See
     * bench/gc_churn/README.md's own recorded sweep for the numbers. */
    size_t bytes_allocated_at_last_minor_gc;
    size_t minor_gc_threshold_bytes;
    /* Every old object with at least one recorded old->young pointer
     * (DiamondObject.remembered tracks membership to avoid duplicates --
     * see that field's own comment). A minor collection's root set is
     * the ordinary root walk *plus* every entry here; see
     * gc_write_barrier and diamond_vm_collect_minor (src/vm.c). Grows
     * like any other dynamic array in this codebase (gc_protected is
     * the closest existing precedent) -- entries are removed only when
     * their own object dies (filtered by `marked` during a major
     * collection's sweep, *before* sweeping -- see that function's own
     * comment on why the ordering matters), never just because nothing
     * young is reachable through them *right now*: an old object that
     * received an old->young write once might still transitively reach
     * a young object through an unmarked-dirty part of itself, and
     * re-deriving that safely is exactly what staying in the remembered
     * set for the object's whole lifetime avoids needing to reason
     * about. */
    DiamondObject **remembered_set;
    size_t remembered_count;
    size_t remembered_capacity;
    void *frames;
    bool stress_gc;
    /* DIAMOND_STRESS_MINOR_GC (src/run_source.c) -- forces a minor
     * collection before every eligible allocation, the nursery-scoped
     * counterpart to stress_gc above. Exists specifically to catch
     * missing write-barrier sites: a young object reachable only
     * through an old object's own unrecorded pointer looks fine under
     * infrequent, ordinary collection timing and only reliably breaks
     * under aggressive minor-collection pressure. */
    bool stress_minor_gc;
    /* Set once, in diamond_vm_run, to whichever chunk this vm was first
     * invoked with -- the "home" chunk for every ordinary instance this
     * vm ever allocates (DiamondInstance.owner==nullptr means "belongs to
     * root_chunk", not "belongs to whatever chunk happens to be
     * executing right now"). Before ClassName.compile_method existed,
     * those two things were always the same chunk for the vm's entire
     * run, so nothing needed this field: the ambient `chunk` parameter
     * threaded through run_chunk's recursion was already an equally
     * correct stand-in. compile_method changed that -- a method
     * installed from it runs with a *different*, foreign chunk as its
     * own ambient `chunk` (its own bytecode's class/function-index
     * operands are only meaningful there), so a nested self.foo() call
     * from inside that method's body, against a receiver whose real
     * class lives in root_chunk, must fall back to root_chunk, not the
     * foreign chunk that merely happens to be executing at that moment.
     * See the DIAMOND_OP_INVOKE_TYPED-family dispatch sites (src/vm.c)
     * that read this. */
    const DiamondChunk *root_chunk;
    /* Direct GC-cost evidence (DIAMOND_TRACE_GC, src/run_source.c) --
     * collection count and total wall time spent inside
     * diamond_vm_collect/diamond_vm_collect_minor, timed via
     * CLOCK_MONOTONIC, split by generation (this is exactly the
     * distinction bench/gc_churn's own measurements needed to show the
     * generational collector's actual cost profile -- many cheap minor
     * collections against few expensive major ones). Added so a future
     * investigation of docs/roadmap.md's "Generational or incremental
     * GC" item can measure real-walk cost directly instead of inferring
     * it from external RSS sampling under live network load, which
     * conflates request-handling timing, OS scheduling, and page-cache
     * behavior with the collector's own cost (see bench/burn_in's
     * "A follow-up push" section for the investigation this was built
     * for). Only observable today via the normal print-at-process-exit
     * path an ordinary (non-daemon) Diamond program takes -- a long-
     * running server killed by signal (gremlin_serve, as bench/burn_in
     * runs it) never reaches that path, so these counters aren't yet
     * wired up to anything a live burn-in run can read; that's a
     * separate, not-yet-designed follow-up, not something this addition
     * attempts. */
    size_t gc_major_collection_count;
    double gc_major_total_seconds;
    size_t gc_minor_collection_count;
    double gc_minor_total_seconds;
    DiamondMethodCache method_caches[DIAMOND_INLINE_CACHE_COUNT];
    DiamondFieldCache field_caches[DIAMOND_INLINE_CACHE_COUNT];
    size_t inline_cache_hits;
    size_t inline_cache_misses;
    size_t monomorphic_dispatches;
    size_t method_cache_probes;
    size_t monomorphic_threshold;
    size_t direct_dispatch_rewrites;
    const uint8_t **rewritten_sites;
    size_t rewritten_site_count;
    size_t rewritten_site_capacity;
    size_t field_cache_hits;
    size_t field_cache_misses;
    size_t shape_transitions;
    size_t opcode_counts[DIAMOND_OP_COUNT];
    bool quickening;
    size_t quickening_threshold;
    size_t quickening_observations;
    size_t quickened_sites;
    size_t deoptimized_sites;
    /* Copied from the top-level DiamondChunk's own field once, at
     * diamond_vm_run's own entry -- NOT re-read from whatever
     * DiamondChunk happens to be ambient at a given opcode, since a
     * nested function/closure call constructs its *own* fresh
     * DiamondChunk view at each call site (many places in this file),
     * none of which carry a program-wide field like this one along
     * unless it's threaded through some other way. Living on DiamondVm
     * instead sidesteps needing to touch every one of those call sites:
     * the underlying classes[] array itself is the same memory across
     * every such view in the common (single-program, non-ProgramBuilder-
     * adopted) case, so `&chunk->classes[vm->range_class_index]` stays
     * correct however deep in a nested call this is read. See
     * DiamondChunk's own copy of this comment (this field exists so
     * DIAMOND_OP_INDEX_GET/SET can recognize a Range instance -- lib/
     * core.di, no native VM value kind for Range at all -- without a
     * hardcoded class index, confirmed directly that one can shift with
     * conservative prelude module selection). */
    uint8_t range_class_index;
    DiamondValue namespace_constants[DIAMOND_MAX_NAMESPACE_CONSTANTS];
    bool namespace_constant_initialized[DIAMOND_MAX_NAMESPACE_CONSTANTS];
    /* Class variable values, indexed as class_variables[class_index *
     * DIAMOND_MAX_FIELDS + slot], slot assigned exactly as DiamondClass's
     * own class_variables[]/class_variable_count name table does at
     * compile time -- see that field's own comment. Per-VM, not
     * per-DiamondProgram: every spawned Thread's fresh diamond_vm_init
     * gets its own nullptr copy, so `threads: N` gives each worker its
     * own independent class variables, consistent with every other piece
     * of state a spawned Thread never shares (see docs/threads.md).
     * A pointer, allocated lazily (get_cvar_helper/set_cvar_helper,
     * src/vm.c) on first actual GET_CVAR/SET_CVAR rather than embedded
     * inline here, deliberately: DIAMOND_MAX_CLASSES * DIAMOND_MAX_FIELDS
     * DiamondValues is 128KB, and DiamondVm is stack-allocated at the top
     * of every entry point (run_source.c, repl.c) underneath run_chunk's
     * entire recursive call chain -- confirmed the hard way that even
     * this struct's own *one-time* size growth (not anything per
     * recursive frame) was enough to turn DIAMOND_MAX_CALL_DEPTH's clean
     * "call stack overflow" guard into a real ASan-caught stack overflow,
     * because that 128KB comes out of the same stack budget the
     * recursion depth guard's whole margin depends on. Lazy allocation
     * means a program that never touches a class variable -- the common
     * case -- pays nothing at all, and one that does pays a single
     * ordinary heap allocation, checked for failure exactly like
     * allocate_hash/allocate_array already are, not a stack cost. Unlike
     * namespace_constants, ordinary mutable storage -- no write-once
     * guard, defaults to nil (DIAMOND_VALUE_NIL == 0, so calloc already
     * gives every slot the right default) until first assigned. */
    DiamondValue *class_variables;
    DiamondValue exception;
    bool has_exception;
    char error[1024];
    const DiamondFiberQueue *root_queue;
    DiamondFiber *running_fiber;
    /* Signal.trap(name, handler) stores handler here, indexed the same
     * way as the file-scope diamond_signal_numbers/diamond_signal_names
     * tables in vm.c. Zero-initialized to DIAMOND_NIL (kind 0) by
     * diamond_vm_init's own compound-literal init, same as every other
     * DiamondValue field here -- no explicit "unset" flag needed, a
     * closure check at dispatch time is enough. Marked as a GC root in
     * diamond_vm_collect alongside exception/namespace_constants; without
     * that, a handler with no other live reference (the common case --
     * nothing about "keep this Callable around forever in case a signal
     * arrives" fits any existing local variable's lifetime) would be
     * collected the moment nothing else referenced it. */
    DiamondValue trapped_signal_handlers[DIAMOND_SIGNAL_COUNT];
    /* Singly linked list of DiamondAdoptedProgram nodes (private type,
     * src/vm.c), same opaque-pointer-in-the-public-header pattern as
     * `frames` above. Populated only when an Instance result crosses a
     * ProgramBuilder#run boundary into this vm (copy_value_into_vm):
     * rather than deep-copying the source DiamondProgram's classes and
     * their bytecode (which would need every function/class index
     * embedded in that bytecode remapped), the whole source program is
     * kept alive here instead, and the copied instance's own `owner`
     * field (DiamondInstance, src/object.h) points at the adopted
     * program's chunk. Freed alongside every adopted program in
     * diamond_vm_free -- never reclaimed earlier, since a value holding
     * a reference into an adopted program can outlive the call that
     * created it by an arbitrary amount, and nothing here reference-
     * counts across the two heaps to know when the last one is gone. */
    void *adopted_programs;
    /* ARGV/ENV (see docs/syntax.md) -- real Array/Hash values, not
     * lazily nil like trapped_signal_handlers above, since
     * DIAMOND_OP_ARGV/DIAMOND_OP_ENV just read these directly with no
     * fallback check. diamond_vm_init gives every VM (including a
     * spawned Thread's own child_vm and ProgramBuilder#run's internal
     * VM) a real, empty ARGV and a real, populated ENV by default --
     * environment variables are process-wide and universally useful,
     * but a script's own trailing command-line arguments only make
     * sense for the actual top-level script invocation, so ARGV only
     * becomes non-empty when diamond_vm_set_argv (called from
     * src/run_source.c) is used. */
    DiamondValue argv_value;
    DiamondValue env_value;
    /* GC root stack for values under construction outside of any register
     * -- copy_value_into_vm (src/vm.c) is the one user: it deep-copies a
     * value across a VM boundary (ProgramBuilder#run's result, Thread
     * arguments/join results) by recursively calling allocate_string/
     * allocate_array/etc, and unlike ordinary bytecode execution has no
     * destination register to root the value-in-progress through while
     * sibling elements are still being copied. A freshly allocated object
     * with no GC root is invisible to diamond_vm_collect_impl's mark
     * phase, so without this, a GC triggered by copying one array element
     * could free an already-copied sibling still sitting in a plain C
     * local or an unscanned buffer -- exactly the bug this fixed in
     * regexp_scan_helper/regexp_match_helper, but unreachable there since
     * both of those had a real destination register to root through
     * immediately (see docs/roadmap.md). Push/pop discipline only (see
     * gc_protect/gc_unprotect) -- always unwound back to a saved mark
     * before the pushing function returns, mirroring the strictly nested
     * lifetime of copy_value_into_vm's own recursion, so this never grows
     * across separate top-level calls. */
    DiamondValue *gc_protected;
    size_t gc_protected_count;
    size_t gc_protected_capacity;
    /* DIAMOND_DEBUG_FD (see docs/debugging.md), read once here by
     * diamond_vm_init rather than re-reading getenv on every
     * DIAMOND_OP_DEBUGGER pause -- -1 (the ordinary case: no DAP client
     * attached) means debugger_helper (src/vm.c) takes its original
     * print-to-stdout/getchar() path unchanged; a value >=0 is an
     * already-open fd (a dedicated pipe end wired up by whatever spawned
     * this process, e.g. dap/main.c's launch handling) that
     * debugger_helper instead writes a Content-Length-framed JSON pause
     * payload to and blocks reading one framed command back from. */
    int debug_fd;
    /* Extra GC roots beyond every field mark_roots (src/vm.c) already
     * walks -- null/0 (its zero-init default) for every ordinary VM.
     * The one user is a Channel's own private DiamondVm (see docs/
     * threads.md's Channels section, DiamondChannel in src/vm.c): that
     * VM never runs bytecode of its own (no frames, no running_fiber),
     * it exists purely as GC-managed storage for values queued between
     * send and receive, so its own queue array (not a field of DiamondVm
     * itself) is the only root set it has. Set to point at the
     * channel's own flat DiamondValue queue buffer, with
     * extra_root_count updated to the current queued-item count
     * immediately before any allocation that could trigger a collection
     * (send/receive already hold the channel's own mutex at that point,
     * so this is never read concurrently with a write). */
    DiamondValue *extra_roots;
    size_t extra_root_count;
};

void diamond_vm_init(DiamondVm *vm);
void diamond_vm_free(DiamondVm *vm);
void diamond_vm_collect(DiamondVm *vm);
void diamond_vm_collect_minor(DiamondVm *vm);
/* The collection-trigger check every allocate_* helper makes before
 * actually allocating (src/vm.c) -- declared here, not static, so
 * src/bignum.c's own bignum_alloc (a separate translation unit) can
 * share it too, instead of keeping a second copy of the same check out
 * of sync. */
void maybe_collect(DiamondVm *vm);
void diamond_vm_invalidate_method_caches(DiamondVm *vm);
void diamond_vm_bind_fiber_queue(DiamondVm *vm, const DiamondFiberQueue *queue);
void diamond_vm_set_argv(DiamondVm *vm, int argc, char *const *argv);
DiamondFiber *diamond_fiber_new(const DiamondChunk *chunk);
void diamond_fiber_free(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *fiber, DiamondVm *vm);
DiamondFiberStatus diamond_fiber_run(DiamondFiber *fiber);
DiamondValue diamond_fiber_result(const DiamondFiber *fiber);
DiamondVmStatus diamond_fiber_status(const DiamondFiber *fiber);
bool diamond_fiber_resumable(const DiamondFiber *fiber);
const char *diamond_fiber_state_name(DiamondFiberState state);
DiamondFiberStatus diamond_fiber_make_runnable(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_begin(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_resume(DiamondFiber *fiber, DiamondValue value);
DiamondFiberStatus diamond_fiber_suspend(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_yield(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_complete(DiamondFiber *fiber, DiamondValue result);
DiamondFiberStatus diamond_fiber_fail(DiamondFiber *fiber, DiamondVmStatus status);
void diamond_fiber_queue_init(DiamondFiberQueue *queue);
void diamond_fiber_queue_free(DiamondFiberQueue *queue);
bool diamond_fiber_queue_push(DiamondFiberQueue *queue, DiamondFiber *fiber);
DiamondFiber *diamond_fiber_queue_pop(DiamondFiberQueue *queue);
size_t diamond_fiber_queue_count(const DiamondFiberQueue *queue);
DiamondFiber *diamond_fiber_queue_at(const DiamondFiberQueue *queue, size_t index);
DiamondFiber *diamond_fiber_scheduler_step(DiamondFiberQueue *queue);
bool diamond_fiber_scheduler_requeue(DiamondFiberQueue *queue, DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_scheduler_run_once(DiamondFiberQueue *queue);
DiamondFiberStatus diamond_fiber_scheduler_run_all(DiamondFiberQueue *queue);
DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result);
const char *diamond_vm_status_name(DiamondVmStatus status);
const char *diamond_vm_error(const DiamondVm *vm);
bool diamond_native_method_satisfies(uint8_t receiver_type,const char *name,
                                     uint8_t arity,uint8_t *return_type);

#endif
