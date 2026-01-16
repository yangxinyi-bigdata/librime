# 仓库指南

## 项目结构与模块组织
- `src/`：核心 C++ 引擎源码；内部头文件在 `src/rime/`。
- `plugins/`：可选的引擎插件与扩展。
- `test/`：单元测试（GoogleTest），命名为 `*_test.cc`。
- `data/`：预置的 YAML 数据与方案资源。
- `cmake/`：CMake 模块与构建辅助脚本。
- `tools/` 与 `bin/`：开发者工具与脚本。
- `deps/`：部分构建流程使用的第三方库。
- `build/` 与 `dist/`：生成的构建产物。

## 构建、测试与开发命令
- `make`：通过 CMake 生成并构建 Release 版本到 `build/`。
- `make debug`：生成并构建 Debug 版本到 `debug/`。
- `make test`：运行 Release 版本测试（`ctest --output-on-failure`）。
- `make test-debug`：运行 Debug 版本测试。
- `make clang-format-lint`：按 `.clang-format` 检查格式。
- `make clang-format-apply`：格式化 `*.cc` 与 `*.h` 源码。
- `make deps`：构建第三方依赖（macOS/Windows 常用）。
- Windows 辅助：`build.bat deps` 与 `build.bat librime`（见 `README-windows.md`）。

## 代码风格与命名约定
- 使用 C++17；通过 `clang-format` 和仓库 `.clang-format`（Chromium 风格）统一格式。
- 修改文件时遵循已有约定（命名、头文件布局、include 顺序）。
- 源码扩展名为 `.cc` 与 `.h`；测试遵循 `*_test.cc`。

## 测试指南
- 测试位于 `test/`，通过 CMake/CTest 接入。
- GoogleTest 为可选依赖；可用时请在提交前运行 `make test` 或 `make test-debug`。
- 新增测试应小而专注，并与相关模块放在一起。

## 提交与拉取请求规范
- 提交信息简短清晰；不少提交使用 Conventional Commits（如 `fix(component): ...`、`chore: ...`），合适时可沿用。
- 使用 `.github/pull-request-template.md` 模板。
- 说明变更、用 `Fixes #...` 关联问题，并按情况勾选单元/手动测试。
- PR 需通过 GitHub Actions、至少一位评审，并以 rebase 方式合并。

## 文档与平台说明
- 平台构建细节见 `README-mac.md` 与 `README-windows.md`。
- Linux 快速构建：`make` 后执行 `sudo make install`。
