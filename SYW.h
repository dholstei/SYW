/**
 * @file SYW.h
 * @brief SYW (Show Your Work) core class: document editing session, Markdown export and git history.
 *
 * @details
 * SYW records the evolution of a document. One @ref SYW object manages a single editing session:
 *
 *  1. @ref SYW::Open launches an external editor on a DOCX file (and checks that the file lives in a git repo),
 *  2. every time the editor saves, @ref SYW::Save converts the DOCX to Markdown with pandoc and commits
 *     both files to git,
 *  3. when the editor closes, @ref SYW::Exit does a final save and quits the program.
 *
 * The class is deliberately GUI- and OS-independent. Everything that touches the outside world (file
 * dialogs, processes, directories, user prompts) is reached through `std::function` **hooks** that the
 * front end (see @ref MainWindow) overrides. The defaults are inert and fail safe: they report failure
 * rather than pretend success.
 *
 * @par Error contract
 * Every operation clears @ref SYW::err on entry, records failures with @ref SYW::SetErr, and publishes the
 * result once, on exit, through `MSG_ERR(err)` (see Error.h). Where the message goes is decided by
 * `Error::Publisher` (printf for a CLI, a popup for the Qt application).
 *
 * @par Assumptions
 *  - The DOCX and its Markdown file live in the root of a git repository.
 *  - The process current directory is changed to the file's directory on Open().
 */
#ifndef SYW_H
#define SYW_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Error.h"
#include "XmlCls.h"

/**
 * @brief One file touched by a commit.
 */
struct GitFile
{
    std::string status;     ///< git name-status code: `A` added, `M` modified, `D` deleted, `R100` renamed, ...
    std::string path;       ///< Path relative to the repository root (for a rename: `old<TAB>new`).
};

/**
 * @brief One commit and the files it changed.
 */
struct GitCommit
{
    std::string hash;       ///< Abbreviated commit hash.
    std::string date;       ///< Author date, `YYYY-MM-DD`.
    std::string author;     ///< Author name.
    std::string subject;    ///< First line of the commit message.
    std::vector<GitFile> files;     ///< Files changed (nominally the DOCX and the MD file).
};

/**
 * @brief One local branch and its most recent commits (newest first).
 */
struct GitBranch
{
    std::string name;       ///< Branch name, e.g. `master`.
    bool current = false;   ///< `true` for the checked-out branch.
    std::vector<GitCommit> commits;     ///< At most SYW::LogLimit commits, newest first.
};

/**
 * @brief One SYW editing session.
 *
 * @see MainWindow for the Qt front end that wires up the hooks.
 */
class SYW
{
private:
    /* data */
public:
    /// @name State
    /// @{

    /**
     * @brief Result of the most recent operation.
     *
     * Cleared at the start of Open() and Save(), published with `MSG_ERR(err)` at their end.
     * Test it with `if (err)`; see Error.h.
     */
    Error err;

    /// Git server (reserved for the future push support; currently unused).
    std::string git_server = "https://github.com";
    /// Git user name (reserved for the future push support; currently unused).
    std::string git_user = "user";

    /**
     * @brief printf-style command template used to launch the editor.
     *
     * `%s` is replaced with the absolute path of the file. The path is already quoted in the template
     * because the command line is split with QProcess::splitCommand() by the front end.
     */
    std::string editor = "flatpak run --command=libreoffice org.libreoffice.LibreOffice --writer \"%s\"";

    /**
     * @brief printf-style command template used to convert the DOCX to Markdown.
     *
     * Both `%s` are replaced with the absolute path of the file **without its extension**
     * (see @ref BasePath). The commit step assumes the output is `<base>.md`.
     */
    std::string to_Markdown = "pandoc \"%s.docx\" -t markdown -o \"%s.md\"";

    /// PID of the running editor process, or 0 when no editor is running.
    long EditorPID = 0;
    /// Absolute path of the open file; empty (`""`) when no file is open.
    char CurrentFile[4096] = "";

    /// @name Git state (filled by RefreshLog())
    /// @{
    std::string RepoRoot;               ///< Top-level directory of the git repository.
    std::string CurrentBranch;          ///< Checked-out branch; empty for a detached HEAD.
    std::vector<GitBranch> Branches;    ///< Local branches (current branch first) with commits and files.
    int LogLimit = 50;                  ///< Maximum number of commits listed per branch.
    /// @}
    /// @}

    /// Constructs an idle session (no file open, all hooks at their inert defaults).
    SYW(/* args */);
    /// Destroys the session. Does not stop the editor; use Exit() first.
    ~SYW();

    /// @name Hooks
    /// Override these from the front end. Defaults are inert and fail safe.
    /// @{

    /**
     * @brief OS and GUI-independent file name getter.
     * 
     * Defines a function that returns the file name. This function must be overridden in caller code
     * to provide a custom implementation for retrieving the file name in different environments or contexts.
     *
     * @returns The file name, or an empty string if the user cancelled.
     */
    std::function<std::string()> GetFileName = []() { return ""; };

    /**
     * @brief OS shell command executor.
     *
     * @param command The shell command to execute (run in the current directory).
     * @param output  Receives stdout+stderr, or a description if the command could not be run.
     * @returns The exit code, or -1 if the command could not be started, crashed or timed out.
     *
     * @note Callers rely on the exit code: git commands such as `git diff --cached --quiet` use 0/1
     *       to answer yes/no, so a front end must pass the real exit code through.
     */
    std::function<int(const std::string&, std::string&)> Shell =
        [](const std::string&, std::string& output) { output = "Shell hook not set"; return -1; };

    /**
     * @brief Launches the editor process (non-blocking) and reports its PID.
     *
     * Must be overridden by the GUI layer.
     *
     * @param command Fully expanded editor command line (see @ref editor).
     * @returns The PID, or 0 on failure. On failure the hook should describe the problem with SetErr().
     */
    std::function<long(const std::string&)> LaunchEditor = [](const std::string&) { return 0L; };

    /**
     * @brief Absolute form of a path.
     * @param file A path, possibly relative to the current directory.
     * @returns The absolute path.
     */
    std::function<std::string(const std::string&)> AbsPath = [](const std::string& file) { return file; };

    /**
     * @brief Make the directory containing `file` the current directory.
     * @param file Absolute path of a file.
     * @returns `false` on failure.
     */
    std::function<bool(const std::string&)> CdToFile = [](const std::string&) { return false; };

    /**
     * @brief `file` without its extension, directory kept.
     * @param file Absolute path, e.g. `/a/b/report.docx`.
     * @returns The path without extension, e.g. `/a/b/report`.
     */
    std::function<std::string(const std::string&)> BasePath = [](const std::string& file) { return file; };

    /**
     * @brief Asks for the commit message of a revision.
     * @returns The message, or an empty string to skip the commit (the files stay staged).
     */
    std::function<std::string()> GetCommitMessage = []() { return std::string("SYW revision"); };

    /// Called after an editor has been launched for a file.
    std::function<void()> OnOpened = []() {};
    /// Called after Save() has run (success or not), so the GUI can refresh and re-arm file watching.
    std::function<void()> OnSaved = []() {};
    /// Called at the end of Exit() to terminate the program.
    std::function<void()> Quit = []() {};
    /// @}

    /**
     * @brief Record an error in @ref err (printf style).
     *
     * @param level Severity (INFO, WARN or ERR).
     * @param msg   Short, human-readable summary.
     * @param fmt   printf format for the `data` field (details such as paths, exit codes and command
     *              output), or `nullptr` for no data. Output is truncated at 2047 characters.
     * @param ...   Arguments for `fmt`.
     *
     * @note This only records the error. Publishing happens at the end of the public operation, or
     *       explicitly with `MSG_ERR(err)`.
     */
    void SetErr(lvl level, const char* msg, const char* fmt, ...);

    /**
     * @brief Launch the editor on `file`.
     *
     * Sequence (each step records an error in @ref err and stops on failure):
     *  1. refuse if an editor is already running (WARN),
     *  2. resolve the absolute path and check it fits in @ref CurrentFile,
     *  3. check that the file can be opened for reading (`fopen`),
     *  4. change the current directory to the file's directory,
     *  5. check that the directory is a git repository (see RefreshLog()); a repository with no
     *     commits yet is fine,
     *  6. launch the editor and record its PID, then call @ref OnOpened.
     *
     * @param file Path of the document, absolute or relative.
     */
    void Open(const std::string& file);

    /// Ask the user for a file name (see @ref GetFileName) and Open() it; does nothing if cancelled.
    void Open() {
        std::string fileName = GetFileName();
        if (!fileName.empty()) Open(fileName);
    }

    /**
     * @brief The file has been modified, do the `thing`.
     *
     * Sequence (each step records an error in @ref err and stops on failure):
     *  1. convert the DOCX to Markdown with @ref to_Markdown,
     *  2. `git add` the DOCX and the Markdown file,
     *  3. `git diff --cached --quiet` to see whether either changed; if not, stop quietly,
     *  4. ask for a commit message (@ref GetCommitMessage); an empty answer skips the commit,
     *  5. `git commit -F <file> -- <docx> <md>` (only those two paths are committed),
     *  6. refresh the branch / commit / file lists (see RefreshLog()).
     *
     * @ref OnSaved is always called, then the error (if any) is published. Nothing is pushed.
     */
    void Save();

    /**
     * @brief The editing process has completed, do the `other thing` and quit the program.
     *
     * Performs a final Save(), clears the editor state and calls @ref Quit.
     */
    void Exit();

    /**
     * @brief Fill @ref Branches, @ref CurrentBranch and @ref RepoRoot from git, using the current directory.
     *
     * A repository with no commits yet is **not** an error: @ref Branches is simply empty and `true` is
     * returned. The previous contents are replaced only if every git command succeeded.
     *
     * @returns `true` on success; `false` with @ref err set on failure (git missing, not a repository,
     *          a git command failed). Unusable branch names are skipped with a WARN in @ref err.
     */
    bool RefreshLog();
};

#endif // SYW_H