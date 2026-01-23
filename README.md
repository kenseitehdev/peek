# peek — a multi-buffer terminal pager

### peek is a lightweight ncurses-based terminal pager with:

### Multiple buffers (tabs)

### Syntax highlighting for source files

### Built-in man page viewing (auto-detected)

### Stdin support (pipes)

### Search (/, n, N)

### Simple Vim-like navigation

### Optional fzf file opening

#### Think: less + tabs + syntax highlighting + man pages, without being a full editor.

### Features

##### 📄 View multiple files at once

##### 📘 View man pages as tabs ("man grep")

##### 🔁 Mix files, stdin, and commands in one invocation

#### 🎨 Syntax highlighting for:

##### C / C++

##### Python

##### JavaScript / TypeScript

##### Shell

##### Markdown

##### Man pages (sections + flags)

#### 🔍 Incremental search

#### ⌨️ Keyboard-driven navigation

#### 📂 Optional fzf integration (o key)

### Installation
#### Requirements

A POSIX system (Linux, BSD, macOS)

ncurses

A C compiler (gcc or clang)

Optional:

fzf (for interactive file opening)

Install dependencies
macOS
brew install ncurses fzf

Linux (Debian/Ubuntu)
sudo apt install libncurses-dev fzf

Arch Linux
sudo pacman -S ncurses fzf

Build
cc -Wall -Wextra -O2 -o peek peek.c -lncurses


Or with clang:

clang -Wall -Wextra -O2 -o peek peek.c -lncurses


Install system-wide (optional):

sudo mv peek /usr/local/bin/

Usage
Basic file viewing
peek file1.c file2.py README.md


Each file opens in its own buffer (tab).

Navigation
Key	Action
j / k	Scroll down / up
g / G	Top / bottom
Ctrl-D / Ctrl-U	Half-page down / up
Tab	Next buffer
Shift-Tab	Previous buffer
/	Search
n / N	Next / previous match
q	Quit
Viewing man pages (auto-detected)

No flags required — just pass "man <command>":

peek "man grep" "man sed" "man awk"


Each man page opens as its own buffer with:

Highlighted section headers

Highlighted flags (-r, --help, etc.)

💡 Tip: for better wrapping:

peek "MANWIDTH=200 man grep"

Using stdin (pipes)

View piped input:

cat file.txt | peek


Or mix stdin with files:

cat file.txt | peek - other.txt


- explicitly means “read stdin into a buffer”.

Mixing everything together

This is where peek shines:

peek "man grep" file1.c "man sed" file2.py


Or with stdin:

man grep | peek - "man sed" file.c


Buffers can contain:

Files

Stdin

Man pages

Command output

Explicit command buffers (-m)

Auto-detection is enabled by default, but you can still force command execution:

peek -m "man grep" -m "ls -la"


Useful for non-man commands.

fzf file picker

Press o inside peek to open a new file using fzf
(the file becomes a new buffer).

Requires fzf to be installed.

Notes & Design Choices

Man page formatting is normalized (backspace overstrikes removed)

Only one real stdin exists — additional “stdin-like” inputs are commands

Input is fully buffered before entering ncurses (not a streaming pager)

This is a viewer, not an editor (by design)

Limitations

No live streaming (input is read once)

Syntax highlighting is heuristic, not a full parser

Terminal width affects man page wrapping

License

MIT (or whatever you choose)

Why peek?

Because sometimes you want to:

Compare multiple files

Read several man pages at once

Pipe output and open files

Stay in the terminal

Not launch a full editor

peek does exactly that — nothing more, nothing less.
