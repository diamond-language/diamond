"""A VM without a trap must not steal a signal while its owner is joining."""
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

program = '''
class Signals
  def self.caught()
    @@caught = true
  end
  def self.check()
    unless @@caught == true then raise "signal lost to another VM" end
  end
end
def busy()
  puts("worker ready")
  deadline = Time.monotonic() + 0.5
  while Time.monotonic() < deadline
    nil
  end
end
Signal.trap("TERM", Signals.caught)
worker = Thread.new(busy)
puts("owner joining")
worker.join()
Signals.check()
puts("caught")
'''
for attempt in range(5):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "output"
        with path.open("w") as log:
            process = subprocess.Popen([sys.argv[1], "-e", program], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while "worker ready" not in path.read_text() or "owner joining" not in path.read_text():
                    assert process.poll() is None and time.monotonic() < deadline, path.read_text()
                    time.sleep(.005)
                # Give the owner time to enter join; the other VM remains busy.
                time.sleep(.03)
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=5) == 0, path.read_text()
                assert "caught\n" in path.read_text(), path.read_text()
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
print("thread signal ownership tests passed")
