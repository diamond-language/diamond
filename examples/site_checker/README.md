# Site checker

A useful worker-pool example: check a URL list concurrently, follow redirects,
report status and latency, and return a failing exit status for broken links.
The HTTP and cancellation cuts supply the I/O and cooperative shutdown;
bounded channels keep the producer from running ahead of the workers.

From the repository root:

```sh
make debug facet
bash tools/install_local_cuts.sh .
build/diamond examples/site_checker/check.d6 examples/site_checker/urls.txt
# OK https://example.com/ — 200 (... ms)
# OK https://example.org/ — 200 (... ms)
build/diamond examples/site_checker/check.d6 --json --workers 4 --timeout-ms 2000 examples/site_checker/urls.txt
```

One URL per line; blank lines and `#` comments are ignored. A list contains
1–1,000 URLs. Workers default to 4 (maximum 16); connect/read/write timeouts
default to 2,000 ms (maximum 30,000). Checks use HEAD, so sites that reject HEAD
are reported as failures rather than downloading arbitrary response bodies.
Relative redirects are followed up to five hops. These are per-I/O timeouts,
not an end-to-end request budget: a slow stream or multiple redirect hops can
take longer. DNS and native calls follow the runtime's timeout semantics.

Results retain input order even when requests complete out of order. JSON is an
array of `{url, status, ok, elapsed_ms, error}`; status is null on network errors.
HTTP 200–399 is successful. Exit codes: 0 all successful, 1 failed checks,
64 invalid arguments/input, 130 interrupted. Ctrl-C cancels channel waits and
stops scheduling work; active HTTP calls finish or hit their I/O timeout before
workers join. No further results are printed after cancellation.

Build a standalone executable with
`build/diamond build examples/site_checker/check.d6 -o /tmp/site-check`.
`bash examples/site_checker/smoke_test.sh` tests interpreted and compiled modes
against a local fixture: no public internet is needed. CI runs it through
`make test-example-apps`.
