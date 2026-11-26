# SYW - Show Your Work

SYW records how a document evolved, not just how it ended. You edit a normal DOCX in your usual editor;
every time you save, SYW exports Markdown with pandoc and commits both files to git, so the history becomes
part of the work. The idea and goals are in [SYW.md](SYW.md). This README covers the **code**.

```
 editor (LibreOffice)  --save-->  DOCX  --pandoc-->  Markdown  --git add / commit-->  history
                                   ^                                                   |
                                   +------------- SYW watches, prompts, shows log <----+
```

## Layout

| File | Role |
|------|------|
| `SYW.h` / `SYW.cpp` | **Core** (`class SYW`) and `main()`. No GUI types in the class; everything external goes through hooks. |
| `MainWindow.h` / `MainWindow.cpp` | **Qt front end**: File and View menus, branch/commit/file tree, Markdown popup viewer, snapshot diff, editor process, file watching. Wires the hooks. |
| `Error.h` | `Error` object, `MSG_ERR()` and the `Error::Publisher` output hook. |
| `Makefile` | GNU make build (explicit Qt flags, `moc` rule). |
| `SYW.md` | Design document: the "why". |

The headers are documented with Doxygen-style comments (`@brief`, `@param`, `@details`), so
`doxygen` can generate API docs if you add a `Doxyfile`.

## Build

Requirements: GNU make, g++ (C++17 or later), Qt 6 Widgets, and the sibling repositories `../XmlCls` and
`../cpp-base64` that the link line currently expects.

```sh
gmake                      # builds ./SYW  (release)
gmake DEBUG=-g             # debug build
gmake QT_PREFIX=/path/to/Qt/6.x/gcc_64    # different Qt (default: /usr/local/Qt/6.5.3/gcc_64)
gmake clean
```

`MainWindow.h` contains `Q_OBJECT`, so the Makefile runs Qt's `moc` to produce `moc_MainWindow.cpp`
before compiling. Build output is also logged to `Install.log`.

## Run

```sh
./SYW                      # then File > Open...
./SYW --file path/to/document.docx
```

Runtime requirements:

- the **document must be inside a git repository** (a new repo with no commits is fine; the first save
  makes the initial commit),
- `git`, `pandoc` and the editor from the `editor` template (default: LibreOffice via Flatpak) on `PATH`,
- `git config user.name` / `user.email` set, otherwise commits fail (the error says so).

SYW changes the process **current directory** to the document's directory on open, and runs all git and
pandoc commands there.

## How it works

### Session flow

```
Open(file)
  absolute path -> can fopen() it? -> cd to its directory -> is it a git repo? -> launch editor
  (any failure: Error set, published, session stays idle)

editor saves  ->  QFileSystemWatcher  ->  500 ms debounce  ->  Save()
  pandoc DOCX -> MD
  git add DOCX MD
  git diff --cached --quiet   (nothing changed? stop)
  ask for commit message      (empty / Cancel? skip, files stay staged)
  git commit -F <msgfile> -- DOCX MD
  git log per branch -> branch / commit / file tree

editor exits / window closes  ->  Exit(): final Save(), then Quit()
```

Only the DOCX and its Markdown file are committed; other staged files are left alone. **Nothing is pushed.**

### Viewing a snapshot

Select a single `.md` file in the tree (a commit's file node) and choose **View > Document** (or double-click
it). `SYW::FileAtCommit(hash, path, content)` runs `git show <hash>:<path>` and `MainWindow::viewDocument()`
renders the result in a separate, non-modal window, so several snapshots can be open side by side. The action
is enabled by `MainWindow::updateViewActions()` only for a single Markdown file that exists in its commit.
The viewer uses `QTextBrowser::setMarkdown()`, which renders CommonMark/GitHub-style Markdown.

### Configuration

Settings live in XML files with a `<SYW>` root, read and written with [XmlCls](https://github.com/dholstei/XmlCls)
(`SYW.cpp`; XmlCls is not included from `SYW.h`):

```xml
<SYW>
    <git_server>https://github.com</git_server>
    <git_user>dholstei</git_user>
    <git_repo>resume</git_repo>      <!-- per document; ~/.syw.xml has an empty <git_repo/> -->
    <editor>flatpak run --command=libreoffice org.libreoffice.LibreOffice --writer "%s"</editor>
    <to_Markdown>pandoc "%s.docx" -t markdown -o "%s.md"</to_Markdown>
    <diffLevel>Line</diffLevel>
    <auto_push value="1"/>           <!-- 0 = off, e.g. when working offline -->
</SYW>
```

| File | Loaded by | Purpose |
|------|-----------|---------|
| `~/.syw.xml` | `SYW::LoadConfig()` in `main()`, before any document | Defaults. Created from the compiled-in values if missing. |
| `<git dir>/.syw.xml` (`git rev-parse --absolute-git-dir`) | `SYW::LoadDocConfig()` inside `Open()`, before the editor is launched | Per-document overrides plus `<git_repo>`; inside `.git`, so never committed. |

The server (`git_server`, `git_user`) is shared by all documents, but the repository is not: `<git_repo>` (`SYW::git_repo`) is
given a value only in the per-document file; `~/.syw.xml` just declares an empty `<git_repo/>` (SaveConfig adds it if
missing and never overwrites what is there), and in the Configure dialog it is editable only when
"This document only" is chosen. If it is empty, the repository's directory name is used.

Missing elements keep their value, unknown elements are ignored (and survive a save), `diffLevel` is
`Word`, `Line` or `Paragraph` (case-insensitive). A broken file is reported and the previous values stay in force.
**File > Configure...** (`MainWindow::configure()`) edits the values and calls `SYW::SaveConfig(scope)`,
which writes all of them to the chosen file: an existing file is updated element by element with
`XmlNode::parse()` / `AddChild()`, a new one is built with `XmlDoc(std::string)` and `Save(path)`. Saving as a default
while a document has overrides writes the effective values.

### Pushing

**File > Push** (enabled while a document is open) calls `SYW::Push(summary)`; there is no confirmation.

```
git status -sb -uno                          first line gives branch...upstream [ahead N, behind M]
  upstream, ahead 0     -> nothing to do ("Everything is pushed")
  upstream, ahead > 0   -> git push
  no upstream           -> remote = origin, else the first remote
                           no remote at all -> git remote add origin <git_server>/<git_user>/<git_repo>.git   (repo = <git_repo> from .git/.syw.xml, else the directory name)
                           git push -u <remote> <branch>
```

`main()` sets `GIT_TERMINAL_PROMPT=0` and the `Shell` hook has no terminal, so git cannot ask for a password:
credentials must come from a credential helper or SSH keys, otherwise git's own message is shown. A detached HEAD or a
repository with no commits is refused.

### Auto push

With `<auto_push value="1"/>` (`SYW::auto_push`, on by default; also accepts `true/false`, `yes/no`, `on/off`) every
commit made by `Save()` is followed by `Push()` when a repository is associated with
the document: `<git_repo>` is set (the first push then creates `origin`) or the repository already has a remote.
A document with neither is left alone. A failed push is reported (the commit is safe) and the next commit tries
again; set `value="0"` in `~/.syw.xml` or, for one document, in `.git/.syw.xml` while offline. In the Configure dialog
the **Git repository** field is editable only for "This document only" (the directory name often differs from the
repository name, so it stays editable after the first push; a remote that already exists is used by Push as it is); **Auto push** is a check box in either scope. Success is shown in the
status bar through the `OnStatus` hook.

### Diffing two snapshots

Select exactly two `.md` file nodes (Ctrl-click, usually the same file in two commits) and choose
**View > Diff**. The flow:

```
MainWindow::viewDiff()
  SYW::OrderCommits(a, b, bIsOlder)   git log -1 --format=%ct, parsed with sscanf; same second -> git merge-base --is-ancestor
  SYW::DiffFiles(old, new, html)      two FileAtCommit() calls, then the DiffHtml hook at SYW::DiffLevel
    DiffToHtml()                      (MainWindow.cpp) diff_match_patch on tokens, rendered as HTML
```

`SYW::DiffLevel` (`enum SYW::diffLevel { Word, Line, Paragraph }`, default `Word`) sets the token size. Each
distinct token is encoded as one `QChar`, `diff_main()` compares those, `diff_cleanupSemantic()` tidies the
result and the tokens are decoded again. (`diff_charsToLines()` is private in diff-match-patch, so the encoding is
done in `Tokenize()`/`DiffToHtml()`.) Additions are green and underlined, deletions red and struck through;
Qt rich text has no `<ins>/<del>`, so spans are used. The popup lists both commits above the diff. The core
never sees diff-match-patch: it only knows the `DiffHtml` hook, so a different diff engine is a one-line swap.

### Hooks (the GUI boundary)

`SYW` never calls Qt. It calls these `std::function` members, which `MainWindow::wireSyw()` assigns. The
defaults are inert and fail safe (for example the default `Shell` returns -1), so a missing hook is an
error, not silent success.

| Hook | Signature | Purpose |
|------|-----------|---------|
| `GetFileName` | `std::string()` | Ask the user for a file (empty = cancelled) |
| `AbsPath` | `std::string(const std::string&)` | Absolute path |
| `CdToFile` | `bool(const std::string&)` | `chdir` to the file's directory |
| `BasePath` | `std::string(const std::string&)` | Path without extension |
| `Shell` | `int(const std::string& cmd, std::string& out)` | Run a command, capture stdout+stderr, return the **real exit code** (-1 = could not run) |
| `LaunchEditor` | `long(const std::string&)` | Start the editor, return its PID (0 = failed) |
| `GetCommitMessage` | `std::string()` | Commit message (empty = skip the commit) |
| `OnOpened`, `OnSaved`, `Quit` | `void()` | Notifications |

Command templates are printf-style strings in `SYW`:

```cpp
std::string editor      = "flatpak run --command=libreoffice org.libreoffice.LibreOffice --writer \"%s\"";
std::string to_Markdown = "pandoc \"%s.docx\" -t markdown -o \"%s.md\"";
```

`editor` receives the absolute file path, `to_Markdown` the path without extension (twice).

### Error contract

Every failure is recorded in `SYW::err` with `SetErr(level, msg, fmt, ...)`: a short `msg` plus a
printf-formatted `data` (paths, exit codes, command output). Each public operation clears `err` on entry
and publishes it **once** on exit with `MSG_ERR(err)`, which calls `Error::Publisher`:

- default (`SYW.cpp`): `printf()` for command-line use,
- Qt application: `main()` replaces it with a `QMessageBox` (icon by level).

Code that calls `Open()` or `Save()` can inspect `if (syw.err)` afterwards. In the code, operations use
`goto done` cleanup so every path publishes exactly once.

## Conventions

- Plain C-style string handling (`snprintf`, `strerror`, char buffers, truncation checked) where practical.
- Conventional, well-tested library calls for paths and processes (`QFileInfo`, `QDir`, `QProcess`).
- Paranoid error handling: every API failure ends up in `err` with `msg` and `data`.
- Commit messages go through a temporary file (`git commit -F`) to avoid shell-escaping problems.

## Known limitations

- `fopen()` on a UTF-8 path fails for non-ASCII names on Windows; the rest goes through Qt.
- `to_Markdown` and the commit step assume `<name>.docx` produces `<name>.md`.
- `git_server` / `git_user` are placeholders for push support; the plan is to manage properties with
  [XmlCls](https://github.com/dholstei/XmlCls).
- Commands are split with `QProcess::splitCommand()`; file paths containing a double quote are not supported.

## Roadmap

- **Windows build** (macOS after that). The code is Qt-portable; what needs work:
  - Build system: the GNU `Makefile` assumes Linux paths and `g++`. Needs a Windows toolchain (MSVC or MinGW)
    with Qt 6 for that compiler, and libxml2 (vcpkg, Conan or a local build; see the XmlCls README).
  - `diff_match_patch.h`: its `enum Operation { DELETE, INSERT, EQUAL }` collides with the `DELETE` macro from
    `<windows.h>`. Rename the enumerator in the submodule patch, or `#undef DELETE` before including it.
  - XmlCls is POSIX-only as written: `gmtime_r`, `<unistd.h>`, `<sys/file.h>`/`flock` (`FileLock`), `fsync`-style
    flushing and `access()` need Windows equivalents (`gmtime_s`, `LockFileEx`, `_commit`, `_access`).
  - `fopen()` on a UTF-8 path fails for non-ASCII names (use `_wfopen` with the path converted by Qt), in `SYW::Open()`
    and in the configuration-file checks.
  - Default `editor` template (`flatpak ...`) is Linux-only; ship a per-platform default (LibreOffice's `soffice.exe`).
  - Check quoting in the command templates under `QProcess::splitCommand()`, and git/pandoc being on `PATH`.
  - Re-check the greyed-out menu entries (palette workaround in `MainWindow::buildMenu()`) on both platforms.
- Actions on tree selections (checkout).
- Git actions navigation: branches, diffs, timeline.
- Repository creation from the application.
- **File > New**: create a new, empty DOCX in the same directory as the current document, then spawn a
  separate SYW process (`SYW --file <new.docx>`) with its own editor. One process per document matches the
  one-editor-per-session design of `SYW`, so the core does not change.