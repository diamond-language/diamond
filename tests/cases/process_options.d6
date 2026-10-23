# Process.run / Process.spawn options: env, cwd, stdin.
env_and_cwd = Process.run(["sh", "-c", "echo $DIAMOND_TEST_VALUE; pwd"],
  {"env": {"DIAMOND_TEST_VALUE": "from-env"}, "cwd": "/"})

# stdin larger than a pipe buffer is fed while output is drained, so this cannot deadlock.
counted = Process.run(["wc", "-c"], {"stdin": "x" * 300000}).stdout().strip()

echoed = Process.run(["cat"], {"stdin": "fed\n"}).stdout()

handle = Process.spawn(["cat"], {"stdin": true})
handle.stdin().write("piped\n")
handle.stdin().close()
IO.poll([handle.stdout()], [], 5000)
spawned = handle.stdout().read(100)
spawned_exit = handle.wait()

quiet = Process.spawn(["true"])
no_stdin = quiet.stdin()
quiet.wait()

rejected = false
begin
  Process.run(["true"], {"bogus": 1})
rescue e: TypeError
  rejected = true
end

bad_cwd = false
begin
  Process.run(["true"], {"cwd": "/definitely/not/here"})
rescue e: IOError
  bad_cwd = true
end

[env_and_cwd.stdout().split("\n"), counted, echoed.strip(), spawned.strip(), spawned_exit, no_stdin, rejected, bad_cwd]
