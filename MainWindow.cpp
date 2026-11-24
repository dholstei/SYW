/**
 * @file MainWindow.cpp
 * @brief Implementation of the Qt front end. See MainWindow.h for the hook table.
 */
#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QStatusBar>

/**
 * @details Creates the (initially disabled, read-only) log pane, builds the menu, wires the SYW hooks and
 * connects three signals:
 *  - QProcess::finished -> warn if the editor crashed or returned non-zero, then SYW::Exit(),
 *  - QFileSystemWatcher::fileChanged -> restart the debounce timer,
 *  - debounce timeout -> SYW::Save().
 *
 * LibreOffice saves by writing a temporary file and renaming it, which drops the path from the watcher;
 * that is why SYW::OnSaved re-arms it (see watchFile()).
 */
MainWindow::MainWindow(SYW& s, QWidget* parent)
    : QMainWindow(parent), syw(s)
{
    setWindowTitle("SYW");
    resize(900, 600);

    text = new QPlainTextEdit(this);
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlaceholderText("Open a file to launch the editor.");
    text->setEnabled(false);              // active only while an editor is running
    setCentralWidget(text);

    buildMenu();
    wireSyw();

    // Editor closed -> final save and quit.
    connect(&editorProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                text->setEnabled(false);
                if (status != QProcess::NormalExit || code != 0) {
                    syw.SetErr(WARN, "The editor exited abnormally", "%s, exit code %d",
                               status == QProcess::CrashExit ? "crashed" : "exited", code);
                    MSG_ERR(syw.err);
                }
                syw.Exit();
            });

    // Saves from LibreOffice are write-to-temp + rename, so the watcher loses the
    // path after each change; debounce, then Save() (whose OnSaved re-adds the path).
    debounce.setSingleShot(true);
    debounce.setInterval(500);
    connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this]() { debounce.start(); });
    connect(&debounce, &QTimer::timeout, this, [this]() { syw.Save(); });
}

/// @details **File > Open...** (standard Open shortcut) calls SYW::Open(); **File > Quit** closes the window.
void MainWindow::buildMenu()
{
    QMenu* file = menuBar()->addMenu("&File");

    QAction* open = file->addAction("&Open...");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this]() { syw.Open(); });

    file->addSeparator();

    QAction* quit = file->addAction("&Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
}

/**
 * @details Hooks that report failure (Shell, LaunchEditor) pass the exit code or PID back to SYW and put the
 * description in `Error` (via SYW::SetErr), so the error contract holds across the GUI boundary.
 * Command lines are split with QProcess::splitCommand(), so templates must quote paths containing spaces.
 */
void MainWindow::wireSyw()
{
    syw.GetFileName = [this]() {
        return QFileDialog::getOpenFileName(this, "Open document", QString(),
                                            "Word documents (*.docx);;All files (*)")
            .toStdString();
    };

    syw.AbsPath = [](const std::string& file) {
        return QFileInfo(QString::fromUtf8(file.c_str())).absoluteFilePath().toStdString();
    };

    syw.CdToFile = [](const std::string& file) {
        return QDir::setCurrent(QFileInfo(QString::fromUtf8(file.c_str())).absolutePath());
    };

    syw.BasePath = [](const std::string& file) {
        QFileInfo fi(QString::fromUtf8(file.c_str()));
        return fi.absoluteDir().filePath(fi.completeBaseName()).toStdString();
    };

    syw.GetCommitMessage = [this]() {
        bool ok = false;
        QString msg = QInputDialog::getMultiLineText(
            this, "Commit revision", "What changed, and why?",
            QDateTime::currentDateTime().toString("'Revision' yyyy-MM-dd hh:mm"), &ok);
        if (!ok) return std::string();          // cancelled: skip the commit
        return msg.trimmed().toStdString();
    };

    syw.Shell = [](const std::string& command, std::string& output) -> int {
        QStringList args = QProcess::splitCommand(QString::fromStdString(command));
        if (args.isEmpty()) { output = "Empty command"; return -1; }

        QString program = args.takeFirst();
        QProcess p;
        p.setProcessChannelMode(QProcess::MergedChannels);   // stdout + stderr
        p.start(program, args);                              // runs in the current directory
        if (!p.waitForStarted(5000)) {
            output = (program + ": " + p.errorString()).toStdString();
            return -1;
        }
        if (!p.waitForFinished(30000)) {
            p.kill();
            output = (program + ": timed out").toStdString();
            return -1;
        }
        output = QString::fromUtf8(p.readAll()).toStdString();
        if (p.exitStatus() != QProcess::NormalExit) return -1;
        return p.exitCode();
    };

    syw.LaunchEditor = [this](const std::string& command) -> long {
        QStringList args = QProcess::splitCommand(QString::fromStdString(command));
        if (args.isEmpty()) {
            syw.SetErr(ERR, "Editor command is empty", nullptr);
            return 0;
        }
        QString program = args.takeFirst();
        editorProc.start(program, args);
        if (!editorProc.waitForStarted(5000)) {
            syw.SetErr(ERR, "Could not launch the editor", "%s: %s",
                       program.toUtf8().constData(),
                       editorProc.errorString().toUtf8().constData());
            return 0;
        }
        return static_cast<long>(editorProc.processId());
    };

    syw.OnOpened = [this]() {
        text->setEnabled(true);
        statusBar()->showMessage("Editing " + QString::fromUtf8(syw.CurrentFile));
        watchFile();
        showLog();
    };

    syw.OnSaved = [this]() {
        watchFile();
        showLog();
    };

    syw.Quit = []() { QApplication::quit(); };
}

/// @details Adds SYW::CurrentFile to the watcher if it exists and is not already watched.
void MainWindow::watchFile()
{
    const QString path = QString::fromUtf8(syw.CurrentFile);
    if (path.isEmpty() || watcher.files().contains(path)) return;
    if (QFileInfo::exists(path)) watcher.addPath(path);
}

/// @details Replaces the contents of the log pane with SYW::LogText (UTF-8).
void MainWindow::showLog()
{
    text->setPlainText(QString::fromUtf8(syw.LogText.c_str()));
}

/**
 * @details Disconnects the editor's `finished` handler first so the shutdown is handled here exactly once
 * (otherwise the terminated editor would trigger a second SYW::Exit()).
 */
void MainWindow::closeEvent(QCloseEvent* event)
{
    if (editorProc.state() != QProcess::NotRunning) {
        editorProc.disconnect(this);       // we handle the shutdown ourselves
        editorProc.terminate();
        if (!editorProc.waitForFinished(3000)) editorProc.kill();
    }
    syw.Exit();                            // final Save() + Quit()
    event->accept();
}