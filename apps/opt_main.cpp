// The `opt` executable: everything is in runOptCli (opt_cli.h).

#include <iostream>
#include <string>
#include <vector>

#include "opt_cli.h"

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return db::apps::runOptCli(args, std::cout, std::cerr);
}
