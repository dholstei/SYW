/**
 * @file SYW.cpp
 * @brief SYW core implementation and the Qt application entry point (main()).
 *
 * The API is documented in SYW.h; the comments here describe how each part is implemented.
 */
#include "SYW.h"
#include "MainWindow.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QMessageBox>

/**
 * @brief Definition of the error publisher (declared in Error.h).
 *
 * @details This definition satisfies the linker and gives a CLI behaviour: print level, message and
 * data with printf(). The Qt application replaces it once, at the top of main(), with a popup.
 */
// CLI default: print the error. main() replaces this with a Qt popup.
std::function<void(const Error&)> Error::Publisher = [](const Error& e) {
    printf("%s: %s", LvlStr(e.level).c_str(), e.msg.c_str());
    if (!e.data.empty()) printf(" (%s)", e.data.c_str());
    printf("\n");
};

/**
 * @brief Application entry point.
 *
 * @details
 *  1. Creates the QApplication.
 *  2. Replaces Error::Publisher with a QMessageBox (icon by level, `msg` as the text, `data` as the
 *     detail text).
 *  3. Creates the SYW session and the MainWindow, which wires the SYW hooks to Qt.
 *  4. Handles the optional `--file <path>` argument by calling SYW::Open().
 *  5. Runs the Qt event loop until SYW::Quit is called.
 *
 * @param argc Argument count.
 * @param argv Arguments. Supported: `--file <path>` (open that document at startup).
 * @returns The exit code of the Qt event loop.
 */
int main(int argc, char** argv) {
    // Build Qt infrastructure and initialize the application
    QApplication app(argc, argv);

    // Qt app: errors become a popup
    Error::Publisher = [](const Error& e) {
        QMessageBox::Icon icon = QMessageBox::Information;
        if (e.level == WARN) icon = QMessageBox::Warning;
        if (e.level == ERR)  icon = QMessageBox::Critical;

        QMessageBox box(icon, QString::fromStdString(LvlStr(e.level)),
                        QString::fromStdString(e.msg), QMessageBox::Ok,
                        QApplication::activeWindow());
        if (!e.data.empty()) box.setInformativeText(QString::fromStdString(e.data));
        box.exec();
    };

    SYW syw;
    MainWindow window(syw);   // wires the SYW hooks to the GUI
    window.show();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            syw.Open(argv[i + 1]); ++i; // Skip the next argument since it's the file name
        }
    }

    // Main loop of the application (ends when SYW::Quit is called)
    return app.exec();
}

SYW::SYW(/* args */)
{
}

SYW::~SYW()
{
}

/**
 * @details Formats `fmt` into a 2 KiB buffer with vsnprintf() (truncating), then fills `err.level`,
 * `err.msg` and `err.data`. Does not publish.
 */
void SYW::SetErr(lvl level, const char* msg, const char* fmt, ...)
{
    char buf[2048];

    buf[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    }
    err.level = level;
    err.msg = msg;
    err.data = buf;
}

/**
 * @details Uses `goto done` so that every exit path, success or failure, publishes `err` exactly once.
 * On any failure after @ref CurrentFile was set, it is cleared again so the session returns to "no file
 * open". Command lines are built with snprintf() and checked for truncation.
 */
void SYW::Open(const std::string& file)
{
    std::string abs;
    FILE* fp;
    char cmd[8192];
    int n;

    err = Error();

    if (EditorPID != 0) {
        SetErr(WARN, "An editor is already running", "%s", CurrentFile);
        goto done;
    }
    if (file.empty()) {
        SetErr(ERR, "No file name given", nullptr);
        goto done;
    }

    abs = AbsPath(file);
    if (abs.size() >= sizeof CurrentFile) {
        SetErr(ERR, "File path too long", "%zu characters (max %zu): %s",
               abs.size(), sizeof CurrentFile - 1, abs.c_str());
        goto done;
    }

    fp = fopen(abs.c_str(), "rb");
    if (!fp) {
        SetErr(ERR, "Cannot open file", "%s: %s", abs.c_str(), strerror(errno));
        goto done;
    }
    fclose(fp);

    if (!CdToFile(abs)) {
        SetErr(ERR, "Cannot change to the file's directory", "%s", abs.c_str());
        goto done;
    }

    snprintf(CurrentFile, sizeof CurrentFile, "%s", abs.c_str());

    // The root DOCX/MD files are presumed to live in a root git repo; check before editing.
    if (!RefreshLog()) {
        CurrentFile[0] = '\0';
        goto done;
    }

    n = snprintf(cmd, sizeof cmd, editor.c_str(), CurrentFile);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "Editor command too long", "%s", CurrentFile);
        CurrentFile[0] = '\0';
        goto done;
    }

    EditorPID = LaunchEditor(cmd);
    if (EditorPID == 0) {
        if (!err) SetErr(ERR, "Could not launch the editor", "%s", cmd);
        CurrentFile[0] = '\0';
        goto done;
    }

    OnOpened();

done:
    MSG_ERR(err);
}

/**
 * @details Uses `goto refresh` (skip committing, still refresh the log) and `goto done` (stop, report).
 * The commit message is written to `syw-commit-msg.tmp` in the current directory and passed with
 * `git commit -F`, which avoids shell escaping of quotes and newlines; the file is removed afterwards,
 * also on failure. Exit codes from `git diff --cached --quiet`: 0 = nothing to commit, 1 = changes,
 * anything else = error.
 */
void SYW::Save()
{
    std::string base, out, msg;
    char cmd[8192], md[4096], msgfile[] = "syw-commit-msg.tmp";
    int n, rc, werr;
    FILE* fp;

    if (CurrentFile[0] == '\0') return;
    err = Error();

    // --- DOCX -> Markdown ---
    base = BasePath(CurrentFile);
    n = snprintf(cmd, sizeof cmd, to_Markdown.c_str(), base.c_str(), base.c_str());
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "Markdown conversion command too long", "%s", base.c_str());
        goto done;
    }

    rc = Shell(cmd, out);
    if (rc != 0) {
        SetErr(ERR, "Markdown conversion failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
        goto done;
    }

    n = snprintf(md, sizeof md, "%s.md", base.c_str());
    if (n < 0 || (size_t)n >= sizeof md) {
        SetErr(ERR, "Markdown file name too long", "%s", base.c_str());
        goto done;
    }

    // --- stage both files ---
    n = snprintf(cmd, sizeof cmd, "git add -- \"%s\" \"%s\"", CurrentFile, md);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "git add command too long", "%s", CurrentFile);
        goto done;
    }
    rc = Shell(cmd, out);
    if (rc != 0) {
        SetErr(ERR, "git add failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
        goto done;
    }

    // --- anything new to commit?  (git diff --cached --quiet: 0 = no, 1 = yes, else error) ---
    n = snprintf(cmd, sizeof cmd, "git diff --cached --quiet -- \"%s\" \"%s\"", CurrentFile, md);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "git diff command too long", "%s", CurrentFile);
        goto done;
    }
    rc = Shell(cmd, out);
    if (rc == 0) goto refresh;                  // unchanged, nothing to commit
    if (rc != 1) {
        SetErr(ERR, "git diff failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
        goto done;
    }

    // --- commit message (empty = the author declined; the files stay staged for next time) ---
    msg = GetCommitMessage();
    if (msg.empty()) goto refresh;

    // The message goes through a file (git commit -F) so quotes etc. need no shell escaping.
    n = snprintf(cmd, sizeof cmd, "git commit -F \"%s\" -- \"%s\" \"%s\"", msgfile, CurrentFile, md);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "git commit command too long", "%s", CurrentFile);
        goto done;
    }
    fp = fopen(msgfile, "wb");
    if (!fp) {
        SetErr(ERR, "Cannot create the commit message file", "%s: %s", msgfile, strerror(errno));
        goto done;
    }
    werr = 0;
    if (fwrite(msg.c_str(), 1, msg.size(), fp) != msg.size()) werr = errno ? errno : EIO;
    if (fclose(fp) != 0 && !werr) werr = errno ? errno : EIO;
    if (werr) {
        remove(msgfile);
        SetErr(ERR, "Cannot write the commit message file", "%s: %s", msgfile, strerror(werr));
        goto done;
    }

    rc = Shell(cmd, out);
    remove(msgfile);
    if (rc != 0) {      // e.g. user.name / user.email not configured
        SetErr(ERR, "git commit failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
        goto done;
    }

refresh:
    RefreshLog();   // sets err itself on failure

done:
    OnSaved();      // GUI update / re-arm the file watcher, even after an error
    MSG_ERR(err);
}

/**
 * @details Save() publishes its own error, so Exit() always proceeds to clear EditorPID and quit.
 */
void SYW::Exit()
{
    Save();
    EditorPID = 0;
    Quit();
}

/**
 * @details Runs, in the current directory:
 *  1. `git rev-parse --show-toplevel` - repository root. Distinguishes "could not run git" (exit code -1)
 *     from "not a repository" (any other non-zero code, with git's own message in `err.data`).
 *  2. `git symbolic-ref --short -q HEAD` - current branch (exit code 1 = detached HEAD).
 *  3. `git for-each-ref refs/heads` - local branch names (none = a repository with no commits yet).
 *  4. Per branch, `git log <branch> -n LogLimit --name-status` - the commits and the files each changed.
 *
 * The log is requested with a fixed header line per commit (`@hash|date|author|subject`) followed by
 * `status<TAB>path` lines, and parsed with strtok() and sscanf(). Results are collected in temporaries
 * and swapped into the members only when every step succeeded, so a failure never leaves a half-filled
 * list. `core.quotepath=off` keeps non-ASCII file names readable.
 */
bool SYW::RefreshLog()
{
    std::string top, cur, refs, log;
    std::vector<std::string> names;
    std::vector<GitBranch> branches;
    std::vector<char> buf;
    char cmd[8192], hash[64], date[32], author[256], subject[1024], status[16], path[4096];
    char* line;
    int rc, n;
    size_t i;

    // 1. repository root
    rc = Shell("git rev-parse --show-toplevel", top);
    if (rc < 0) {
        SetErr(ERR, "Could not run git", "%s", top.c_str());
        return false;
    }
    if (rc != 0) {
        SetErr(ERR, "Not in a git repository", "%s\n%s", CurrentFile, top.c_str());
        return false;
    }
    while (!top.empty() && (top.back() == '\n' || top.back() == '\r')) top.pop_back();

    // 2. current branch (exit code 1: detached HEAD)
    rc = Shell("git symbolic-ref --short -q HEAD", cur);
    if (rc == 0) {
        while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) cur.pop_back();
    } else if (rc == 1) {
        cur.clear();
    } else {
        SetErr(ERR, "git symbolic-ref failed", "exit code %d\n%s", rc, cur.c_str());
        return false;
    }

    // 3. local branches
    rc = Shell("git for-each-ref --format=\"%(refname:short)\" refs/heads", refs);
    if (rc != 0) {
        SetErr(ERR, "git for-each-ref failed", "exit code %d\n%s", rc, refs.c_str());
        return false;
    }
    buf.assign(refs.begin(), refs.end());
    buf.push_back('\0');
    for (line = strtok(buf.data(), "\n"); line; line = strtok(nullptr, "\n")) {
        n = (int)strlen(line);
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';
        if (line[0] == '\0') continue;
        if (strchr(line, '"')) {
            SetErr(WARN, "Skipping a branch with an unsupported name", "%s", line);
            continue;
        }
        names.push_back(line);
    }
    for (i = 0; i < names.size(); ++i)          // current branch first
        if (names[i] == cur) { std::swap(names[0], names[i]); break; }

    // 4. commits and files of each branch
    for (i = 0; i < names.size(); ++i) {
        GitBranch b;
        b.name = names[i];
        b.current = (b.name == cur);

        n = snprintf(cmd, sizeof cmd,
                     "git -c core.quotepath=off log \"%s\" -n %d --name-status --date=short "
                     "--pretty=format:\"@%%h|%%ad|%%an|%%s\" --",
                     b.name.c_str(), LogLimit);
        if (n < 0 || (size_t)n >= sizeof cmd) {
            SetErr(ERR, "git log command too long", "%s", b.name.c_str());
            return false;
        }
        rc = Shell(cmd, log);
        if (rc != 0) {
            SetErr(ERR, "git log failed", "branch %s, exit code %d\n%s", b.name.c_str(), rc, log.c_str());
            return false;
        }

        buf.assign(log.begin(), log.end());
        buf.push_back('\0');
        for (line = strtok(buf.data(), "\n"); line; line = strtok(nullptr, "\n")) {
            n = (int)strlen(line);
            if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';
            if (line[0] == '\0') continue;

            if (line[0] == '@') {                       // @hash|date|author|subject
                GitCommit c;
                subject[0] = '\0';
                n = sscanf(line + 1, "%63[^|]|%31[^|]|%255[^|]|%1023[^\n]", hash, date, author, subject);
                if (n < 3) {
                    SetErr(WARN, "Unrecognised git log line", "%s", line);
                    continue;
                }
                c.hash = hash;
                c.date = date;
                c.author = author;
                c.subject = subject;
                b.commits.push_back(c);
            } else if (!b.commits.empty()) {            // status<TAB>path
                GitFile f;
                if (sscanf(line, "%15[^\t]\t%4095[^\n]", status, path) != 2) {
                    SetErr(WARN, "Unrecognised git log line", "%s", line);
                    continue;
                }
                f.status = status;
                f.path = path;
                b.commits.back().files.push_back(f);
            }
        }
        branches.push_back(b);
    }

    Branches.swap(branches);
    RepoRoot = top;
    CurrentBranch = cur;
    return true;
}