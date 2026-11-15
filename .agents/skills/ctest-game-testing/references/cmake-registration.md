# CMake Registration & Execution

## Introducing the Suite

This is an implementation pattern, not existing project configuration. At the root, use `include(CTest)`, retain normal production target setup, and guard `add_subdirectory(tests)` with `if(BUILD_TESTING)`. `include(CTest)` creates the option and enables testing unless it is OFF. Enabling only in a nested directory prevents normal root-level discovery.

Inspect dependency options before exposing `BUILD_TESTING`: keep project tests distinct from vendor suites. Installed libraries have switches such as `FMT_TEST`, `JSON_BuildTests`, `ENTT_BUILD_TESTING`, `SDL_TESTS`, `BGFX_BUILD_TESTS`, and `MINIAUDIO_BUILD_TESTS`; verify each declaration/default locally and set only relevant options before its `add_subdirectory`. Avoid forcing global `BUILD_TESTING` OFF to disable vendor tests.

For an implemented CPU-only production target and test source, an example registration in `tests/CMakeLists.txt` is:

```cmake
add_executable(chunk_boundary_tests unit/game/world/chunk_boundary_tests.cpp)
target_link_libraries(chunk_boundary_tests PRIVATE game_world_logic)
add_test(NAME game.world.chunk_boundaries COMMAND chunk_boundary_tests)
set_tests_properties(game.world.chunk_boundaries PROPERTIES
    LABELS "unit;gameplay;world"
    TIMEOUT 10)
```

`game_world_logic` and the example source are proposed names, not existing artifacts. Register executable targets by name, avoiding hardcoded output paths. Use several cases through a domain driver or a framework's CMake integration as the suite grows; preserve useful case-level failure reporting. Do not edit generated `CTestTestfile.cmake` files.

## Isolation & Failure Semantics

Return zero for success and nonzero for failed checks. Do not use output-matching properties to hide incorrect exit codes; expected error handling should be checked by the test. `WILL_FAIL` does not turn crashes into expected success.

Give writable tests unique directories beneath the build tree. Treat `tests/fixtures/` as read-only; never overwrite repository `settings.json` or generate test data into runtime `assets/`. Set `WORKING_DIRECTORY` or pass absolute fixture paths explicitly. Isolated paths permit parallel runs.

Use CTest setup/cleanup fixtures for genuinely shared setup; a `DEPENDS` ordering alone does not enforce successful setup. Use `RESOURCE_LOCK` only for unavoidable shared resources. Fixtures do not serialize users. Bound per-test time and review `PROCESSORS`/parallelism when each test spawns its own pool.

## Commands After Wiring Exists

From the repository root:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build -N
ctest --test-dir build --output-on-failure --no-tests=error
ctest --test-dir build -L gameplay --output-on-failure --no-tests=error
ctest --test-dir build -R '^game\.world\.' --output-on-failure --no-tests=error
ctest --test-dir build -j 4 --output-on-failure --no-tests=error
```

For multi-configuration generators, build with `--config Debug` and run CTest with `-C Debug`. Normal CTest execution does not build executables; its separate build-and-test/dashboard modes serve different workflows. Use `-V` and `build/Testing/Temporary/LastTest.log` for diagnostics. Reconfigure with `BUILD_TESTING=OFF` to verify optional tests do not break production builds.

## Official References

- [CTest module, CMake 3.20](https://cmake.org/cmake/help/v3.20/module/CTest.html): root enablement and `BUILD_TESTING`.
- [add_test, CMake 3.20](https://cmake.org/cmake/help/v3.20/command/add_test.html): target resolution, working directories, and exit status.
- [CTest CLI, CMake 3.20](https://cmake.org/cmake/help/v3.20/manual/ctest.1.html): discovery, selection, configuration, and execution.
- [Required fixtures, CMake 3.20](https://cmake.org/cmake/help/v3.20/prop_test/FIXTURES_REQUIRED.html): setup failure and cleanup semantics.
