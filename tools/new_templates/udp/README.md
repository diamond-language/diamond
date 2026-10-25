# {{name}}

A UDP server written in [Diamond](https://getdiamond.codes). Each datagram a client sends
gets one datagram back.

```sh
diamond main.d6                 # listens on UDP 127.0.0.1:7001
printf 'PING' | nc -u -w1 127.0.0.1 7001    # PONG
bash test.sh                    # run the tests
diamond build main.d6 -o {{name}}   # a standalone executable
```

`PORT` and `HOST` set the address. The default host, `127.0.0.1`, is reachable only from
this machine; use `HOST=0.0.0.0` to accept outside datagrams.

- `main.d6` binds the socket and calls `{{Name}}.serve`.
- `lib/{{name}}.d6` holds the protocol (`{{Name}}.respond`) and the receive loop.
- `test/{{name}}_test.d6` tests the protocol with `test/minitest.d6`, a copy of Diamond's
  minitest library.
