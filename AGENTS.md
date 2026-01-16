# Repository Guidelines

## Project Structure & Module Organization
- `src/`: core C++ engine sources; internal headers live under `src/rime/`.
- `plugins/`: optional engine plugins and extensions.
- `test/`: unit tests (GoogleTest), named `*_test.cc`.
- `data/`: preset YAML data and schema resources.
- `cmake/`: CMake modules and build helpers.
- `tools/` and `bin/`: developer utilities and scripts.
- `deps/`: vendored third-party libraries (used by some build flows).
- `build/` and `dist/`: generated build artifacts.

## Build, Test, and Development Commands
- `make`: configure and build a Release build via CMake into `build/`.
- `make debug`: configure and build a Debug build into `debug/`.
- `make test`: run tests for the Release build via `ctest --output-on-failure`.
- `make test-debug`: run tests for the Debug build.
- `make clang-format-lint`: check formatting against `.clang-format`.
- `make clang-format-apply`: apply formatting to `*.cc` and `*.h` sources.
- `make deps`: build third-party dependencies (mainly used for macOS and Windows).
- Windows helper: `build.bat deps` and `build.bat librime` (see `README-windows.md`).

## Coding Style & Naming Conventions
- C++17; formatting is enforced with `clang-format` using the repo `.clang-format`
  (Chromium style). Run the lint target before submitting changes.
- Match existing conventions in the file you edit (naming, header layout, includes).
- Source files use `.cc` and `.h`; tests follow the `*_test.cc` pattern.

## Testing Guidelines
- Tests are located in `test/` and are wired into CMake/CTest.
- GoogleTest is an optional dependency; if present, run `make test` or
  `make test-debug` before opening a PR.
- Keep new tests focused and co-located with related modules.

## Commit & Pull Request Guidelines
- Commit messages are short and descriptive; many follow Conventional Commits
  (e.g., `fix(component): ...`, `chore: ...`). Use that style when it fits.
- Use the PR template in `.github/pull-request-template.md`.
- Include a short description, link issues with `Fixes #...`, and check the
  unit/manual test boxes as applicable.
- PRs are expected to pass GitHub Actions CI, receive at least one review, and
  be cleanly rebase-merged.

## Documentation & Platform Notes
- Platform build details live in `README-mac.md` and `README-windows.md`.
- For Linux, the quick start is `make` then `sudo make install`.

## Communication Rule
- 必须使用中文回复。
