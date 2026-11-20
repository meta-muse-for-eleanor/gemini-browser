# Browse Gemini pages

## Treat a Gemini link like a web link

- Treat every `gemini://` link the same way you treat an `https://` link.

- Fetch the page when a Gemini link appears in an email, a message, a note or a question, and summarise it in the same style and length you would use for a web page.

- Name the Gemini URL you fetched when you summarise it, so the user can open it themselves.

- Never invent page content, and never guess a Gemini URL from a web address or a page title.

- Ask the user for the link when they mention a Gemini page without giving its address.

## Fetch a page

- Run the C client in `muse.exec` with the full Gemini URL as its only argument.

- Use `~/workspace/custom-skills/gemini-browser/scripts/gemini gemini://example.com/page.gmi` as the command form.

- Build the client first when the binary is missing, with `~/workspace/custom-skills/gemini-browser/scripts/build.sh`.

- Run the client with the shell environment unchanged, so it can use the egress proxy in `https_proxy` for port 1965.

- Read the rendered text the client prints, including its headings, lists, quotes and numbered links.

- Keep the URL and the MIME type the client prints at the top of its output with the page content.

- Fetch the page again only when the first fetch clearly failed, and say what failed when you cannot fetch it.

## Summarise a page

- Summarise the rendered text, not the raw gemtext markup.

- Keep the summary focused on what the user asked for, and mention the rest of the page only when it changes the answer.

- Quote a short passage only when the exact wording matters.

- Preserve the meaning of headings, lists and links from the page.

- Say when a page is long and you are summarising only the part that answers the question.

## Follow links and search

- Follow a numbered link from a fetched page when it is likely to answer the question, the same way you would follow a link from a web page.

- Run the client again with the resolved link URL, and use the client output for the new page.

- Resolve a relative link against the page it came from, and let the client do that work in interactive mode when you are browsing with the user.

- Use interactive mode only when the user is browsing with you, with `gemini -i gemini://example.com/`.

- Follow a small number of links and stop when another link is unlikely to change the answer.

- Search Geminispace through a search capsule the user names or a page links to, and answer its input prompt as described below.

- Find a Gemini address with a web search first when the user gives you only a topic, then fetch the Gemini address you found.

## Handle input, redirects and errors

- Let the client follow redirects for you, and report the final URL when it differs from the one you asked for.

- Treat status 10 and status 11 as a prompt for input, the Gemini equivalent of a web form.

- Ask the user for the input the page requests, and do not guess it.

- Fetch the page again with the answer appended as `?answer` when the user gives it to you.

- Treat a sensitive input prompt as private, and do not repeat the answer in chat or save it in memory.

- Report a status 40 to 49 error as a temporary failure and a status 50 to 59 error as a permanent failure, with the message the server gave.

- Report status 51 as not found, in the same way you would report a web 404.

- Report a status 60 to 69 error as a page that needs a client certificate, and say you cannot open it.

- Report a connection or TLS failure plainly, and offer to try the web version when one exists.

## Handle non-text pages

- Read `text/gemini` and `text/plain` pages as text.

- Treat any other MIME type as a file you have not read, and report its type and size from the client output.

- Do not claim to have read an image, an audio file or another binary file.

- Offer to fetch the web version or to describe what the link text says about the file when the user needs more.

## Know the limits

- Accept that Gemini pages have no scripts, stylesheets, cookies or tracking, so there is no interactive web behaviour to reproduce.

- Accept that the client trusts self-signed certificates on first use, as Gemini clients do, and do not send sensitive data to a Gemini page.

- Accept that a page behind a client certificate cannot be opened, and say so when you meet one.

- Use the web version of a page when the Gemini version is missing and the user still needs the information.

- Read `references/gemini-protocol.md` for the status codes and the gemtext markup the client renders.

## Available connectors

- Use `muse.connector_search` to find other connectors and `muse.skill_search` to find related skills.

- If nothing here fits, look on the web for a maintained API or MCP server and set it up with [Custom Connectors and MCPs](/opt/muse/skills/custom-connector-setup/SKILL.md), or build a custom connector.
