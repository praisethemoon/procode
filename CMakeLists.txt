# Both C tools in one build.
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build              # lap.* and kb.* suites
#   ctest --test-dir build -R '^kb\.'   # one tool's suites
#   cmake --install build               # installs bin/lap and bin/kb
#   cmake --build build --target uninstall
#
# Each tool's own CMakeLists.txt still builds standalone, and the Makefiles
# in cli/ stay the quick path on POSIX (npm run build uses them).

cmake_minimum_required(VERSION 3.16)
project(lap_workspace LANGUAGES C)

enable_testing()

add_subdirectory(cli/lap-cli)
add_subdirectory(cli/kb-cli)
