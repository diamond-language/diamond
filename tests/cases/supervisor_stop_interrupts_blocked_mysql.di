# A MySQL query waiting on a server that never answers is interrupted. The
# server is a stub in this script: it sends a handshake, accepts any login and
# then reads without replying.
HANDSHAKE = "\x4e\x00\x00\x00\x0a\x35\x2e\x37\x2e\x30\x2d\x66\x61\x6b\x65\x00\x01\x00\x00\x00\x61\x62\x63\x64\x65\x66\x67\x68\x00\x0d\x82\x21\x02\x00\x08\x00\x15\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x69\x6a\x6b\x6c\x6d\x6e\x6f\x70\x71\x72\x73\x74\x00\x6d\x79\x73\x71\x6c\x5f\x6e\x61\x74\x69\x76\x65\x5f\x70\x61\x73\x73\x77\x6f\x72\x64\x00"
LOGIN_OK = "\x07\x00\x00\x02\x00\x00\x00\x02\x00\x00\x00"

def querier()
  begin
    db = MySQL.open("127.0.0.1", "u", "p", "d", 19736)
    db.query("select 1")
  rescue error: StandardError
    # Newer client libraries insist on TLS, which the stub cannot speak; the
    # worker then ends on its own and the test only checks it is gone.
    nil
  end
end

listener = TCPServer.listen_nonblocking(19736)
sup = Supervisor.new()
sup.add_child(querier)
IO.poll([listener], [], 5000)
client = listener.accept()
client.write(HANDSHAKE)
IO.poll([client], [], 5000)
client.read(4096)
client.write(LOGIN_OK)
sup.stop(300)
puts(sup.alive?(0))
