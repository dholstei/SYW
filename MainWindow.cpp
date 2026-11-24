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
#include <QInputDialog>
#include <QMenuBar>
#include <QSet>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {
/// Kind of a tree item, stored in `Qt::UserRole + 1`.
enum ItemKind { KindBranch = 0, KindCommit = 1, KindFile = 2 };

/// Extra per-item data on file items (`Qt::UserRole + n`).
enum FileData { RoleHash = Qt::UserRole + 2, RolePath = Qt::UserRole + 3, RoleStatus = Qt::UserRole + 4 };
}

/**
 * @details Creates the (initially disabled) branch/commit/file tree, builds the menu, wires the SYW hooks and
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

    tree = new QTreeWidget(this);
    tree->setColumnCount(3);
    tree->setHeaderLabels({"Branch / Commit / File", "Date", "Author"});
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);   // Ctrl/Shift: pick two snapshots later
    new QTreeWidgetItem(tree, QStringList("Open a file to launch the editor."));
    tree->setEnabled(false);              // active only while an editor is running
    setCentralWidget(tree);

    buildMenu();
    wireSyw();

    // Editor closed -> final save and quit.
    connect(&editorProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                tree->setEnabled(false);
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

/**
 * @details **File > Open...** (standard Open shortcut) calls SYW::Open(); **File > Quit** closes the window.
 * **View > Document** (disabled until a single Markdown file is selected, see updateViewActions()) calls
 * viewDocument(). Double-clicking a Markdown file in the tree does the same.
 */
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

    QMenu* view = menuBar()->addMenu("&View");

    viewDocumentAct = view->addAction("&Document");
    viewDocumentAct->setEnabled(false);
    connect(viewDocumentAct, &QAction::triggered, this, [this]() { viewDocument(); });

    connect(tree, &QTreeWidget::itemSelectionChanged, this, [this]() { updateViewActions(); });
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this]() {
        if (viewDocumentAct->isEnabled()) viewDocument();
    });
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
        tree->setEnabled(true);
        statusBar()->showMessage("Editing " + QString::fromUtf8(syw.CurrentFile));
        watchFile();
        showTree();
    };

    syw.OnSaved = [this]() {
        watchFile();
        showTree();
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

/**
 * @details Remembers which items are expanded (by key), clears the tree and rebuilds it from SYW::Branches:
 * one top-level item per branch (the current one in bold), one child per commit (hash and subject; date and
 * author in the other columns) and one grandchild per file (`[M] resume.docx`). An empty branch list means
 * the repository has no commits yet. A rename (`old<TAB>new`) is shown as `old -> new`.
 */
void MainWindow::showTree()
{
    // A refresh (after every save) must not collapse what the user has opened.
    QSet<QString> expanded;
    bool refresh = false;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const QString key = (*it)->data(0, Qt::UserRole).toString();
        if (key.isEmpty()) continue;                // placeholder row
        refresh = true;
        if ((*it)->isExpanded()) expanded.insert(key);
    }

    tree->clear();

    if (syw.Branches.empty()) {
        new QTreeWidgetItem(tree, QStringList("(no commits yet)"));
        updateViewActions();
        return;
    }

    for (const GitBranch& b : syw.Branches) {
        const QString bname = QString::fromUtf8(b.name.c_str());
        const QString bkey = "b:" + bname;

        QTreeWidgetItem* bi = new QTreeWidgetItem(tree);
        bi->setText(0, bname + (b.current ? "  (current)" : ""));
        bi->setData(0, Qt::UserRole, bkey);
        bi->setData(0, Qt::UserRole + 1, KindBranch);
        if (b.current) {
            QFont f = bi->font(0);
            f.setBold(true);
            bi->setFont(0, f);
        }

        for (const GitCommit& c : b.commits) {
            const QString hash = QString::fromUtf8(c.hash.c_str());
            const QString ckey = bkey + "/c:" + hash;

            QTreeWidgetItem* ci = new QTreeWidgetItem(bi);
            ci->setText(0, hash + "  " + QString::fromUtf8(c.subject.c_str()));
            ci->setText(1, QString::fromUtf8(c.date.c_str()));
            ci->setText(2, QString::fromUtf8(c.author.c_str()));
            ci->setData(0, Qt::UserRole, ckey);
            ci->setData(0, Qt::UserRole + 1, KindCommit);

            for (const GitFile& f : c.files) {
                QString path = QString::fromUtf8(f.path.c_str());
                path.replace('\t', " -> ");
                QTreeWidgetItem* fi = new QTreeWidgetItem(ci);
                fi->setText(0, "[" + QString::fromUtf8(f.status.c_str()) + "] " + path);
                fi->setData(0, Qt::UserRole, ckey + "/f:" + QString::fromUtf8(f.path.c_str()));
                fi->setData(0, Qt::UserRole + 1, KindFile);
                // For a rename ("old<TAB>new") the file at this commit is the new name.
                fi->setData(0, RoleHash, hash);
                fi->setData(0, RolePath, QString::fromUtf8(f.path.c_str()).section('\t', -1));
                fi->setData(0, RoleStatus, QString::fromUtf8(f.status.c_str()));
            }
            ci->setExpanded(expanded.contains(ckey));
        }
        bi->setExpanded(refresh ? expanded.contains(bkey) : b.current);
    }

    for (int col = 0; col < tree->columnCount(); ++col) tree->resizeColumnToContents(col);
    updateViewActions();
}

/**
 * @details Looks at the tree selection. **Document** is enabled when exactly one item is selected, it is a
 * file item, its path ends in `.md` (case-insensitive) and its status is not `D` (a deleted file does not
 * exist in that commit). The tree is rebuilt after every save, which clears the selection, so this also runs
 * at the end of showTree().
 */
void MainWindow::updateViewActions()
{
    const QList<QTreeWidgetItem*> sel = tree->selectedItems();
    bool doc = false;

    if (sel.size() == 1 && sel[0]->data(0, Qt::UserRole + 1).toInt() == KindFile) {
        const QString path = sel[0]->data(0, RolePath).toString();
        const QString status = sel[0]->data(0, RoleStatus).toString();
        doc = path.endsWith(".md", Qt::CaseInsensitive) && !status.startsWith('D');
    }
    viewDocumentAct->setEnabled(doc);
}

/**
 * @details Fetches the file with SYW::FileAtCommit() (errors are already published there) and shows it in a
 * QTextBrowser that is its own top-level window: non-modal, deleted when closed, and parented to the main
 * window so it goes away with it. QTextBrowser::setMarkdown() renders CommonMark/GitHub-style Markdown, so
 * pandoc-specific extensions may not render perfectly. Links are not followed.
 */
void MainWindow::viewDocument()
{
    const QList<QTreeWidgetItem*> sel = tree->selectedItems();
    if (sel.size() != 1 || !viewDocumentAct->isEnabled()) return;

    const QString hash = sel[0]->data(0, RoleHash).toString();
    const QString path = sel[0]->data(0, RolePath).toString();

    std::string content;
    if (!syw.FileAtCommit(hash.toStdString(), path.toUtf8().constData(), content)) return;

    QTextBrowser* viewer = new QTextBrowser(this);
    viewer->setWindowFlag(Qt::Window);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setOpenLinks(false);
    viewer->setWindowTitle(path + " @ " + hash);
    viewer->setMarkdown(QString::fromUtf8(content.c_str()));
    viewer->resize(700, 800);
    viewer->show();
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