/**
 * @file SYW.cpp
 * @brief SYW core implementation and the Qt application entry point (main()).
 *
 * The API is documented in SYW.h; the comments here describe how each part is implemented.
 */
#include "SYW.h"
#include "MainWindow.h"
#include "XmlCls.h"       // configuration files (libxml2 wrapper); kept out of the header

#include <cctype>
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
 *  3a. Loads the default configuration (SYW::LoadConfig()).
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

    // git runs without a terminal: fail with a message instead of waiting for a password nobody can type.
    qputenv("GIT_TERMINAL_PROMPT", "0");

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

    syw.LoadConfig();         // ~/.syw.xml; errors are published, the compiled-in defaults stay in force

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

    // Per-document settings may override the editor; a broken file was already reported by LoadDocConfig().
    LoadDocConfig();
    err = Error();

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
 * anything else = error. After a successful commit and the log refresh, Push() runs if @ref auto_push is on
 * and a repo is associated (@ref git_repo set, or a remote exists).
 */
void SYW::Save()
{
    std::string base, out, msg;
    char cmd[8192], md[4096], msgfile[] = "syw-commit-msg.tmp";
    int n, rc, werr;
    bool committed = false;
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
    committed = true;

refresh:
    RefreshLog();   // sets err itself on failure

    // Auto push: only for a document with an associated repo (git_repo set, or a remote exists), so a document
    // that was never published stays quiet. Push() clears and publishes err itself; keep ours for the end.
    if (committed && auto_push && (!git_repo.empty() || HasRemote())) {
        Error kept = err;
        std::string summary;
        if (Push(summary) && !summary.empty()) OnStatus(summary);
        err = kept;
    }

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

/**
 * @details The arguments end up inside a double-quoted command line, so they are validated first: the hash
 * must be 1-64 hexadecimal digits, the path non-empty and free of `"` and `\`. `git show` output (which
 * the Shell hook merges with stderr) is returned in `content` on success; on failure it is the error text
 * and goes into `err.data` instead. Uses `goto done` like the other operations so `err` is published once.
 */
bool SYW::FileAtCommit(const std::string& hash, const std::string& path, std::string& content)
{
    char cmd[8192];
    size_t i;
    int n, rc;
    bool ok = false;

    err = Error();
    content.clear();

    if (hash.empty() || hash.size() > 64) {
        SetErr(ERR, "Invalid commit hash", "\"%s\"", hash.c_str());
        goto done;
    }
    for (i = 0; i < hash.size(); ++i) {
        if (!isxdigit((unsigned char)hash[i])) {
            SetErr(ERR, "Invalid commit hash", "\"%s\"", hash.c_str());
            goto done;
        }
    }
    if (path.empty() || path.find_first_of("\"\\") != std::string::npos) {
        SetErr(ERR, "Unsupported file path", "\"%s\"", path.c_str());
        goto done;
    }

    n = snprintf(cmd, sizeof cmd, "git show \"%s:%s\"", hash.c_str(), path.c_str());
    if (n < 0 || (size_t)n >= sizeof cmd) {
        SetErr(ERR, "git show command too long", "%s:%s", hash.c_str(), path.c_str());
        goto done;
    }

    rc = Shell(cmd, content);
    if (rc != 0) {
        SetErr(ERR, rc < 0 ? "Could not run git show" : "git show failed", "exit code %d\n%s\n%s",
               rc, cmd, content.c_str());
        content.clear();
        goto done;
    }
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

/// True if `s` is 1-64 hexadecimal digits (a git object name, or an abbreviation of one).
static bool IsHexHash(const std::string& s)
{
    size_t i;

    if (s.empty() || s.size() > 64) return false;
    for (i = 0; i < s.size(); ++i)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/**
 * @details Reads the committer timestamp of each commit with `git log -1 --format=%ct <hash>` (parsed with
 * sscanf) and compares them. Commits made in the same second are ordered by ancestry instead:
 * `git merge-base --is-ancestor <b> <a>` exits 0 if `b` is an ancestor of `a` (so `b` is older) and 1 if not;
 * any other exit code is an error. Commits on unrelated branches are ordered by time alone.
 */
bool SYW::OrderCommits(const std::string& a, const std::string& b, bool& bIsOlder)
{
    const std::string* hash[2] = { &a, &b };
    long long when[2] = { 0, 0 };
    std::string out;
    char cmd[512];
    int i, n, rc;
    bool ok = false;

    err = Error();
    bIsOlder = false;

    for (i = 0; i < 2; ++i) {
        if (!IsHexHash(*hash[i])) {
            SetErr(ERR, "Invalid commit hash", "\"%s\"", hash[i]->c_str());
            goto done;
        }
        n = snprintf(cmd, sizeof cmd, "git log -1 --format=%%ct %s", hash[i]->c_str());
        if (n < 0 || (size_t)n >= sizeof cmd) {
            SetErr(ERR, "git log command too long", "%s", hash[i]->c_str());
            goto done;
        }
        rc = Shell(cmd, out);
        if (rc != 0) {
            SetErr(ERR, rc < 0 ? "Could not run git log" : "git log failed", "exit code %d\n%s\n%s",
                   rc, cmd, out.c_str());
            goto done;
        }
        if (sscanf(out.c_str(), "%lld", &when[i]) != 1) {
            SetErr(ERR, "Unexpected git log output", "%s\n%s", cmd, out.c_str());
            goto done;
        }
    }

    if (when[0] == when[1] && a != b) {
        n = snprintf(cmd, sizeof cmd, "git merge-base --is-ancestor %s %s", b.c_str(), a.c_str());
        if (n < 0 || (size_t)n >= sizeof cmd) {
            SetErr(ERR, "git merge-base command too long", "%s %s", a.c_str(), b.c_str());
            goto done;
        }
        rc = Shell(cmd, out);
        if (rc != 0 && rc != 1) {
            SetErr(ERR, rc < 0 ? "Could not run git merge-base" : "git merge-base failed",
                   "exit code %d\n%s\n%s", rc, cmd, out.c_str());
            goto done;
        }
        bIsOlder = (rc == 0);
    } else {
        bIsOlder = when[1] < when[0];
    }
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

/**
 * @details FileAtCommit() validates its arguments and publishes its own errors, so a failure there returns
 * immediately without a second message. A failure of the DiffHtml hook is recorded here, with the hook's
 * explanation (it puts it in `html`) as `err.data`.
 */
bool SYW::DiffFiles(const std::string& oldHash, const std::string& oldPath,
                    const std::string& newHash, const std::string& newPath, std::string& html)
{
    std::string oldText, newText;

    err = Error();
    html.clear();

    if (!FileAtCommit(oldHash, oldPath, oldText)) return false;     // error already published
    if (!FileAtCommit(newHash, newPath, newText)) return false;

    if (!DiffHtml(oldText, newText, DiffLevel, html)) {
        SetErr(ERR, "Could not compute the diff", "%s", html.c_str());
        html.clear();
        MSG_ERR(err);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Configuration files
// ---------------------------------------------------------------------------------------------------

namespace {
/// A string setting stored as `<name>text</name>` under the `<SYW>` root.
struct ConfigKey { const char* name; std::string SYW::* member; };
const ConfigKey ConfigKeys[] = {
    { "git_server",  &SYW::git_server  },
    { "git_user",    &SYW::git_user    },
    { "git_repo",    &SYW::git_repo    },     // per document: only the per-document file writes it
    { "editor",      &SYW::editor      },
    { "to_Markdown", &SYW::to_Markdown },
};
const char* const DiffLevelNames[] = { "Word", "Line", "Paragraph" };   // index == SYW::diffLevel

/// Text with the five XML special characters replaced by entities.
std::string XmlEscape(const std::string& s)
{
    std::string out;
    size_t i;

    out.reserve(s.size() + 16);
    for (i = 0; i < s.size(); ++i) {
        switch (s[i]) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += s[i];
        }
    }
    return out;
}

/// Case-insensitive match of `text` (surrounding white space ignored) against the diff level names.
bool ParseDiffLevel(const char* text, SYW::diffLevel& level)
{
    char word[32];
    int i;
    size_t k;

    if (sscanf(text, "%31s", word) != 1) return false;
    for (i = 0; i < 3; ++i) {
        const char* name = DiffLevelNames[i];
        if (strlen(name) != strlen(word)) continue;
        for (k = 0; name[k] && tolower((unsigned char)name[k]) == tolower((unsigned char)word[k]); ++k) {}
        if (!name[k]) { level = (SYW::diffLevel)i; return true; }
    }
    return false;
}

/// Parse `1/0`, `true/false`, `yes/no`, `on/off` (case-insensitive, white space ignored).
bool ParseBool(const char* text, bool& value)
{
    static const char* const yes[] = { "1", "true", "yes", "on" };
    static const char* const no[]  = { "0", "false", "no", "off" };
    char word[16];
    size_t k;
    int i;

    if (sscanf(text, "%15s", word) != 1) return false;
    for (k = 0; word[k]; ++k) word[k] = (char)tolower((unsigned char)word[k]);
    for (i = 0; i < 4; ++i) {
        if (strcmp(word, yes[i]) == 0) { value = true;  return true; }
        if (strcmp(word, no[i]) == 0)  { value = false; return true; }
    }
    return false;
}

/// `/SYW/<key>` exists in the document?
bool HasKey(XmlDoc& xd, const char* key)
{
    char q[128];
    snprintf(q, sizeof q, "/SYW/%s", key);
    return xd.XPath<bool>(q);
}

/// Text of `/SYW/<key>`.
std::string KeyText(XmlDoc& xd, const char* key)
{
    char q[128];
    snprintf(q, sizeof q, "string(/SYW/%s)", key);
    return xd.XPath<std::string>(q);
}
}

/**
 * @details Opens the file with the XmlDoc constructor (parse errors arrive in `XmlDoc::err`), checks the root
 * element, then for every key that is present copies its text into the member. `diffLevel` is matched
 * case-insensitively against `Word`, `Line`, `Paragraph`; anything else is an error and leaves @ref DiffLevel
 * unchanged. Keys are applied one by one, so an error part-way leaves the earlier keys applied.
 */
bool SYW::ReadConfigFile(const char* path)
{
    char cmd[256];
    int k;

    XmlDoc xd(path);
    if (xd.err) {
        SetErr(ERR, "Cannot parse the configuration file", "%s\n%s", path, xd.err.msg.c_str());
        return false;
    }
    if (!xd.XPath<bool>("/SYW")) {
        SetErr(ERR, "Not a SYW configuration file (root element must be <SYW>)", "%s", path);
        return false;
    }

    for (k = 0; k < (int)(sizeof ConfigKeys / sizeof ConfigKeys[0]); ++k) {
        if (!HasKey(xd, ConfigKeys[k].name)) continue;
        const std::string text = KeyText(xd, ConfigKeys[k].name);
        if (xd.err) {
            SetErr(ERR, "Cannot read a configuration value", "%s: %s\n%s", path, ConfigKeys[k].name, xd.err.msg.c_str());
            return false;
        }
        this->*(ConfigKeys[k].member) = text;
    }

    if (HasKey(xd, "diffLevel")) {
        const std::string text = KeyText(xd, "diffLevel");
        diffLevel level;
        if (xd.err) {
            SetErr(ERR, "Cannot read a configuration value", "%s: diffLevel\n%s", path, xd.err.msg.c_str());
            return false;
        }
        if (!ParseDiffLevel(text.c_str(), level)) {
            snprintf(cmd, sizeof cmd, "%.200s", text.c_str());
            SetErr(ERR, "Invalid diffLevel (use Word, Line or Paragraph)", "%s: \"%s\"", path, cmd);
            return false;
        }
        DiffLevel = level;
    }

    if (HasKey(xd, "auto_push")) {            // <auto_push value="1"/>  (element text is accepted too)
        std::string text = xd.XPath<std::string>("string(/SYW/auto_push/@value)");
        if (!xd.err && text.empty()) text = KeyText(xd, "auto_push");
        bool value = false;
        if (xd.err) {
            SetErr(ERR, "Cannot read a configuration value", "%s: auto_push\n%s", path, xd.err.msg.c_str());
            return false;
        }
        if (!ParseBool(text.c_str(), value)) {
            snprintf(cmd, sizeof cmd, "%.200s", text.c_str());
            SetErr(ERR, "Invalid auto_push (use value=\"1\" or value=\"0\")", "%s: \"%s\"", path, cmd);
            return false;
        }
        auto_push = value;
    }
    return true;
}

/**
 * @details If the file exists it is opened and every key is either replaced with XmlNode::parse() (an element's
 * text is changed by replacing the element) or added with XmlNode::AddChild(); elements this program does
 * not know are left alone. Otherwise a new document is built from a minimal string with the string
 * constructor and given its filename by `Save(path)`. All text is XML-escaped.
 */
bool SYW::WriteConfigFile(const char* path, bool document)
{
    std::string el[8];
    const char* names[8];
    bool keepExisting[8] = { false, false, false, false, false, false, false, false };   // add if missing, never overwrite
    FILE* fp;
    int k, nstr, count;

    nstr = 0;
    for (k = 0; k < (int)(sizeof ConfigKeys / sizeof ConfigKeys[0]); ++k) {
        names[nstr] = ConfigKeys[k].name;
        if (!document && strcmp(names[nstr], "git_repo") == 0) {
            // The repository belongs to one document: the default file only declares an empty <git_repo/>
            // (and keeps whatever the user wrote there), it never receives this document's value.
            el[nstr] = "<git_repo/>";
            keepExisting[nstr] = true;
        } else {
            el[nstr] = std::string("<") + names[nstr] + ">" + XmlEscape(this->*(ConfigKeys[k].member)) + "</" + names[nstr] + ">";
        }
        ++nstr;
    }
    names[nstr] = "diffLevel";
    el[nstr] = std::string("<diffLevel>") + DiffLevelNames[DiffLevel] + "</diffLevel>";
    names[nstr + 1] = "auto_push";
    el[nstr + 1] = auto_push ? "<auto_push value=\"1\"/>" : "<auto_push value=\"0\"/>";
    count = nstr + 2;

    fp = fopen(path, "rb");
    if (fp) {                                               // update in place
        fclose(fp);
        XmlDoc xd(path);
        if (xd.err) {
            SetErr(ERR, "Cannot parse the configuration file", "%s\n%s", path, xd.err.msg.c_str());
            return false;
        }
        if (!xd.XPath<bool>("/SYW")) {
            SetErr(ERR, "Not a SYW configuration file (root element must be <SYW>)", "%s", path);
            return false;
        }
        for (k = 0; k < count; ++k) {
            char q[128];
            snprintf(q, sizeof q, "/SYW/%s", names[k]);
            if (HasKey(xd, names[k])) {
                if (keepExisting[k]) continue;
                XmlNode n = xd.XPath<XmlNode>(q);
                if (!n.err) n.parse(el[k]);
                if (n.err) {
                    SetErr(ERR, "Cannot update a configuration value", "%s: %s\n%s", path, names[k], n.err.msg.c_str());
                    return false;
                }
            } else {
                XmlNode root = xd.XPath<XmlNode>("/SYW");
                if (!root.err) root.AddChild(el[k]);
                if (root.err) {
                    SetErr(ERR, "Cannot add a configuration value", "%s: %s\n%s", path, names[k], root.err.msg.c_str());
                    return false;
                }
            }
        }
        xd.Save();
        if (xd.err) {
            SetErr(ERR, "Cannot write the configuration file", "%s\n%s", path, xd.err.msg.c_str());
            return false;
        }
        return true;
    }

    std::string xml = "<SYW>";                              // new file
    for (k = 0; k < count; ++k) xml += el[k];
    xml += "</SYW>";
    XmlDoc nd(xml);                                         // std::string: parsed from memory
    if (nd.err) {
        SetErr(ERR, "Cannot build the configuration document", "%s", nd.err.msg.c_str());
        return false;
    }
    nd.Save(path);                                          // gives the document its filename
    if (nd.err) {
        SetErr(ERR, "Cannot write the configuration file", "%s\n%s", path, nd.err.msg.c_str());
        return false;
    }
    return true;
}

/// @details Path is `<HomeDir()>/.syw.xml`. A file that does not exist yet is created from the current values.
bool SYW::LoadConfig()
{
    std::string home = HomeDir();
    FILE* fp;
    bool ok = false;

    err = Error();

    if (home.empty()) {
        SetErr(WARN, "Cannot find the home directory, using the built-in settings", nullptr);
        goto done;
    }
    DefaultConfigPath = home + "/.syw.xml";

    fp = fopen(DefaultConfigPath.c_str(), "rb");
    if (fp) {
        fclose(fp);
        if (!ReadConfigFile(DefaultConfigPath.c_str())) goto done;
    } else if (errno == ENOENT) {
        if (!WriteConfigFile(DefaultConfigPath.c_str(), false)) goto done;
    } else {
        SetErr(ERR, "Cannot open the configuration file", "%s: %s", DefaultConfigPath.c_str(), strerror(errno));
        goto done;
    }
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

/**
 * @details `git rev-parse --absolute-git-dir` gives the real git directory (also for worktrees and
 * submodules, where `.git` is a file); the configuration file is `.syw.xml` inside it, so it is never part
 * of a commit.
 */
bool SYW::LoadDocConfig()
{
    std::string out;
    char dir[4096];
    FILE* fp;
    int rc;
    bool ok = false;

    err = Error();
    DocConfigPath.clear();

    rc = Shell("git rev-parse --absolute-git-dir", out);
    if (rc != 0) {
        SetErr(ERR, rc < 0 ? "Could not run git rev-parse" : "git rev-parse failed", "exit code %d\n%s", rc, out.c_str());
        goto done;
    }
    if (sscanf(out.c_str(), "%4095[^\r\n]", dir) != 1) {
        SetErr(ERR, "Unexpected git rev-parse output", "%s", out.c_str());
        goto done;
    }
    DocConfigPath = std::string(dir) + "/.syw.xml";

    fp = fopen(DocConfigPath.c_str(), "rb");
    if (fp) {
        fclose(fp);
        if (!ReadConfigFile(DocConfigPath.c_str())) goto done;
    }
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

bool SYW::SaveConfig(configScope scope)
{
    const std::string& path = (scope == ConfigDocument) ? DocConfigPath : DefaultConfigPath;
    bool ok = false;

    err = Error();

    if (path.empty()) {
        SetErr(ERR, scope == ConfigDocument ? "No document is open" : "The home directory is unknown",
               "There is no configuration file to write to.");
        goto done;
    }
    ok = WriteConfigFile(path.c_str(), scope == ConfigDocument);

done:
    MSG_ERR(err);
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// git push
// ---------------------------------------------------------------------------------------------------

/**
 * @details The first line of `git status -sb` has the forms
 *   - `## main...origin/main [ahead 2, behind 1]` (upstream; the bracket is optional, `[gone]` = upstream deleted),
 *   - `## main` (no upstream),
 *   - `## No commits yet on main`, `## HEAD (no branch)`.
 * It is split with strstr() and sscanf(). Untracked files are ignored (`-uno`) to keep it fast.
 */
bool SYW::UpstreamStatus(std::string& upstream, int& ahead, int& behind)
{
    std::string out;
    char first[1024], up[512];
    const char* p;
    int rc;
    bool ok = false;

    upstream.clear();
    ahead = behind = 0;
    err = Error();

    rc = Shell("git status -sb -uno", out);
    if (rc != 0) {
        SetErr(ERR, rc < 0 ? "Could not run git status" : "git status failed", "exit code %d\n%s", rc, out.c_str());
        goto done;
    }
    if (sscanf(out.c_str(), "%1023[^\r\n]", first) != 1 || strncmp(first, "## ", 3) != 0) {
        SetErr(ERR, "Unexpected git status output", "%s", out.c_str());
        goto done;
    }
    if (strncmp(first, "## No commits yet", 17) == 0) {
        SetErr(WARN, "Nothing to push: there are no commits yet", nullptr);
        goto done;
    }
    if (strncmp(first, "## HEAD (no branch)", 19) == 0) {
        SetErr(ERR, "Cannot push a detached HEAD", "Check out a branch first.");
        goto done;
    }

    p = strstr(first, "...");
    if (p && sscanf(p + 3, "%511s", up) == 1 && !strstr(first, "[gone]")) {
        upstream = up;
        if ((p = strstr(first, "ahead ")) != nullptr)  sscanf(p, "ahead %d", &ahead);
        if ((p = strstr(first, "behind ")) != nullptr) sscanf(p, "behind %d", &behind);
    }
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

/**
 * @details Remote and branch names go into double-quoted command lines, so names containing `"` or `\` are
 * refused. The first `git remote` line is used when there is no `origin`. `<repository directory name>` is
 * @ref git_repo, or the last component of @ref RepoRoot when that is empty. `-u` makes later pushes plain `git push`.
 */
bool SYW::Push(std::string& summary)
{
    std::string upstream, out, remote;
    char cmd[8192], url[2048], names[1024];
    const char* base;
    char* line;
    int ahead = 0, behind = 0, rc, n;
    bool ok = false;

    err = Error();
    summary.clear();

    if (RepoRoot.empty() || CurrentBranch.empty()) {
        SetErr(WARN, "Nothing to push", "Open a document on a branch first.");
        goto done;
    }
    if (CurrentBranch.find_first_of("\"\\") != std::string::npos) {
        SetErr(ERR, "Unsupported branch name", "%s", CurrentBranch.c_str());
        goto done;
    }

    if (!UpstreamStatus(upstream, ahead, behind)) { ok = false; return false; }   // already published
    err = Error();

    if (!upstream.empty()) {
        if (ahead == 0) {
            if (behind) snprintf(url, sizeof url, "Nothing to push (%d new on %s; pull needed)", behind, upstream.c_str());
            else        snprintf(url, sizeof url, "Everything is pushed (%s)", upstream.c_str());
            summary = url;
            ok = true;
            goto done;
        }
        rc = Shell("git push", out);
        if (rc != 0) {
            SetErr(ERR, rc < 0 ? "Could not run git push" : "git push failed", "exit code %d\n%s", rc, out.c_str());
            goto done;
        }
        snprintf(url, sizeof url, "Pushed %d commit%s to %s", ahead, ahead == 1 ? "" : "s", upstream.c_str());
        summary = url;
        ok = true;
        goto done;
    }

    // No upstream yet: choose or create a remote, then push with -u.
    rc = Shell("git remote", out);
    if (rc != 0) {
        SetErr(ERR, rc < 0 ? "Could not run git remote" : "git remote failed", "exit code %d\n%s", rc, out.c_str());
        goto done;
    }
    for (line = strtok(&out[0], "\r\n"); line; line = strtok(nullptr, "\r\n")) {
        if (strcmp(line, "origin") == 0) { remote = line; break; }
        if (remote.empty()) remote = line;
    }

    if (remote.empty()) {
        std::string server = git_server;
        while (!server.empty() && server.back() == '/') server.pop_back();
        if (!git_repo.empty()) {
            base = git_repo.c_str();
        } else {                                // fall back to the repository directory's name
            base = strrchr(RepoRoot.c_str(), '/');
            base = base ? base + 1 : RepoRoot.c_str();
        }
        n = (git_repo.size() > 4 && git_repo.compare(git_repo.size() - 4, 4, ".git") == 0)      // "name.git" is accepted too
            ? snprintf(url, sizeof url, "%s/%s/%s", server.c_str(), git_user.c_str(), base)
            : snprintf(url, sizeof url, "%s/%s/%s.git", server.c_str(), git_user.c_str(), base);
        if (n < 0 || (size_t)n >= sizeof url || strpbrk(url, "\"\\")) {
            SetErr(ERR, "Cannot build the remote URL", "git_server=%s git_user=%s", git_server.c_str(), git_user.c_str());
            goto done;
        }
        n = snprintf(cmd, sizeof cmd, "git remote add origin \"%s\"", url);
        if (n < 0 || (size_t)n >= sizeof cmd) { SetErr(ERR, "git remote command too long", "%s", url); goto done; }
        rc = Shell(cmd, out);
        if (rc != 0) {
            SetErr(ERR, rc < 0 ? "Could not run git remote add" : "git remote add failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
            goto done;
        }
        remote = "origin";
    }
    if (remote.find_first_of("\"\\") != std::string::npos) {
        SetErr(ERR, "Unsupported remote name", "%s", remote.c_str());
        goto done;
    }

    n = snprintf(cmd, sizeof cmd, "git push -u \"%s\" \"%s\"", remote.c_str(), CurrentBranch.c_str());
    if (n < 0 || (size_t)n >= sizeof cmd) { SetErr(ERR, "git push command too long", "%s", CurrentBranch.c_str()); goto done; }
    rc = Shell(cmd, out);
    if (rc != 0) {
        SetErr(ERR, rc < 0 ? "Could not run git push" : "git push failed", "exit code %d\n%s\n%s", rc, cmd, out.c_str());
        goto done;
    }
    snprintf(names, sizeof names, "Pushed %s to %s and set it as upstream", CurrentBranch.c_str(), remote.c_str());
    summary = names;
    ok = true;

done:
    MSG_ERR(err);
    return ok;
}

/// @details `git remote` prints one remote per line; any output with exit code 0 means there is one.
bool SYW::HasRemote()
{
    std::string out;
    int rc = Shell("git remote", out);

    return rc == 0 && out.find_first_not_of(" \t\r\n") != std::string::npos;
}