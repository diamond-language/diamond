# {{name}}

A GraphQL API written in [d6](https://rolld6.codes), built schema-first with
[graphql](https://github.com/diamond-language/d6/tree/main/packages/graphql)'s `GraphQL::App`
and served by [gremlin](https://github.com/diamond-language/d6/tree/main/packages/gremlin).
It needs d6 1.1.0 or later, for `File.embed`.

```sh
facet install              # fetch graphql, gremlin, rack and minitest into cuts/
d6 main.d6                 # http://127.0.0.1:8080/graphql
bash test.sh               # run the tests
```

Try it:

```sh
curl -s localhost:8080/graphql -H 'content-type: application/json' \
  -d '{"query": "mutation { addNote(text: \"hello\") { id text } }"}'
curl -s localhost:8080/graphql -H 'content-type: application/json' \
  -d '{"query": "{ notes { id text } }"}'
```

`PORT` and `HOST` set the address. The default host, `127.0.0.1`, is reachable only from
this machine; use `HOST=0.0.0.0` to accept outside connections, or put the app behind a
reverse proxy.

- `lib/schema.graphql` is the API. `File.embed` compiles it into the program, so a
  `d6 build` binary needs no file beside it.
- `lib/{{name}}.d6` holds the resolvers, one class per type that needs code, with a method per
  field named in snake_case (`addNote` -> `add_note`). Fields without a method, like
  `Note.text`, read the value's own field. It also holds the HTTP routes: `POST /graphql`,
  `GET /schema.graphql` and `GET /health`.
- `test/{{name}}_test.d6` posts queries to the handler, so the tests start no server. It uses
  minitest, which `Cutfile.dev` lists because only the tests need it.

Notes live in memory and are lost on restart; swap `NoteStore` for a database when they matter.
Errors carry a code in `extensions.code` (`BAD_USER_INPUT`, `BAD_REQUEST`, `INTERNAL`), so a
client can branch on the code rather than the message.

`facet install` writes `Cutfile.lock`; commit it so every checkout gets the same versions.
`facet update` moves to the newest versions the constraints in `Cutfile` allow.
