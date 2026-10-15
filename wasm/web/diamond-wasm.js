// Diamond in the browser: loads build/wasm/diamond.wasm (wasm/build.sh) and drives it as a REPL.
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

// A minimal wasi_snapshot_preview1: stdout/stderr capture and nothing else. Everything that would
// touch the host (files, sockets, subprocesses) reports "not supported".
function makeWasi(state) {
  const ENOSYS = 52, EBADF = 8, ESPIPE = 70, SUCCESS = 0;
  const decoder = new TextDecoder();
  const view = () => new DataView(state.memory.buffer);
  const unsupported = () => ENOSYS;
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
    fd_read: () => EBADF,
    fd_pread: () => EBADF,
    fd_close: () => SUCCESS,
    fd_sync: () => SUCCESS,
    fd_seek: () => ESPIPE,
    fd_fdstat_get(fd, out) {
      const v = view();
      v.setUint8(out, fd <= 2 ? 2 : 0);               // filetype: character device
      v.setUint16(out + 2, 0, true);                   // flags
      v.setBigUint64(out + 8, 0xffffffffffffffffn, true);
      v.setBigUint64(out + 16, 0xffffffffffffffffn, true);
      return fd <= 2 ? SUCCESS : EBADF;
    },
    fd_fdstat_set_flags: unsupported,
    fd_filestat_get: () => EBADF,
    fd_prestat_get: () => EBADF,
    fd_prestat_dir_name: () => EBADF,
    fd_readdir: () => EBADF,
    path_open: unsupported, path_filestat_get: unsupported, path_link: unsupported,
    path_readlink: unsupported, path_remove_directory: unsupported, path_rename: unsupported,
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

export class DiamondRepl {
  #module; #instance; #state;
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
    const state = { memory: null, stdout: '', stderr: '' };
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
    return /^\s*(def|class|module|interface|struct)\s/.test(text);
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

  get prompt() { return this.#pending ? '... ' : '{> '; }

  reset() { this.#session = '_ = nil\n'; this.#pending = ''; this.#previousOutput = ''; }

  // Feeds one line. Returns { kind: 'continue' } while an entry is unfinished, else
  // { kind: 'ok' | 'error', text }.
  submit(line) {
    this.#pending += line + '\n';
    const wrapped = DiamondRepl.#wrap(this.#pending);
    const candidate = this.#session + wrapped;
    const attempt = this.run(candidate);
    if (attempt.status === 65) {
      if (DiamondRepl.#incomplete(attempt.error, candidate)) return { kind: 'continue' };
      if (!DiamondRepl.#isDeclaration(this.#pending)) {
        const raw = this.#session + this.#pending;
        const rawAttempt = this.run(raw);
        if (rawAttempt.status === 65 && DiamondRepl.#incomplete(rawAttempt.error, raw))
          return { kind: 'continue' };
      }
      this.#pending = '';
      return { kind: 'error', text: attempt.error.replace(/^\d+:\d+: /, '') };
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
