# 在 Windows 上使用 Rime

## 先决条件

`librime` 已在以下构建工具与库的组合上通过测试：

- Visual Studio 2022 或 LLVM 16
- Boost>=1.83
- cmake>=3.10

Boost 与 cmake 的版本需要与所使用的较高版本 Visual Studio 匹配。

构建 OpenCC 词典需要 Python>=2.7。

## 获取源码

``` batch
git clone --recursive https://github.com/rime/librime.git
```
或从 [GitHub 下载](https://github.com/rime/librime)。

## 配置构建环境

将 `env.bat.template` 复制为 `env.bat`，并根据你的环境编辑该文件。
如果已安装 Boost 库，请将 `BOOST_ROOT` 设置为 Boost 源码根目录；如果使用不同版本的 Visual Studio，请修改 `BJAM_TOOLSET`、`CMAKE_GENERATOR` 和 `PLATFORM_TOOLSET`；如构建工具安装在自定义路径，也请设置 `DEVTOOLS_PATH`。

准备就绪后，请在“开发人员命令提示符”（Developer Command Prompt）窗口中执行以下步骤。

## 安装 Boost

此步骤会在 librime 的默认搜索路径下载 Boost 库。
如果你已在其他位置安装 Boost，请跳过本步骤并将环境变量 `BOOST_ROOT` 设置为已安装路径。

``` batch
install-boost.bat
```

## 构建第三方库

``` batch
build.bat deps
```
这会构建 `librime\deps\*` 下的依赖库，并将产物复制到 `librime\include`、`librime\lib` 和 `librime\bin`。

## 构建 librime

``` batch
build.bat librime
```
这会生成 `build\bin\Release\rime.dll`。

构建产物（共享库、API 头文件与支持文件）可在 `dist` 目录中找到。

## 在控制台试用

`librime` 附带了一个 REPL 应用，可用于测试库是否工作正常。

``` batch
cd build\bin
Release\rime_api_console.exe
congmingdeRime shurufa