#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

#include "test_platform.h"

// The test binary doubles as the portable child process fixture
// (test_platform.h): a fixture invocation must exit before doctest parses
// argv, since the fixture arguments are not doctest options.
int main(int argc, char** argv)
{
    imza::test::set_executable_path(argv[0]);
    // CI captures stdout through a fully buffered pipe: without this, a
    // hang in one test case leaves the log silent from the build's end on,
    // hiding everything that ran before it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (imza::test::run_fixture(argc, argv)) {
        return 0;
    }
    doctest::Context context(argc, argv);
    const int result = context.run();
    if (context.shouldExit()) {
        return result;
    }
    return result;
}
