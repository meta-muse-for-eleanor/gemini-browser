# Gemini protocol reference

## Request and response

A Gemini request is one absolute URL plus CRLF, sent over TLS on port 1965.
The server replies with a header line, then a body that ends when the
connection closes.

Header: two digit status, one space, meta text, CRLF. Meta is a MIME type
on success, a URL on redirect, a prompt on input, or an error message.

## Status codes

- 10 Input, plain text answer expected
- 11 Input, sensitive answer expected
- 20 Success, meta is the MIME type
- 30 Redirect, temporary, meta is the new URL
- 31 Redirect, permanent, meta is the new URL
- 40 Temporary failure
- 41 Server unavailable
- 42 CGI error
- 43 Proxy error
- 44 Slow down
- 50 Permanent failure
- 51 Not found
- 52 Gone
- 53 Proxy request refused
- 54 Bad request
- 59 Bad request, other permanent failure
- 60 Client certificate required
- 61 Certificate not authorised
- 62 Certificate not valid

## Gemtext markup

The client renders text/gemini into plain text and numbers its links.

- `#`, `##` and `###` are headings
- `=> URL label` is a link, printed as `[n] label (URL)`
- `*` is a list item
- `>` is a quote
- Lines between triple backticks are preformatted text
- Any other line is plain text

## Network in this VM

Direct TCP to port 1965 is blocked here. The egress proxy returns a
plain text refusal for a direct connection. The client therefore
honours https_proxy, HTTPS_PROXY, http_proxy, HTTP_PROXY, all_proxy
and ALL_PROXY, in that order, and opens an HTTP CONNECT tunnel to
host:1965 through the proxy before starting TLS. That is the same
route HTTPS access uses here. With no proxy variable set, the client
connects directly. Proxy credentials come from the environment at
run time and are never hard coded or logged.

## What was verified

Verified on 2026-11-20 with the compiled client in this VM:

- gemini://geminiprotocol.net/ fetched and rendered, 7 links found
- gemini://gemini.circumlunar.space/ fetched and rendered
- gemini://geminiprotocol.net/docs/faq.gmi fetched, relative links numbered
- gemini://geminiprotocol.net/no-such-page.gmi returned 51 Not found
- Interactive mode followed link 4 from the home page to /docs/ and quit cleanly
- A direct TCP attempt without the proxy failed with a TLS version error, and the same fetch succeeded through the CONNECT tunnel
