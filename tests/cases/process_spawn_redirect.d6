# Process.spawn output redirection: to files, and stderr merged into stdout.
dir = Process.run(["mktemp", "-d"]).stdout().strip()

split = Process.spawn(["sh", "-c", "echo out; echo err 1>&2"],
  {"stdout": "#{dir}/out.log", "stderr": "#{dir}/err.log"})
split_exit = split.wait()
split_streams = [split.stdout(), split.stderr()]

merged = Process.spawn(["sh", "-c", "echo one; echo two 1>&2; echo three"],
  {"stdout": "#{dir}/merged.log", "stderr_to_stdout": true})
merged.wait()

piped = Process.spawn(["sh", "-c", "echo through the pipe; echo and stderr 1>&2"], {"stderr_to_stdout": true})
IO.poll([piped.stdout()], [], 5000)
piped.wait()
text = piped.stdout().read(200)

appended = Process.spawn(["sh", "-c", "echo again"], {"stdout": "#{dir}/out.log"})
appended.wait()

bad = false
begin
  Process.spawn(["true"], {"stdout": 7})
rescue e: TypeError
  bad = true
end

result = [split_exit, split_streams, File.read("#{dir}/out.log").split("\n"), File.read("#{dir}/err.log").strip(),
          File.read("#{dir}/merged.log").split("\n"), piped.stderr(), text.split("\n").sort(), bad]
Process.run(["rm", "-rf", dir])
result
