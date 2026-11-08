// The `exec` executable: everything is in runExecCli (exec_cli.h).

#include <iostream>
#include <string>
#include <vector>

#include "exec_cli.h"

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return db::apps::runExecCli(args, std::cout, std::cerr);
}
