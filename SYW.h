#ifndef SYW_H
#define SYW_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Error.h"
#include "XmlCls.h"

class SYW
{
private:
    /* data */
public:
    Error err;      // Cleared at the start of each operation; published (MSG_ERR) at its end.

    std::string git_server = "https://github.com";
    std::string git_user = "user";

    // printf-style template; %s is the absolute file path
    std::string editor = "flatpak run --command=libreoffice org.libreoffice.LibreOffice --writer \"%s\"";
    // printf-style template; both %s are the absolute file path without its extension
    std::string to_Markdown = "pandoc \"%s.docx\" -t markdown -o \"%s.md\"";

    long EditorPID = 0;
    char CurrentFile[4096] = "";    // absolute path of the open file
    std::string LogText;            // last `git log` output, for the GUI to display

    SYW(/* args */);
    ~SYW();

    /**
     * @brief OS and GUI-independent file name getter.
     * 
     * Defines a function that returns the file name. This function must be overridden in caller code
     * to provide a custom implementation for retrieving the file name in different environments or contexts.
     *
     * @returns The file name.
     */
    std::function<std::string()> GetFileName = []() { return ""; };

    /**
     * @brief OS shell command executor.
     *
     * @param command The shell command to execute (run in the current directory).
     * @param output  Receives stdout+stderr, or a description if the command could not be run.
     * @returns The exit code, or -1 if the command could not be started, crashed or timed out.
     */
    std::function<int(const std::string&, std::string&)> Shell =
        [](const std::string&, std::string& output) { output = "Shell hook not set"; return -1; };

    /**
     * @brief Launches the editor process (non-blocking) and reports its PID.
     *
     * Must be overridden by the GUI layer. Returns 0 on failure (the hook should set `err`).
     */
    std::function<long(const std::string&)> LaunchEditor = [](const std::string&) { return 0L; };

    /// Absolute form of a path.
    std::function<std::string(const std::string&)> AbsPath = [](const std::string& file) { return file; };

    /// Make the directory containing `file` the current directory. Returns false on failure.
    std::function<bool(const std::string&)> CdToFile = [](const std::string&) { return false; };

    /// `file` without its extension, directory kept (/a/b/report.docx -> /a/b/report).
    std::function<std::string(const std::string&)> BasePath = [](const std::string& file) { return file; };

    /// Asks for the commit message of a revision. Return an empty string to skip the commit.
    std::function<std::string()> GetCommitMessage = []() { return std::string("SYW revision"); };

    /// Called after an editor has been launched for a file.
    std::function<void()> OnOpened = []() {};
    /// Called after Save() has run (success or not).
    std::function<void()> OnSaved = []() {};
    /// Called at the end of Exit() to terminate the program.
    std::function<void()> Quit = []() {};

    /// printf-style: record an error in `err`. `fmt` may be nullptr for no data.
    void SetErr(lvl level, const char* msg, const char* fmt, ...);

    /// Launch the editor on `file`. Sets `err` on failure. Ignored if an editor is already running.
    void Open(const std::string& file);
    void Open() {
        std::string fileName = GetFileName();
        if (!fileName.empty()) Open(fileName);
    }

    /**
     * @brief The file has been modified, do the `thing`.
     *
     * Converts the document to Markdown via `to_Markdown`, stages the DOCX and MD files, commits
     * them (if they changed and GetCommitMessage() returns a message) and refreshes the git log.
     * Nothing is pushed.
     */
    void Save();

    /**
     * @brief The editing process has completed, do the `other thing` and quit the program.
     *
     * Performs a final Save(), clears the editor state and calls Quit().
     */
    void Exit();

    /// Fill LogText from `git log` of the current directory. Sets `err` and returns false on failure.
    bool RefreshLog();
};

#endif // SYW_H