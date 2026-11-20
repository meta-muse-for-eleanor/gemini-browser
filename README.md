# gemini-browser

A Muse skill for fetching and summarising `gemini://` pages, just like
`https://` pages.

When a Gemini link appears in an email, a message or a question, Muse
fetches it with the C client in `scripts/`, renders the gemtext, and
summarises it in the same style as a web page. It follows links and
redirects, handles input prompts, and reports errors in plain language.

## Layout

- `SKILL.md` — the skill Muse follows
- `skill.yaml` — name, title, description and category
- `scripts/gemini.c` — the Gemini client, in C, using OpenSSL
- `scripts/build.sh` — builds `scripts/gemini`
- `references/gemini-protocol.md` — status codes, gemtext, and what was verified

## Build

```sh
./scripts/build.sh
```

That runs `gcc -Wall -O2 -o gemini gemini.c -lssl -lcrypto`.
It needs gcc and the OpenSSL development libraries.

## Use

```sh
./scripts/gemini gemini://geminiprotocol.net/
./scripts/gemini -r gemini://example.com/page.gmi
./scripts/gemini -i gemini://geminiprotocol.net/
```

With no flags it fetches once, renders `text/gemini`, and exits.
`-r` prints the raw body. `-i` is interactive: type a link number,
a URL, `b` for back, or `q` to quit.

## Network

Direct TCP to port 1965 is blocked in the Muse VM. The client
therefore honours `https_proxy` and the `http_proxy` / `all_proxy`
fallbacks, opening an HTTP CONNECT tunnel to `host:1965` through the
proxy before starting TLS, the same route HTTPS access uses there.
With no proxy variable set it connects directly.

Gemini capsules almost all use self-signed certificates, so the
client uses trust-on-first-use and does not verify them. Do not send
sensitive data to a Gemini page.

## Limits

- Pages needing a client certificate (status 60 to 69) cannot be opened.
- Non-text content is reported by MIME type and size, not read.
- Gemini has no scripts, stylesheets, cookies or tracking to reproduce.
