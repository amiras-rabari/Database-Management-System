// The `sql` executable: everything is in runSqlCli (sql_cli.h).

#include <iostream>
#include <string>
#include <vector>

#include "sql_cli.h"

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    return db::apps::runSqlCli(args, std::cout, std::cerr);
}
