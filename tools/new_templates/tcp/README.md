# {{name}}

A line-based TCP server written in [Diamond](https://getdiamond.codes). Each line a
client sends gets one line back.

```sh
facet install                   # fetch minitest, the test library, into cuts/
diamond main.d6                 # listens on 127.0.0.1:7000
printf 'PING\nhello\n' | nc -q1 127.0.0.1 7000    # PONG, echo: hello
bash test.sh                    # run the tests
diamond build main.d6 -o {{name}}   # a standalone executable
```

`PORT` and `HOST` set the address. The default host, `127.0.0.1`, is reachable only from
this machine; use `HOST=0.0.0.0` to accept outside connections.

- `main.d6` opens a non-blocking listener and calls `{{Name}}.serve`.
- `lib/{{name}}.d6` holds the protocol (`{{Name}}.respond`) and the server loop, which
  uses `IO.poll` to serve every client from one thread.
- `test/{{name}}_test.d6` tests the protocol with minitest.
  `diamond.dev.cut` lists it because only the tests need it.
