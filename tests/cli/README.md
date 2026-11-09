# Command-line tests

`commandline-test` builds the real `sources/commandline` parser against Qt Core
only. It registers options the same way `sources/hyperhdr/main.cpp` does and
fails on any Qt warning, so a flag queried for a value (`QCommandLineParser:
option not expecting values`) is caught.

```sh
cmake -S tests/cli -B build-cli-tests
cmake --build build-cli-tests
ctest --test-dir build-cli-tests --output-on-failure
```

`hyperhdr-cli-smoke.sh` checks an installed or built binary: exit codes and
stdout/stderr for `--help`, `-h`, `--version`, `--userdata` without a value, an
unknown option, `--debug=1` and `--debug --verbose`. It also checks that none of
these create the user data directory, which shows the daemon did not start.
`--daemon` also runs the daemon for 15 s with `--debug` and with `--verbose`
against a temporary `--userdata` directory.

```sh
tests/cli/hyperhdr-cli-smoke.sh /usr/bin/hyperhdr
tests/cli/hyperhdr-cli-smoke.sh --daemon ./build/bin/hyperhdr
```
