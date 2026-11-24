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
 * @details `git rev-parse --show-toplevel` distinguishes "could not run git" (exit code -1) from "not a
 * repository" (any other non-zero code, with git's own message in `err.data`). When `git log` fails,
 * `git rev-parse --verify --quiet HEAD` (exit code 1 = no HEAD) identifies a repository with no commits.
 */
bool SYW::RefreshLog()
{
    std::string top, log;
    int rc;

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

    rc = Shell("git log --graph --decorate --date=short -n 200 --pretty=format:\"%h %ad %an  %s%d\"", log);
    if (rc < 0) {
        SetErr(ERR, "Could not run git log", "%s", log.c_str());
        return false;
    }
    if (rc != 0) {
        // A repository with no commits yet is not an error: the first Save() makes the initial commit.
        // (git rev-parse --verify --quiet HEAD: exit code 1 = HEAD does not exist)
        std::string head;
        if (Shell("git rev-parse --verify --quiet HEAD", head) == 1) {
            LogText = "Repository: " + top + "\n\n(no commits yet)";
            return true;
        }
        SetErr(WARN, "git log failed", "%s\n%s", top.c_str(), log.c_str());
        return false;
    }

    LogText = "Repository: " + top + "\n\n" + log;
    return true;
}