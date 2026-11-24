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
| `MainWindow.h` / `MainWindow.cpp` | **Qt front end**: File menu, git log pane, editor process, file watching. Wires the hooks. |
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
  git log -> log pane

editor exits / window closes  ->  Exit(): final Save(), then Quit()
```

Only the DOCX and its Markdown file are committed; other staged files are left alone. **Nothing is pushed.**

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

- Rendered Markdown view (`QTextBrowser::setMarkdown()`), including past revisions.
- Git actions navigation: branches, diffs, timeline.
- `git push` once a remote is configured (`git remote add origin <url>`).
- Repository creation from the application.
