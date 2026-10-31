// The `storage` executable: everything is in runStorageCli (storage_cli.h).

#include <iostream>
#include <string>
#include <vector>

#include "storage_cli.h"

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return db::apps::runStorageCli(args, std::cout, std::cerr);
}
