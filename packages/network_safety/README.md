# network_safety

Reject URLs that resolve to private or reserved network addresses before an outbound HTTP request.

## Installation

Install the cut at `cuts/network_safety/` and load it with `require_cut "network_safety"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `http`.

## Usage

```ruby
require_cut "network_safety"

parsed = resolve_public_hostname("https://api.example.com/data")
# => {"scheme": "https", "host": "api.example.com", "port": 443,
#     "path": "/data", "default_port": 443}

resolve_public_hostname("http://127.0.0.1/x")
# => raises ArgumentError: "URL host is a private/reserved IPv4 address: 127.0.0.1"
```

## Notes

Call `resolve_public_hostname(url)` before `http_get(url)`. DNS can change between the check and the request, so this check alone does not pin the destination address.
