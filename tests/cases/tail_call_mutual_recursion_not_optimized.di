def ping(n)
  return pong(n + 1)
end
def pong(n)
  return ping(n + 1)
end
ping(0)
