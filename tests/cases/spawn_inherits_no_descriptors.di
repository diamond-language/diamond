# A spawned child must not inherit the VM's own sockets, files and pipes. Another thread's child holding a
# listener keeps its port bound, and one holding a pipe's write end stops a capture from ever seeing EOF.
# Compares the child's open descriptors (read from /proc) before and after the VM opens some, so it is
# a no-op where /proc does not exist.
def child_descriptors()
  script = "for entry in /proc/$$/fd/*; do echo ${entry##*/}; done"
  Process.run(["sh", "-c", script]).stdout().split("\n").select() do |entry| entry != "" end.sort().join(",")
end

if !File.directory?("/proc/self/fd")
  # No /proc here (macOS, FreeBSD): there are no descriptors to compare, and the case has one expected output for every platform.
  "same descriptors: true"
else
  before = child_descriptors()
  listener = TCPServer.listen(19655)
  datagrams = UDPSocket.open()
  file = File.open("tests/cases/spawn_inherits_no_descriptors.di", "r")
  background = Process.spawn(["sleep", "2"])
  after = child_descriptors()
  background.kill()
  background.wait()
  listener.close()
  datagrams.close()
  file.close()
  "same descriptors: #{before == after}"
end
