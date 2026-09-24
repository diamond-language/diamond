# Diamond platform report

This tries Diamond on your computer and writes a report file to send back. You
don't need to know anything about Diamond. It takes about 20-40 minutes, mostly
unattended.

## Before you start

- Plug your laptop in.
- You need **Homebrew**. If you don't have it, install it from
  [brew.sh](https://brew.sh) first (copy the one line from that page into
  Terminal and follow its prompts).
- If macOS asks to install "command line developer tools" at any point, click
  **Install** and run the steps again afterwards.

## Run it

Open **Terminal** (press Cmd-Space, type "Terminal", press Return), then paste
these three lines and press Return:

```sh
git clone https://github.com/diamond-language/diamond.git ~/diamond
cd ~/diamond
caffeinate -i python3 tools/platform_report/platform_report.py
```

`caffeinate -i` just keeps the Mac awake until it finishes.

If it lists some Homebrew packages and asks **"Install them now with Homebrew?"**,
type `y` and press Return. They're the libraries Diamond builds against
(OpenSSL, SQLite, and the PostgreSQL and MariaDB client libraries) plus a few
standard command-line tools.

## When it's done

It prints something like:

```text
Report written to /Users/you/Desktop/diamond-report-20261002-1430.txt
Please send that file back. Thanks!
```

Send that file back. That's everything.

## What it does, and doesn't do

- It never uses `sudo` or changes system settings. The only thing it can
  install is the Homebrew packages above, and only after you type `y`.
- It works in a temporary copy that it deletes afterwards. The `~/diamond`
  folder can be deleted when you're done (`rm -rf ~/diamond`).
- It uses the network to download Diamond, the Homebrew packages, and one
  package from Diamond's registry, cuts.dilang.tech, for an example app.
- The report contains your Mac's model, chip, core count, memory size, and
  macOS version, plus build logs and timings. Nothing personal.

If something goes wrong, the report still gets written with whatever it
learned. Send it anyway.
