// The `shell` executable: everything is in runShell (db/shell/shell.h).

#include <iostream>
#include <string>
#include <vector>

#include "db/shell/shell.h"

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return db::shell::runShell(args, std::cin, std::cout, std::cerr);
}
