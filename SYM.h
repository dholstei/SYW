#ifndef SYM_H
#define SYM_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Error.h"
#include "XmlCls.h"

class SYM
{
private:
    /* data */
public:
    Error err;

    std::string git_server = "https://github.com";
    std::string git_user = "user";

    std::string editor = "flatpak run --command=libreoffice org.libreoffice.LibreOffice --writer %s";
    std::string to_Markdown = "pandoc %s.docx -t markdown -o %s.md";

    long EditorPID = 0;

    SYM(/* args */);
    ~SYM();

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
     * @param command The shell command to execute.
     */
    std::function<void(std::string)> Shell = [](std::string command) { return; };

    void Open(const std::string& file){;}
    void Open() {
        std::string fileName = GetFileName();
        if (!fileName.empty()) Open(fileName);
    }

    /**
     * @brief The file has been modified, do the `thing`.
     */
    void Save();

    /**
     * @brief The editing process has completed, do the `other thing` and quit the program.
     */
    void Exit();
    
};

#endif // SYM_H