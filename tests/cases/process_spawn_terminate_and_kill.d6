h1 = Process.spawn(["sleep", "30"])
puts(h1.running?())
h1.terminate()
puts(h1.wait())
puts(h1.running?())

h2 = Process.spawn(["sleep", "30"])
h2.kill()
puts(h2.wait())
