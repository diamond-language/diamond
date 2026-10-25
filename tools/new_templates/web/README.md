# {{name}}

A web application written in [Diamond](https://getdiamond.codes), served by
[gremlin](https://github.com/diamond-language/diamond/tree/main/packages/gremlin) with
[rack](https://github.com/diamond-language/diamond/tree/main/packages/rack) middleware.

```sh
facet install              # fetch gremlin, rack and minitest into cuts/
diamond main.d6            # http://127.0.0.1:8080
bash test.sh               # run the tests
```

`PORT` and `HOST` set the address. The default host, `127.0.0.1`, is reachable only from
this machine; use `HOST=0.0.0.0` to accept outside connections, or put the app behind a
reverse proxy.

- `main.d6` starts the server.
- `lib/{{name}}.d6` holds the routes (`{{Name}}.route`) and the middleware chain
  (`{{Name}}.build_chain`).
- `test/{{name}}_test.d6` calls the handler with request hashes, so the tests start no
  server. It uses minitest, which `diamond.dev.cut` lists because only the tests need it.

`facet install` writes `facet.lock`; commit it so every checkout gets the same versions.
`facet update` moves to the newest versions the constraints in `diamond.cut` allow.
