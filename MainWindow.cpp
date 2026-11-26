/**
 * @file MainWindow.cpp
 * @brief Implementation of the Qt front end. See MainWindow.h for the hook table.
 */
#include "MainWindow.h"

#include "diff_match_patch.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMenuBar>
#include <QHash>
#include <QSet>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {
/// Kind of a tree item, stored in `Qt::UserRole + 1`.
enum ItemKind { KindBranch = 0, KindCommit = 1, KindFile = 2 };

/// Extra per-item data on file items (`Qt::UserRole + n`).
enum FileData { RoleHash = Qt::UserRole + 2, RolePath = Qt::UserRole + 3, RoleStatus = Qt::UserRole + 4 };

/// Is this file item a Markdown file that exists in its commit (status not `D`)?
bool IsViewableMarkdown(const QTreeWidgetItem* item)
{
    if (item->data(0, Qt::UserRole + 1).toInt() != KindFile) return false;
    const QString path = item->data(0, RolePath).toString();
    const QString status = item->data(0, RoleStatus).toString();
    return path.endsWith(".md", Qt::CaseInsensitive) && !status.startsWith('D');
}

/**
 * @brief Split text into diff tokens.
 *
 * - SYW::Word: a run of letters/digits/underscore, a run of white space, or one other character.
 * - SYW::Line: text up to and including each newline.
 * - SYW::Paragraph: lines up to and including the blank line(s) that end them.
 *
 * Concatenating the tokens always gives back the input exactly.
 */
QStringList Tokenize(const QString& text, SYW::diffLevel level)
{
    QStringList tok;
    const int n = int(text.size());
    int i = 0;

    if (level == SYW::Word) {
        while (i < n) {
            int j = i + 1;
            const QChar c = text[i];
            if (c.isLetterOrNumber() || c == '_') {
                while (j < n && (text[j].isLetterOrNumber() || text[j] == '_')) ++j;
            } else if (c.isSpace()) {
                while (j < n && text[j].isSpace()) ++j;
            }
            tok << text.mid(i, j - i);
            i = j;
        }
        return tok;
    }

    // Line and Paragraph: first cut into lines (keeping the '\n').
    QStringList lines;
    while (i < n) {
        int j = text.indexOf('\n', i);
        j = (j < 0) ? n : j + 1;
        lines << text.mid(i, j - i);
        i = j;
    }
    if (level == SYW::Line) return lines;

    // Paragraph: a blank line closes the paragraph; consecutive blank lines belong to it.
    QString cur;
    bool sawBlank = false;
    for (const QString& ln : lines) {
        const bool blank = ln.trimmed().isEmpty();
        if (sawBlank && !blank) { tok << cur; cur.clear(); sawBlank = false; }
        cur += ln;
        if (blank) sawBlank = true;
    }
    if (!cur.isEmpty()) tok << cur;
    return tok;
}

/**
 * @brief Diff two texts with diff_match_patch at the given granularity and render HTML.
 *
 * Each distinct token is mapped to one QChar (1, 2, 3...), so diff_main() compares tokens, not characters.
 * This is the same trick diff_match_patch uses internally for its line mode, repeated here because
 * its `diff_charsToLines()` is private. The result is cleaned with diff_cleanupSemantic() while still
 * encoded, then decoded token by token.
 *
 * Qt rich text has no `<ins>/<del>`, so changes are `<span>`s with a background colour (plus underline /
 * line-through so they do not rely on colour alone). The whole thing is inside `<pre style="white-space:
 * pre-wrap">` so line breaks and indentation of the Markdown source survive.
 *
 * @returns `true` on success; on failure `html` holds the reason.
 */
bool DiffToHtml(const std::string& oldText, const std::string& newText, SYW::diffLevel level, std::string& html)
{
    const QStringList tokOld = Tokenize(QString::fromUtf8(oldText.c_str(), int(oldText.size())), level);
    const QStringList tokNew = Tokenize(QString::fromUtf8(newText.c_str(), int(newText.size())), level);

    QHash<QString, int> code;       // token -> 1..N
    QStringList table;              // code - 1 -> token
    const int kMaxTokens = 0xD7FF;  // stay below the UTF-16 surrogate range

    auto encode = [&](const QStringList& tokens, QString& out) -> bool {
        out.reserve(int(tokens.size()));
        for (const QString& t : tokens) {
            int k = code.value(t, 0);
            if (k == 0) {
                if (table.size() >= kMaxTokens) return false;
                table << t;
                k = int(table.size());
                code.insert(t, k);
            }
            out += QChar(ushort(k));
        }
        return true;
    };

    QString encOld, encNew;
    if (!encode(tokOld, encOld) || !encode(tokNew, encNew)) {
        html = "Too many distinct tokens for the selected diff level; try Line or Paragraph.";
        return false;
    }

    diff_match_patch dmp;
    dmp.Diff_Timeout = 0;           // exact result; token strings are short
    QList<Diff> diffs = dmp.diff_main(encOld, encNew, false);
    dmp.diff_cleanupSemantic(diffs);

    QString out = "<pre style=\"white-space: pre-wrap; font-family: monospace;\">";
    for (const Diff& d : diffs) {
        QString text;
        for (const QChar ch : d.text) {
            const int k = ch.unicode();
            if (k < 1 || k > table.size()) {
                html = "Internal error: unknown token code in diff result.";
                return false;
            }
            text += table[k - 1];
        }
        const QString esc = text.toHtmlEscaped();
        switch (d.operation) {
        case INSERT:
            out += "<span style=\"background-color:#b7f0b1; text-decoration: underline;\">" + esc + "</span>";
            break;
        case DELETE:
            out += "<span style=\"background-color:#ffb3b3; text-decoration: line-through;\">" + esc + "</span>";
            break;
        default:
            out += esc;
            break;
        }
    }
    out += "</pre>";
    html = out.toStdString();
    return true;
}
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
                pushAct->setEnabled(false);
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
 * @details **File > Open...** (standard Open shortcut) calls SYW::Open(); **File > Configure...** calls configure();
 * **File > Push** (disabled until a document is open) calls push(); **File > Quit** closes the window.
 * **View > Document** (disabled until a single Markdown file is selected, see updateViewActions()) calls
 * viewDocument(); **View > Diff** (disabled until two are selected) calls viewDiff(). Double-clicking a Markdown file in the tree does the same.
 * Disabled menu entries are explicitly painted grey (palette, Disabled group) so they read as unavailable
 * on every platform style.
 */
void MainWindow::buildMenu()
{
    QMenu* file = menuBar()->addMenu("&File");

    QAction* open = file->addAction("&Open...");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this]() { syw.Open(); });

    QAction* configureAct = file->addAction("&Configure...");
    connect(configureAct, &QAction::triggered, this, [this]() { configure(); });

    pushAct = file->addAction("&Push");
    pushAct->setEnabled(false);          // enabled once a document is open
    connect(pushAct, &QAction::triggered, this, [this]() { push(); });

    file->addSeparator();

    QAction* quit = file->addAction("&Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu("&View");

    viewDocumentAct = view->addAction("&Document");
    viewDocumentAct->setEnabled(false);
    connect(viewDocumentAct, &QAction::triggered, this, [this]() { viewDocument(); });

    viewDiffAct = view->addAction("D&iff");
    viewDiffAct->setEnabled(false);
    connect(viewDiffAct, &QAction::triggered, this, [this]() { viewDiff(); });

    // Disabled entries must look greyed out whatever the platform style does by default.
    auto greyDisabled = [](QMenu* menu) {
        QPalette p = menu->palette();
        const QColor grey(128, 128, 128);
        p.setColor(QPalette::Disabled, QPalette::WindowText, grey);
        p.setColor(QPalette::Disabled, QPalette::Text, grey);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, grey);
        menu->setPalette(p);
    };
    greyDisabled(file);
    greyDisabled(view);

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
        if (!p.waitForFinished(120000)) {      // a push of a large DOCX can take a while
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

    syw.DiffHtml = DiffToHtml;

    syw.HomeDir = []() { return QDir::homePath().toStdString(); };

    syw.OnStatus = [this](const std::string& text) {
        statusBar()->showMessage(QString::fromUtf8(text.c_str()), 10000);
    };

    syw.OnOpened = [this]() {
        tree->setEnabled(true);
        pushAct->setEnabled(true);
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
 * @details Looks at the tree selection. **Document** is enabled when exactly one item is selected and
 * IsViewableMarkdown() accepts it (a file item, path ends in `.md` case-insensitively, status not `D` because a
 * deleted file does not exist in that commit). **Diff** is enabled when exactly two items are selected and both
 * are viewable Markdown. The tree is rebuilt after every save, which clears the selection, so this also runs
 * at the end of showTree().
 */
void MainWindow::updateViewActions()
{
    const QList<QTreeWidgetItem*> sel = tree->selectedItems();

    viewDocumentAct->setEnabled(sel.size() == 1 && IsViewableMarkdown(sel[0]));
    viewDiffAct->setEnabled(sel.size() == 2 && IsViewableMarkdown(sel[0]) && IsViewableMarkdown(sel[1]));
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
 * @details The two selected items come in selection order, which says nothing about age, so
 * SYW::OrderCommits() decides which is old and which is new. SYW::DiffFiles() fetches both snapshots and calls
 * the DiffHtml hook (DiffToHtml()). The popup is a QTextBrowser like the one in viewDocument(), with a header
 * naming both commits (hash, subject, date, author, taken from the tree rows) above the diff. The granularity is
 * SYW::DiffLevel.
 */
void MainWindow::viewDiff()
{
    const QList<QTreeWidgetItem*> sel = tree->selectedItems();
    if (sel.size() != 2 || !viewDiffAct->isEnabled()) return;

    QTreeWidgetItem* older = sel[0];
    QTreeWidgetItem* newer = sel[1];

    bool bIsOlder = false;
    if (!syw.OrderCommits(older->data(0, RoleHash).toString().toStdString(),
                          newer->data(0, RoleHash).toString().toStdString(), bIsOlder))
        return;
    if (bIsOlder) std::swap(older, newer);

    const QString oldHash = older->data(0, RoleHash).toString();
    const QString newHash = newer->data(0, RoleHash).toString();
    const QString oldPath = older->data(0, RolePath).toString();
    const QString newPath = newer->data(0, RolePath).toString();

    std::string html;
    if (!syw.DiffFiles(oldHash.toStdString(), oldPath.toUtf8().constData(),
                       newHash.toStdString(), newPath.toUtf8().constData(), html))
        return;

    // Header: one line per commit, from the tree rows (commit item is the file item's parent).
    auto describe = [](QTreeWidgetItem* file, const QString& path) {
        QTreeWidgetItem* commit = file->parent();
        return QString("%1 &nbsp; <small>%2, %3</small><br><small>%4</small>")
            .arg(commit ? commit->text(0).toHtmlEscaped() : QString())
            .arg(commit ? commit->text(1).toHtmlEscaped() : QString())
            .arg(commit ? commit->text(2).toHtmlEscaped() : QString())
            .arg(path.toHtmlEscaped());
    };
    const QString header =
        "<p><b>Old:</b> " + describe(older, oldPath) + "<br><b>New:</b> " + describe(newer, newPath) +
        "<br><small>Legend: <span style=\"background-color:#ffb3b3; text-decoration: line-through;\">removed</span> "
        "<span style=\"background-color:#b7f0b1; text-decoration: underline;\">added</span></small></p><hr>";

    QTextBrowser* viewer = new QTextBrowser(this);
    viewer->setWindowFlag(Qt::Window);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setOpenLinks(false);
    viewer->setWindowTitle("Diff " + oldHash + " .. " + newHash);
    viewer->setHtml(header + QString::fromUtf8(html.c_str()));
    viewer->resize(800, 800);
    viewer->show();
}

/**
 * @details The dialog is modal. Edits are plain QLineEdits; the diff level is a QComboBox whose index equals
 * SYW::diffLevel. The "Save to" combo box has the default file always and the document file only while
 * SYW::DocConfigPath is set (a document is open); it starts on the document file in that case. Cancel changes
 * nothing.
 */
void MainWindow::configure()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Configure SYW");
    dlg.setMinimumWidth(640);
    QFormLayout* form = new QFormLayout(&dlg);

    auto field = [&](const QString& label, const std::string& value) {
        QLineEdit* e = new QLineEdit(QString::fromUtf8(value.c_str()), &dlg);
        form->addRow(label, e);
        return e;
    };
    QLineEdit* server = field("Git server", syw.git_server);
    QLineEdit* user = field("Git user", syw.git_user);
    QLineEdit* repo = field("Git repository (this document)", syw.git_repo);
    repo->setPlaceholderText("default: the repository's directory name");
    // The directory name often differs from the repository name, so this stays editable for the document
    // even after the remote exists (then it is only used if a remote has to be created again).
    const bool hasRemote = !syw.RepoRoot.empty() && syw.HasRemote();
    QLineEdit* editor = field("Editor command (%s = the document)", syw.editor);
    QLineEdit* toMd = field("To Markdown (%s = path without .docx/.md)", syw.to_Markdown);

    QComboBox* level = new QComboBox(&dlg);
    level->addItems({"Word", "Line", "Paragraph"});     // index == SYW::diffLevel
    level->setCurrentIndex(int(syw.DiffLevel));
    form->addRow("Diff level", level);

    QCheckBox* autoPush = new QCheckBox("Push after every commit (when a repository is set or a remote exists)", &dlg);
    autoPush->setChecked(syw.auto_push);
    form->addRow("Auto push", autoPush);

    QComboBox* scope = new QComboBox(&dlg);
    scope->addItem("Default for all documents (" + QString::fromUtf8(syw.DefaultConfigPath.c_str()) + ")",
                   int(SYW::ConfigDefault));
    if (!syw.DocConfigPath.empty()) {
        scope->addItem("This document only (" + QString::fromUtf8(syw.DocConfigPath.c_str()) + ")",
                       int(SYW::ConfigDocument));
        scope->setCurrentIndex(1);
    }
    form->addRow("Save to", scope);

    // The repository belongs to one document: editable only when saving to the document's file.
    auto syncRepo = [=]() {
        const bool doc = scope->currentData().toInt() == int(SYW::ConfigDocument);
        repo->setEnabled(doc);
        if (!doc) repo->setToolTip("The repository is per document; choose \"This document only\" to set it.");
        else if (hasRemote) repo->setToolTip("This document already has a remote, which Push uses as it is. The name is used when a remote has to be created.");
        else repo->setToolTip(QString());
    };
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, syncRepo);
    syncRepo();

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    syw.git_server = server->text().toUtf8().constData();
    syw.git_user = user->text().toUtf8().constData();
    syw.editor = editor->text().toUtf8().constData();
    syw.to_Markdown = toMd->text().toUtf8().constData();
    syw.DiffLevel = SYW::diffLevel(level->currentIndex());
    syw.auto_push = autoPush->isChecked();
    const SYW::configScope chosen = SYW::configScope(scope->currentData().toInt());
    if (chosen == SYW::ConfigDocument) syw.git_repo = repo->text().toUtf8().constData();
    if (syw.SaveConfig(chosen))
        statusBar()->showMessage("Configuration saved", 5000);
}

/// @details SYW::Push() publishes its own errors; on success its summary line is shown for 10 seconds.
void MainWindow::push()
{
    std::string summary;
    statusBar()->showMessage("Pushing...");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = syw.Push(summary);
    QApplication::restoreOverrideCursor();
    if (ok) statusBar()->showMessage(QString::fromUtf8(summary.c_str()), 10000);
    else statusBar()->clearMessage();
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