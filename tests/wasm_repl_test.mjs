// Drives build/wasm/diamond.wasm through the same module the site uses (wasm/web/diamond-wasm.js)
// and checks the REPL behaves: results, session state, continuation lines, ordinary errors for
// the features the browser build lacks, and known answers for the portable crypto.
// Build first: WASI_SDK=/path/to/wasi-sdk wasm/build.sh
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { DiamondRepl } from '../wasm/web/diamond-wasm.js';

const wasm = fileURLToPath(new URL('../build/wasm/diamond.wasm', import.meta.url));
const repl = await DiamondRepl.load(await readFile(wasm));
let failures = 0;

function feed(lines) {
  let last = { kind: 'continue', text: '' };
  for (const line of lines) last = repl.submit(line);
  return last;
}
function expect(name, lines, check) {
  repl.reset();
  const result = feed(Array.isArray(lines) ? lines : [lines]);
  const ok = check(result);
  if (!ok) { failures++; console.error(`FAIL ${name}: ${JSON.stringify(result)}`); }
  return ok;
}
const equals = (text) => (r) => r.kind === 'ok' && r.text === text;
const errorContaining = (fragment) => (r) => r.kind === 'error' && r.text.includes(fragment);

expect('arithmetic', '1 + 2', equals('3'));
expect('assignment persists', ['x = 40', 'x + 2'], equals('42'));
expect('puts output then value', 'puts("hi")', equals('hi\nnil'));
expect('multi-line def', ['def sq(n)', '  n * n', 'end', 'sq(12)'], equals('144'));
expect('class', ['class Dog', '  def speak() = "woof"', 'end', 'Dog.new().speak()'], equals('woof'));
expect('block', ['[1, 2, 3].map() do |v|', '  v * 10', 'end'], equals('[10, 20, 30]'));
expect('struct', ['struct P(x: Int, y: Int)', 'end', 'P.new(1, 2) == P.new(1, 2)'], equals('true'));
expect('syntax error is reported', 'nope()', errorContaining('undefined function'));
expect('unfinished input continues', 'def f(', (r) => r.kind === 'continue');
expect('type annotation is checked', ['def d(n: Int) -> Int = n * 2', 'd("x")'],
  errorContaining('expected Int'));

expect('sha256', 'Digest.sha256("abc")',
  equals('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'));
expect('sha1', 'Digest.sha1("abc")', equals('a9993e364706816aba3e25717850c26c9cd0d89d'));
expect('hmac', 'HMAC.sha256("Jefe", "what do ya want for nothing?")',
  equals('5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843'));
expect('base64', 'Base64.encode("hello")', equals('aGVsbG8='));
expect('base64 decode', 'Base64.decode("aGVsbG8=")', equals('hello'));
expect('secure random', 'SecureRandom.hex(8).length()', equals('16'));

for (const [name, snippet, fragment] of [
  ['postgres', 'PostgreSQL.open("host=x")', 'not compiled into this build'],
  ['mysql', 'MySQL.open("h", "u", "p", "d", 3306)', 'not compiled into this build'],
  ['gzip', 'Gzip.compress("x")', 'not compiled into this build'],
  ['cipher', 'Cipher.encrypt("01234567890123456789012345678901", "x")', 'not compiled into this build'],
  ['tls', 'TLSServer.listen(8443, "c", "k")', 'not available in the browser'],
  ['tcp', 'TCPSocket.connect("example.com", 80)', 'not available in the browser'],
  ['process', 'Process.run(["ls"])', 'Not supported'],
]) expect(`${name} fails as an ordinary error`, snippet, (r) =>
  r.kind === 'error' && r.text.includes(fragment));

// SQLite runs in the module: in-memory databases only.
expect('sqlite in memory', [
  'db = SQLite3.open(":memory:")',
  'db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")',
  'db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 36])',
  'db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Grace", 45])',
  'db.query("SELECT name FROM people ORDER BY age DESC")',
], equals('[{name: Grace}, {name: Ada}]'));
expect('sqlite aggregate', [
  'db = SQLite3.open(":memory:")',
  'db.execute("CREATE TABLE n (v INTEGER)")',
  'db.execute("INSERT INTO n VALUES (1), (2), (3)")',
  'db.query("SELECT sum(v) AS total FROM n")',
], equals('[{total: 6}]'));
expect('sqlite file is refused cleanly', 'SQLite3.open("/tmp/diamond-test.db")',
  errorContaining('cannot open'));

// Completion: the engine behind the terminal REPL's Tab key, driven through the page's API.
function completes(name, setup, line, check) {
  repl.reset();
  feed(setup);
  const result = repl.complete(line);
  if (!check(result)) { failures++; console.error(`FAIL ${name}: ${JSON.stringify(result)}`); }
}
completes('completes a method after a dot', ['words = ["a", "b"]'], 'words.ma', (r) =>
  r.prefix === 'ma' && r.labels.includes('map') && r.labels.every((label) => label.startsWith('ma')));
completes('only the receiver\'s members follow a dot, each once', ['def squared(n) = n * n', 'words = ["a"]'],
  'words.', (r) => r.labels.includes('map') && !r.labels.includes('squared') && !r.labels.includes('words')
    && new Set(r.labels).size === r.labels.length);
completes('completes a name defined earlier', ['def squared(n) = n * n'], 'squ', (r) =>
  r.prefix === 'squ' && r.labels.includes('squared'));
completes('completes a name from an unfinished entry', ['class Dog', '  def speak() = 1'], '  def x() = spe',
  (r) => Array.isArray(r.labels));
completes('no candidates for a name nothing starts with', [], 'qqqzzz', (r) => r.labels.length === 0);
completes('a dangling operator is not an error', [], '1 +', (r) => Array.isArray(r.labels));
expect('still evaluates after completing', ['x = 20', 'x * 2'], equals('40'));

// The session survives every failure above.
expect('still alive', ['TCPSocket.connect("example.com", 80)', '1 + 1'], equals('2'));

if (failures > 0) { console.error(`${failures} failure(s)`); process.exit(1); }
console.log('wasm repl test passed');
