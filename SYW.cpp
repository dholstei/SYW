#include "SYW.h"
#include "MainWindow.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QMessageBox>

// CLI default: print the error. main() replaces this with a Qt popup.
std::function<void(const Error&)> Error::Publisher = [](const Error& e) {
    printf("%s: %s", LvlStr(e.level).c_str(), e.msg.c_str());
    if (!e.data.empty()) printf(" (%s)", e.data.c_str());
    printf("\n");
};

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

void SYW::Save()
{
    std::string base, out;
    char cmd[8192];
    int n, rc;

    if (CurrentFile[0] == '\0') return;
    err = Error();

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

    RefreshLog();   // sets err itself on failure

done:
    OnSaved();      // GUI update / re-arm the file watcher, even after an error
    MSG_ERR(err);
}

void SYW::Exit()
{
    Save();
    EditorPID = 0;
    Quit();
}

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
    if (rc != 0) {    // e.g. a repository with no commits yet
        SetErr(WARN, "git log failed", "%s\n%s", top.c_str(), log.c_str());
        return false;
    }

    LogText = "Repository: " + top + "\n\n" + log;
    return true;
}