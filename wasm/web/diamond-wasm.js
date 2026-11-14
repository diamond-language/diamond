// d6 in the browser: loads build/wasm/diamond.wasm (wasm/build.sh) and drives it as a REPL.
//
// The module exports diamond_wasm_eval(source), which compiles and runs a complete program in a
// fresh VM and prints the program's output, then 0x1E and the result value, to stdout (errors go
// to stderr). The REPL session is therefore text kept here: every submitted line re-runs the
// accepted session plus the new line, and only the output beyond what the previous run already
// printed is shown -- the same replay model as the terminal REPL (src/repl.c).
//
// Works in browsers and in Node (>= 18): it uses no DOM and only a tiny WASI surface.

const RECORD_SEPARATOR = '\x1e';

export class NativeUnavailable extends Error {}

// Resolves `path` against `base` into a normalized absolute path ('/a/b'), or null if it climbs
// above the root.
function resolvePath(base, path) {
  const parts = path.startsWith('/') ? [] : base.split('/').filter(Boolean);
  for (const part of path.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..') { if (parts.length === 0) return null; parts.pop(); } else parts.push(part);
  }
  return '/' + parts.join('/');
}

// A minimal wasi_snapshot_preview1: stdout/stderr capture, plus a read-only filesystem holding
// the files the page added (DiamondRepl.addFile), preopened at '/' as fd 3, so `require` and
// `require_cut` can load them. Writes, sockets and subprocesses report "not supported".
function makeWasi(state) {
  const ENOSYS = 52, EBADF = 8, ESPIPE = 70, SUCCESS = 0;
  const ENOENT = 44, EISDIR = 31, ENOTDIR = 54, EINVAL = 28, EROFS = 69;
  const FILETYPE_CHARACTER_DEVICE = 2, FILETYPE_DIRECTORY = 3, FILETYPE_REGULAR = 4;
  const decoder = new TextDecoder();
  const view = () => new DataView(state.memory.buffer);
  const unsupported = () => ENOSYS;
  const readString = (pointer, length) =>
    decoder.decode(new Uint8Array(state.memory.buffer, pointer, length));
  // fd -> { path, directory, data, position }; 3 is the preopened root.
  const open = new Map([[3, { path: '/', directory: true }]]);
  let nextFd = 4;
  const isDirectory = (path) => {
    if (path === '/') return true;
    const prefix = path + '/';
    for (const name of state.files.keys()) if (name.startsWith(prefix)) return true;
    return false;
  };
  const lookup = (dirFd, pointer, length) => {
    const directory = open.get(dirFd);
    if (directory === undefined || !directory.directory) return { error: EBADF };
    const path = resolvePath(directory.path, readString(pointer, length));
    if (path === null) return { error: ENOENT };
    if (state.files.has(path)) return { path, data: state.files.get(path) };
    if (isDirectory(path)) return { path, directory: true };
    return { error: ENOENT };
  };
  const writeFilestat = (out, entry) => {
    const v = view();
    for (let offset = 0; offset < 64; offset += 8) v.setBigUint64(out + offset, 0n, true);
    v.setUint8(out + 16, entry.directory ? FILETYPE_DIRECTORY : FILETYPE_REGULAR);
    v.setBigUint64(out + 24, 1n, true);
    v.setBigUint64(out + 32, BigInt(entry.directory ? 0 : entry.data.length), true);
  };
  return {
    args_get: () => SUCCESS,
    args_sizes_get(argc, size) { view().setUint32(argc, 0, true); view().setUint32(size, 0, true); return SUCCESS; },
    environ_get: () => SUCCESS,
    environ_sizes_get(count, size) { view().setUint32(count, 0, true); view().setUint32(size, 0, true); return SUCCESS; },
    clock_res_get(id, out) { view().setBigUint64(out, 1000n, true); return SUCCESS; },
    clock_time_get(id, precision, out) {
      const nanoseconds = id === 0
        ? BigInt(Date.now()) * 1000000n
        : BigInt(Math.round(performance.now() * 1e6));
      view().setBigUint64(out, nanoseconds, true);
      return SUCCESS;
    },
    fd_write(fd, iovs, count, written) {
      if (fd !== 1 && fd !== 2) return EBADF;
      const v = view();
      let total = 0, text = '';
      for (let i = 0; i < count; i++) {
        const pointer = v.getUint32(iovs + i * 8, true), length = v.getUint32(iovs + i * 8 + 4, true);
        text += decoder.decode(new Uint8Array(state.memory.buffer, pointer, length), { stream: true });
        total += length;
      }
      if (fd === 1) state.stdout += text; else state.stderr += text;
      v.setUint32(written, total, true);
      return SUCCESS;
    },
    fd_read(fd, iovs, count, read) {
      const file = open.get(fd);
      if (file === undefined) return EBADF;
      if (file.directory) return EISDIR;
      const v = view();
      let total = 0;
      for (let i = 0; i < count; i++) {
        const pointer = v.getUint32(iovs + i * 8, true), length = v.getUint32(iovs + i * 8 + 4, true);
        const chunk = file.data.subarray(file.position, file.position + length);
        new Uint8Array(state.memory.buffer, pointer, chunk.length).set(chunk);
        file.position += chunk.length;
        total += chunk.length;
        if (chunk.length < length) break;
      }
      v.setUint32(read, total, true);
      return SUCCESS;
    },
    fd_pread: () => EBADF,
    fd_close(fd) { if (fd > 3) open.delete(fd); return SUCCESS; },
    fd_sync: () => SUCCESS,
    fd_seek(fd, offset, whence, out) {
      const file = open.get(fd);
      if (file === undefined) return fd <= 2 ? ESPIPE : EBADF;
      if (file.directory) return EISDIR;
      const base = whence === 0 ? 0 : whence === 1 ? file.position : file.data.length;
      const position = base + Number(offset);
      if (position < 0) return EINVAL;
      file.position = position;
      view().setBigUint64(out, BigInt(position), true);
      return SUCCESS;
    },
    fd_fdstat_get(fd, out) {
      const v = view();
      const file = open.get(fd);
      if (fd > 2 && file === undefined) return EBADF;
      v.setUint8(out, fd <= 2 ? FILETYPE_CHARACTER_DEVICE
        : file.directory ? FILETYPE_DIRECTORY : FILETYPE_REGULAR);
      v.setUint16(out + 2, 0, true);                   // flags
      v.setBigUint64(out + 8, 0xffffffffffffffffn, true);
      v.setBigUint64(out + 16, 0xffffffffffffffffn, true);
      return SUCCESS;
    },
    fd_fdstat_set_flags: unsupported,
    fd_filestat_get(fd, out) {
      const file = open.get(fd);
      if (file === undefined) return EBADF;
      writeFilestat(out, file);
      return SUCCESS;
    },
    fd_prestat_get(fd, out) {
      if (fd !== 3) return EBADF;
      view().setUint8(out, 0);                         // a directory
      view().setUint32(out + 4, 1, true);              // name length: '/'
      return SUCCESS;
    },
    fd_prestat_dir_name(fd, pointer, length) {
      if (fd !== 3 || length < 1) return EBADF;
      new Uint8Array(state.memory.buffer, pointer, 1)[0] = 0x2f;
      return SUCCESS;
    },
    fd_readdir: () => ENOSYS,
    path_open(dirFd, lookupFlags, pointer, length, openFlags, rightsBase, rightsInheriting, fdFlags, out) {
      if (openFlags & (1 | 4 | 8)) return EROFS;      // O_CREAT, O_EXCL, O_TRUNC
      if (rightsBase & 0x40n) return EROFS;            // fd_write
      const entry = lookup(dirFd, pointer, length);
      if (entry.error !== undefined) return entry.error;
      if ((openFlags & 2) && !entry.directory) return ENOTDIR;
      const fd = nextFd++;
      open.set(fd, { ...entry, position: 0 });
      view().setUint32(out, fd, true);
      return SUCCESS;
    },
    path_filestat_get(dirFd, lookupFlags, pointer, length, out) {
      const entry = lookup(dirFd, pointer, length);
      if (entry.error !== undefined) return entry.error;
      writeFilestat(out, entry);
      return SUCCESS;
    },
    // Nothing here is a symlink: EINVAL for a path that exists, ENOENT otherwise (realpath
    // relies on the difference).
    path_readlink(dirFd, pointer, length) {
      const entry = lookup(dirFd, pointer, length);
      return entry.error !== undefined ? entry.error : EINVAL;
    },
    path_link: unsupported, path_create_directory: unsupported,
    path_remove_directory: unsupported, path_rename: unsupported,
    path_unlink_file: unsupported, poll_oneoff: unsupported, sock_accept: unsupported,
    sock_shutdown: unsupported, sock_recv: unsupported, sock_send: unsupported,
    random_get(pointer, length) {
      const bytes = new Uint8Array(state.memory.buffer, pointer, length);
      for (let offset = 0; offset < length; offset += 65536)
        crypto.getRandomValues(bytes.subarray(offset, Math.min(length, offset + 65536)));
      return SUCCESS;
    },
    proc_exit(code) { throw new Error(`diamond exited with status ${code}`); },
    sched_yield: () => SUCCESS,
  };
}

// Indexed by the class numbers diamond_wasm_tokens returns (wasm/repl_driver.c).
const HIGHLIGHT_CLASSES = ['', 'tok-kw', 'tok-str', 'tok-num', 'tok-fn', 'tok-const', 'tok-ivar', 'tok-sym', 'tok-str'];

export class DiamondRepl {
  #module; #instance; #state;
  #files = new Map();
  #session = '_ = nil\n';
  #pending = '';
  #previousOutput = '';

  static async load(source) {
    const repl = new DiamondRepl();
    if (source instanceof WebAssembly.Module) repl.#module = source;
    else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) repl.#module = await WebAssembly.compile(source);
    else {
      // A server that does not send application/wasm makes compileStreaming throw; fetch the
      // bytes and compile them instead.
      const response = await fetch(source);
      try {
        repl.#module = await WebAssembly.compileStreaming(response.clone());
      } catch {
        repl.#module = await WebAssembly.compile(await response.arrayBuffer());
      }
    }
    repl.#instantiate();
    return repl;
  }

  #instantiate() {
    const state = { memory: null, stdout: '', stderr: '', files: this.#files };
    // Native features the browser build does not have (TLS, databases, sockets, subprocesses)
    // are imports that throw; evaluate() turns that into an ordinary message.
    const env = {};
    for (const entry of WebAssembly.Module.imports(this.#module))
      if (entry.module === 'env')
        env[entry.name] = () => { throw new NativeUnavailable(entry.name); };
    this.#instance = new WebAssembly.Instance(this.#module, {
      wasi_snapshot_preview1: makeWasi(state), env,
    });
    state.memory = this.#instance.exports.memory;
    this.#instance.exports._initialize?.();
    this.#state = state;
  }

  // Runs one complete program. status: 0 ok, 65 compile error, 70 runtime error.
  run(source) {
    const { exports } = this.#instance;
    this.#state.stdout = ''; this.#state.stderr = '';
    const bytes = new TextEncoder().encode(source + '\0');
    const pointer = exports.diamond_wasm_alloc(bytes.length);
    new Uint8Array(exports.memory.buffer, pointer, bytes.length).set(bytes);
    let status;
    try {
      status = exports.diamond_wasm_eval(pointer);
    } catch (error) {
      if (!(error instanceof NativeUnavailable)) { this.#instantiate(); throw error; }
      const feature = error.message;
      this.#instantiate();                 // the trapped instance is unusable
      return { status: 70, output: '', value: '', error: `runtime error: ${feature} is not available in the browser` };
    }
    exports.diamond_wasm_free(pointer);
    const stdout = this.#state.stdout;
    const split = stdout.lastIndexOf(RECORD_SEPARATOR);
    return {
      status,
      output: split >= 0 ? stdout.slice(0, split) : stdout,
      value: split >= 0 ? stdout.slice(split + 1).replace(/\n$/, '') : '',
      error: this.#state.stderr.replace(/\n$/, ''),
    };
  }

  static #isDeclaration(text) {
    return /^\s*(def|class|module|interface|struct|sealed)\s/.test(text) || /^\s*require(_cut)?[ \t"]/.test(text);
  }

  // The expression/assignment wrapping src/repl.c applies to a pending entry.
  static #wrap(pending) {
    if (DiamondRepl.#isDeclaration(pending)) return pending;
    const assignment = /^[ \t]*([A-Za-z0-9_]+)[ \t]*=(?!=)/.exec(pending);
    if (assignment) return `${pending}_ = ${assignment[1]}\n`;
    return `_ = (${pending.replace(/[\r\n]+$/, '')})\n`;
  }

  static #incomplete(error, source) {
    const match = /^(\d+):\d+: error:/.exec(error);
    if (!match) return false;
    // A diagnostic past the last newline-terminated line is the parser running off the end of
    // the input: an unfinished entry. One on an existing line is a real error.
    const terminated = source.split('\n').length - 1;
    return Number(match[1]) > terminated;
  }

  // Splits source into [{ cls, text }] segments for colouring. The classes come from the
  // lexer inside the module (diamond_wasm_tokens); gaps starting with '#' are comments.
  highlight(source) {
    const { exports } = this.#instance;
    const bytes = new TextEncoder().encode(source + '\0');
    const pointer = exports.diamond_wasm_alloc(bytes.length);
    new Uint8Array(exports.memory.buffer, pointer, bytes.length).set(bytes);
    const table = exports.diamond_wasm_tokens(pointer);
    const count = table ? new Uint32Array(exports.memory.buffer, table, 1)[0] : 0;
    const entries = count ? Array.from(new Uint32Array(exports.memory.buffer, table + 4, count * 3)) : [];
    exports.diamond_wasm_free(pointer);
    const decoder = new TextDecoder();
    const slice = (from, to) => decoder.decode(bytes.subarray(from, to));
    const segments = [];
    const gap = (text) => {
      const hash = text.indexOf('#');
      if (hash < 0) { if (text) segments.push({ cls: '', text }); return; }
      if (hash) segments.push({ cls: '', text: text.slice(0, hash) });
      segments.push({ cls: 'tok-comment', text: text.slice(hash) });
    };
    const end = bytes.length - 1;
    let at = 0;
    for (let i = 0; i < entries.length; i += 3) {
      const [kind, start, length] = entries.slice(i, i + 3);
      if (start < at) continue;
      gap(slice(at, start));
      segments.push({ cls: HIGHLIGHT_CLASSES[kind] || '', text: slice(start, start + length) });
      at = start + length;
    }
    gap(slice(at, end));
    return segments;
  }

  // Completion candidates for `line` with the caret at character index `caret`, from the same
  // engine as the terminal REPL's Tab key (diamond_wasm_complete). Returns { prefix, labels }:
  // `prefix` is the identifier typed before the caret, and every label starts with it. An
  // unfinished entry's earlier lines count as context, as they do in the terminal.
  complete(line, caret = line.length) {
    const { exports } = this.#instance;
    const encoder = new TextEncoder();
    const prefix = /[A-Za-z0-9_]*$/.exec(line.slice(0, caret))[0];
    const put = (text) => {
      const bytes = encoder.encode(text + '\0');
      const pointer = exports.diamond_wasm_alloc(bytes.length);
      new Uint8Array(exports.memory.buffer, pointer, bytes.length).set(bytes);
      return pointer;
    };
    const pointers = [put(this.#session), put(this.#pending), put(line)];
    let text = '';
    try {
      const result = exports.diamond_wasm_complete(...pointers, encoder.encode(line.slice(0, caret)).length);
      if (result) {
        const memory = new Uint8Array(exports.memory.buffer);
        let end = result;
        while (memory[end]) end++;
        text = new TextDecoder().decode(memory.subarray(result, end));
      }
    } catch (error) {
      if (!(error instanceof NativeUnavailable)) this.#instantiate();
      throw error;
    } finally {
      for (const pointer of pointers) exports.diamond_wasm_free(pointer);
    }
    return { prefix, labels: text ? text.slice(0, -1).split('\n') : [] };
  }

  get prompt() { return this.#pending ? '... ' : '{> '; }

  reset() { this.#session = '_ = nil\n'; this.#pending = ''; this.#previousOutput = ''; }

  // Adds a read-only file programs can load: `require "./x"` resolves from '/', and
  // `require_cut "name"` finds '/cuts/name/lib/name.d6' (plus its Cutfile, if added). `text`
  // is a string or bytes; the files outlive reset().
  addFile(path, text) {
    const resolved = resolvePath('/', path);
    if (resolved === null || resolved === '/') throw new Error(`not a file path: ${path}`);
    this.#files.set(resolved, typeof text === 'string' ? new TextEncoder().encode(text) : new Uint8Array(text));
  }

  // Feeds one line. Returns { kind: 'continue' } while an entry is unfinished, else
  // { kind: 'ok' | 'error', text }.
  submit(line) {
    // A blank or comment-only line starts no entry: nothing to evaluate, so nothing to print
    // (src/repl.c does the same). Inside an unfinished entry it is ordinary input.
    if (!this.#pending && /^\s*(#.*)?$/.test(line)) return { kind: 'ok', text: '' };
    this.#pending += line + '\n';
    let candidate = this.#session + DiamondRepl.#wrap(this.#pending);
    let attempt = this.run(candidate);
    if (attempt.status === 65) {
      if (DiamondRepl.#incomplete(attempt.error, candidate)) return { kind: 'continue' };
      let block = null;
      if (!DiamondRepl.#isDeclaration(this.#pending)) {
        const raw = this.#session + this.#pending;
        const rawAttempt = this.run(raw);
        if (rawAttempt.status === 65 && DiamondRepl.#incomplete(rawAttempt.error, raw))
          return { kind: 'continue' };
        // A statement that only fails inside `_ = (...)`, such as a trailing `if` modifier,
        // runs as the body of `if true ... end` -- the same fallback src/repl.c uses.
        if (rawAttempt.status !== 65) {
          const body = `if true\n${this.#pending}end\n`;
          const blockCandidate = this.#session + DiamondRepl.#wrap(body);
          const blockAttempt = this.run(blockCandidate);
          if (blockAttempt.status !== 65) block = { candidate: blockCandidate, attempt: blockAttempt };
        }
      }
      if (block === null) {
        this.#pending = '';
        return { kind: 'error', text: attempt.error.replace(/^\d+:\d+: /, '') };
      }
      ({ candidate, attempt } = block);
    }
    if (attempt.status !== 0) {
      this.#pending = '';
      return { kind: 'error', text: attempt.error };
    }
    const fresh = attempt.output.startsWith(this.#previousOutput)
      ? attempt.output.slice(this.#previousOutput.length) : attempt.output;
    // A declaration's value is always nil: say nothing rather than print it.
    const declaration = DiamondRepl.#isDeclaration(this.#pending);
    this.#session = candidate;
    this.#previousOutput = attempt.output;
    this.#pending = '';
    return { kind: 'ok', text: declaration ? fresh : fresh + attempt.value };
  }
}
