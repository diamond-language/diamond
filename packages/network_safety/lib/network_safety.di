require "../../http/lib/http"

# A basic SSRF (server-side request forgery) guard for outbound HTTP
# requests -- ported from ModFederate::NetworkSafety (MaquinasStack's
# private Ruby gem). Rejects an obviously-local target before an
# ingest-style caller (see e.g. skindicate.dia's own winamp ingester)
# hands a URL from a third-party API response to http_get/http_post.
#
# **String-level checks only -- this is a real, deliberate reduction
# from the Ruby original, not an oversight.** The Ruby version resolves
# the hostname via DNS, validates the resulting IP address, then
# connects to that *exact* IP (bypassing a second DNS lookup), so a
# hostname that resolves to a private/internal address is caught even
# though the URL string itself looks innocuous, and a DNS answer that
# changes between the check and the connect (DNS rebinding) can't slip
# through either. Diamond has no way to resolve a hostname to an IP
# address without actually connecting (`TCPSocket.connect` does
# resolve+connect as one step and never exposes the resolved address
# afterward), so neither of those protections is buildable at the
# Diamond-language level today. What's checked here instead: the URL
# scheme, a missing host, embedded userinfo (`user:pass@host` -- some
# HTTP clients historically use this to smuggle a different real
# target), and whether the host *string itself* is a private/loopback/
# link-local IPv4 literal or an obviously-local name (`localhost`, a
# `.local`/`.internal` suffix). A hostname that only *resolves* to a
# private address is not caught. Confirmed with the maintainer this is
# an acceptable gap for now: the URLs this guards (a fixed third-party
# API's own GraphQL/CDN responses) aren't raw, arbitrary user input,
# just semi-trusted data from that API. A native DNS-resolve-without-
# connecting VM primitive would close this gap properly; that's a
# separate, bigger change (touches src/vm.c, needs a rebuild), not
# attempted here.
#
# IPv4 only, matching the Ruby original's own real coverage need: no
# bitwise-AND operator exists in Diamond (only `<<`, for left-shift),
# so CIDR containment is done via integer division instead (two
# addresses share a /prefix network iff they're equal after dividing
# out the 2^(32-prefix) host-bit range) -- exact, no masking needed.
# The same trick doesn't scale as cleanly to IPv6's 128-bit space, and
# the APIs this guards are plain IPv4/dual-stack HTTPS endpoints, so an
# IPv6 blocklist table isn't attempted; a host that IS a literal IPv6
# address (contains ":") is rejected outright below rather than parsed.
# Diamond has no module-level constant a function body can read as a
# plain variable (see e.g. active_auth's own Session.duration_seconds()
# for the established stand-in) -- a function returning the literal
# array in place of a real constant.
def network_safety_blocked_ipv4_networks()
  [
    ["0.0.0.0", 8], ["10.0.0.0", 8], ["100.64.0.0", 10], ["127.0.0.0", 8],
    ["169.254.0.0", 16], ["172.16.0.0", 12], ["192.0.0.0", 24], ["192.0.2.0", 24],
    ["192.168.0.0", 16], ["198.18.0.0", 15], ["198.51.100.0", 24], ["203.0.113.0", 24],
    ["224.0.0.0", 4], ["240.0.0.0", 4],
  ]
end

# A dotted-quad String -> its 32-bit integer value, or nil if `text`
# isn't a canonical "0-255.0-255.0-255.0-255" form (rejects garbage,
# out-of-range octets, and non-canonical forms like a leading zero --
# "01" reads as decimal 1 but isn't how a real IPv4 literal is
# written, so treating it as one here would be more permissive than
# intended).
def network_safety_ipv4_to_int(text)
  parts = text.split(".")
  if parts.length() != 4
    return nil
  end
  total = 0
  index = 0
  while index < 4
    part = parts[index]
    octet = part.to_i()
    if octet < 0 || octet > 255 || "#{octet}" != part
      return nil
    end
    total = total * 256 + octet
    index += 1
  end
  total
end

def network_safety_ipv4_blocked?(ip_int)
  networks = network_safety_blocked_ipv4_networks()
  index = 0
  while index < networks.length()
    network_text = networks[index][0]
    prefix = networks[index][1]
    network_int = network_safety_ipv4_to_int(network_text)
    divisor = 1 << (32 - prefix)
    if ip_int / divisor == network_int / divisor
      return true
    end
    index += 1
  end
  false
end

def network_safety_looks_local?(host)
  lower = host.downcase()
  lower == "localhost" || lower.end_with?(".local") || lower.end_with?(".internal")
end

# The raw "host[:port]" segment straight out of the URL string -- the
# same slice http_parse_url itself works from, extracted independently
# so userinfo (`user:pass@host`) and bracketed IPv6 literals
# (`[::1]`) can be caught *before* handing the URL to http_parse_url,
# which has no support for either and would otherwise silently
# misparse one into a garbage host/port instead of flagging it (e.g.
# `[::1]` alone splits on its own first colon into host `[`, port
# `::1]`.to_i() == 0 -- neither the userinfo nor the IPv6 check below
# would ever see the real host string if they ran after that).
def network_safety_host_and_port_segment(url)
  scheme_end = url.index_of("://")
  if scheme_end == nil
    return ""
  end
  rest = url.slice(scheme_end + 3, url.length())
  path_start = rest.index_of("/")
  if path_start == nil then rest else rest.slice(0, path_start) end
end

# The checked, parsed URL (http_parse_url's own {"scheme", "host",
# "port", "path", "default_port"} shape) on success. Raises
# ArgumentError -- never a silent nil -- on any rejection: an
# unsupported/missing scheme (http_parse_url's own check), embedded
# userinfo, a literal IPv6 host, a missing host, a blocked IPv4
# literal, or an obviously-local hostname.
def resolve_public_hostname(url)
  raw_host_and_port = network_safety_host_and_port_segment(url)
  if raw_host_and_port.index_of("@") != nil
    raise ArgumentError.new("URL credentials are not allowed: #{url}")
  end
  if raw_host_and_port.start_with?("[")
    raise ArgumentError.new("literal IPv6 hosts are not allowed: #{url}")
  end
  parsed = http_parse_url(url)
  host = parsed["host"]
  if host == ""
    raise ArgumentError.new("URL host is required: #{url}")
  end
  if network_safety_looks_local?(host)
    raise ArgumentError.new("URL host is a local hostname: #{host}")
  end
  ip_int = network_safety_ipv4_to_int(host)
  if ip_int != nil && network_safety_ipv4_blocked?(ip_int)
    raise ArgumentError.new("URL host is a private/reserved IPv4 address: #{host}")
  end
  parsed
end
