/**
 * @file MainWindow.h
 * @brief Qt front end for SYW: menu, git log pane, editor process and file watching.
 */
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>

#include "SYW.h"

class QPlainTextEdit;

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
 * | OnOpened/OnSaved  | enable/refresh the log pane, (re-)arm @ref watcher          |
 * | Quit              | QApplication::quit()                                        |
 *
 * @par Lifecycle
 *  - **File > Open** calls SYW::Open(), which launches the editor.
 *  - A save in the editor changes the file; @ref watcher notices, @ref debounce waits 500 ms, then
 *    SYW::Save() converts, commits and refreshes the log.
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
    /// Create the **File** menu (Open, Quit).
    void buildMenu();
    /// Assign every SYW hook (see the table in the class description).
    void wireSyw();
    /// Watch SYW::CurrentFile for changes. Called after every save because editors replace the file.
    void watchFile();
    /// Show SYW::LogText in the text pane.
    void showLog();

    SYW& syw;                       ///< The session being driven.
    QPlainTextEdit* text;           ///< Read-only git log pane; enabled only while an editor is running.
    QProcess editorProc;            ///< The external editor process.
    QFileSystemWatcher watcher;     ///< Detects saves of the open file.
    QTimer debounce;                ///< Single-shot 500 ms timer coalescing bursts of file changes.
};

#endif // MAINWINDOW_H