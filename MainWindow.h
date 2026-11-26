/**
 * @file MainWindow.h
 * @brief Qt front end for SYW: menus, branch/commit/file tree, document viewer, editor process and file watching.
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>

#include "SYW.h"

class QAction;
class QTreeWidget;

/**
 * @brief Main window of the SYW application.
 *
 * @details
 * MainWindow owns everything Qt-specific and connects it to the GUI-independent @ref SYW core by
 * assigning the core's hooks in wireSyw():
 *
 * | SYW hook          | Implemented with                                            |
 * |-------------------|-------------------------------------------------------------|
 * | GetFileName       | QFileDialog::getOpenFileName()                              |
 * | AbsPath           | QFileInfo::absoluteFilePath()                               |
 * | CdToFile          | QDir::setCurrent()                                          |
 * | BasePath          | QFileInfo::completeBaseName() + absoluteDir()               |
 * | Shell             | QProcess (stdout+stderr merged, 5 s start / 30 s run limit) |
 * | LaunchEditor      | the @ref editorProc QProcess                                |
 * | GetCommitMessage  | QInputDialog::getMultiLineText()                            |
 * | DiffHtml          | diff_match_patch on word/line/paragraph tokens, see DiffToHtml() |
 * | HomeDir           | QDir::homePath()                                            |
 * | OnOpened/OnSaved  | enable/refresh the @ref tree, (re-)arm @ref watcher          |
 * | Quit              | QApplication::quit()                                        |
 *
 * @par View menu
 * **View > Document** is enabled only while exactly one item is selected in the @ref tree and that item is
 * a Markdown file that exists in its commit. It opens a separate, non-modal window showing the rendered
 * Markdown of that snapshot (SYW::FileAtCommit()); any number of these windows can be open at once.
 *
 * **View > Diff** is enabled only while exactly two Markdown file items are selected (two snapshots, usually of
 * the same file in different commits). The older one is found with SYW::OrderCommits(), the comparison is made
 * by SYW::DiffFiles() through the @ref SYW::DiffHtml hook (implemented here with diff_match_patch) at the
 * granularity SYW::DiffLevel, and shown in a popup window, again any number at once.
 *
 * @par File menu
 * **File > Configure...** (configure()) edits the five settings of SYW (git server and user, editor and
 * Markdown commands, diff level) and writes them to `~/.syw.xml` or, for the open document, to
 * `<git dir>/.syw.xml`. **File > Push** (push()) runs SYW::Push(); it is enabled while a document is open.
 *
 * @par Lifecycle
 *  - **File > Open** calls SYW::Open(), which launches the editor.
 *  - A save in the editor changes the file; @ref watcher notices, @ref debounce waits 500 ms, then
 *    SYW::Save() converts, commits and refreshes the tree.
 *  - When the editor exits (or the window is closed) SYW::Exit() performs a final save and quits.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    /**
     * @brief Build the window and connect it to a SYW session.
     * @param syw    The session to drive. Must outlive the window.
     * @param parent Parent widget (normally none).
     */
    explicit MainWindow(SYW& syw, QWidget* parent = nullptr);

protected:
    /**
     * @brief Shut the session down when the window is closed.
     *
     * Terminates a still-running editor (kill after 3 s), then calls SYW::Exit() for the final save
     * and program exit.
     */
    void closeEvent(QCloseEvent* event) override;

private:
    /// Create the **File** menu (Open, Quit) and the **View** menu (Document, Diff).
    void buildMenu();
    /// Assign every SYW hook (see the table in the class description).
    void wireSyw();
    /// Watch SYW::CurrentFile for changes. Called after every save because editors replace the file.
    void watchFile();
    /**
     * @brief Rebuild the tree from SYW::Branches.
     *
     * Three levels: branch > commit > file. Every item carries a unique key in `Qt::UserRole`
     * (`b:<branch>`, `.../c:<hash>`, `.../f:<path>`) and its kind (0 branch, 1 commit, 2 file) in
     * `Qt::UserRole + 1`, so later actions can tell what was selected. Expanded items stay expanded across
     * refreshes; on the first fill only the current branch is expanded.
     */
    void showTree();
    /**
     * @brief Enable or disable the View actions to match the tree selection.
     *
     * Called whenever the selection changes and after every rebuild of the tree.
     */
    void updateViewActions();
    /**
     * @brief Open a popup window with the rendered Markdown of the selected file at its commit.
     *
     * Does nothing unless exactly one Markdown file is selected. Errors are published through `syw.err`.
     */
    void viewDocument();
    /**
     * @brief Open a popup window comparing the two selected Markdown snapshots.
     *
     * Does nothing unless exactly two Markdown files are selected. Orders them old/new, calls
     * SYW::DiffFiles() and shows the HTML in a read-only QTextBrowser headed by both commits. Additions are
     * green, deletions red and struck through. Errors are published through `syw.err`.
     */
    void viewDiff();
    /**
     * @brief Show the settings dialog and save the result.
     *
     * One line edit per setting plus a combo box for the diff level, and a combo box choosing where to
     * save: the default file (`~/.syw.xml`) or, when a document is open, that document's file (preselected).
     * On OK the values are assigned to the SYW members, then SYW::SaveConfig() writes them. The editor
     * command is used the next time a file is opened; the others take effect immediately.
     */
    void configure();
    /// Run SYW::Push() and show its one-line result in the status bar (errors are published by SYW).
    void push();

    SYW& syw;                       ///< The session being driven.
    QAction* viewDocumentAct;       ///< **View > Document**; enabled by updateViewActions().
    QAction* pushAct;               ///< **File > Push**; enabled while a document is open.
    QAction* viewDiffAct;           ///< **View > Diff**; enabled by updateViewActions().
    QTreeWidget* tree;              ///< Branch > commit > file tree; enabled only while an editor is running.
    QProcess editorProc;            ///< The external editor process.
    QFileSystemWatcher watcher;     ///< Detects saves of the open file.
    QTimer debounce;                ///< Single-shot 500 ms timer coalescing bursts of file changes.
};

#endif // MAINWINDOW_H