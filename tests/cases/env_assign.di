ENV["DIAMOND_ENV_ASSIGN"] = "hello"
read_back = ENV["DIAMOND_ENV_ASSIGN"]
child = Process.run(["sh", "-c", "printf %s \"$DIAMOND_ENV_ASSIGN\""])
overridden = Process.run(["sh", "-c", "printf %s \"$DIAMOND_ENV_ASSIGN\""], {"env": {"DIAMOND_ENV_ASSIGN": "over"}})
ENV.delete("DIAMOND_ENV_ASSIGN")
gone = Process.run(["sh", "-c", "printf %s \"${DIAMOND_ENV_ASSIGN-unset}\""])
[read_back, child.stdout(), overridden.stdout(), gone.stdout()]
