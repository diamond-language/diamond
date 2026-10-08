# network_safety

Reject local names and private or reserved IPv4 literals before an outbound HTTP request.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add network_safety --registry https://cuts.dilang.tech --version "^0.1.2"
facet update
```

This installs the cut into `cuts/network_safety/`; load it with `require_cut "network_safety"`. `http` is installed with it.

## Usage

```ruby
require_cut "network_safety"

parsed = resolve_public_hostname("https://api.example.com/data")
# => {"scheme": "https", "host": "api.example.com", "port": 443,
#     "path": "/data", "default_port": 443}

resolve_public_hostname("http://127.0.0.1/x")
# => raises ArgumentError: "URL host is a private/reserved IPv4 address: 127.0.0.1"
```

## In a project

Put the check in front of every outbound request that uses a URL a user can influence, such as
a webhook or an avatar import:

```ruby
require_cut "http"
require_cut "network_safety"

def fetch_public(url)
  resolve_public_hostname(url)    # raises ArgumentError for localhost and private ranges
  http_get(url)
end

begin
  fetch_public("http://localhost/admin")
rescue error: ArgumentError
  # reject the request, e.g. return a 422 to the caller
end
```

## Notes

Call `resolve_public_hostname(url)` before `http_get(url)`. This function checks the URL text; it does not resolve DNS or pin the connection address. A public-looking hostname that resolves to a private address is not blocked.
