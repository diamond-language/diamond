result = Process.run(["echo", "hello"])
puts(result.stdout())
puts(result.exit_code())
result.success?()
