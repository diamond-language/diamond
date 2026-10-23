# Header-Hash helpers shared by the middlewares. Response header names are matched case-insensitively (HTTP says they are), although the Hashes
# here are keyed by whatever spelling the handler used, so `x-frame-options` and `X-Frame-Options` must be seen as one header.
class RackHeaders
  # The key `name` is stored under in `headers`, or nil.
  def self.key_for(headers, name)
    wanted = name.downcase()
    found = nil
    headers.keys().each() do |key|
      found = key if found == nil && "#{key}".downcase() == wanted
    end
    found
  end

  # `headers` with each of `additions` added only when the response does not already set that header: a route's own stricter value wins.
  def self.add_missing(headers, additions)
    result = headers
    additions.keys().each() do |name|
      if RackHeaders.key_for(result, "#{name}") == nil
        extra = {}
        extra[name] = additions[name]
        result = result.merge(extra)
      end
    end
    result
  end

  # `headers` with `token` added to Vary (the existing Vary value is kept and the token appended once).
  def self.add_vary(headers, token)
    existing = RackHeaders.key_for(headers, "Vary")
    update = {}
    if existing == nil
      update["Vary"] = token
    else
      listed = "#{headers[existing]}".split(",").map() do |part| part.strip().downcase() end
      return headers if listed.include?(token.downcase()) || listed.include?("*")
      update[existing] = "#{headers[existing]}, #{token}"
    end
    headers.merge(update)
  end
end
