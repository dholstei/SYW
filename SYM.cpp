#include "SYM.h"

int main(int argc, char** argv) {
    // Build Qt infrastructure and initialize the application

    SYM sym;

    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            sym.Open(argv[i + 1]); ++i; // Skip the next argument since it's the file name
        }
    }

    while (sym.EditorPID != 0) {
        // Main loop of the application
    }

    return 0;
}

SYM::SYM(/* args */)
{
}

SYM::~SYM()
{
}