# Contributing

Small, reproducible improvements are welcome. Include a failing fixture or statistical counterexample and explain the expected behavior. Preserve the public feature/schema contracts or version them explicitly. New detectors need a fair held-out evaluation against the existing baseline, including false positives and misses; aggregate accuracy alone is insufficient.

Build and run CTest before opening a pull request. On Linux, also run with `-DRD_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug`. C17 source should compile without warnings under the configured strict flags. Format C files with clang-format using the checked-in configuration. Test/benchmark scripts use only the Python standard library. Do not add real private telemetry or downloaded datasets to git.

Changes to serialized models need compatibility tests and a format version decision. Changes to alert interpretation need corresponding documentation and calibration tests. Contributions are provided under the repository's MIT license.
