# 在 macOS 上使用 Rime

在 macOS 上安装 librime 有两种方式：

## 1. 使用 Homebrew（推荐）

```sh
brew install librime
```

## 2. 手动编译

### 准备工作

安装 Xcode（包含命令行工具）。

安装其他构建工具：

``` sh
brew install cmake git
```

### 获取源代码

``` sh
git clone --recursive https://github.com/rime/librime.git
```
或从 [GitHub 下载](https://github.com/rime/librime)，然后单独获取第三方依赖的代码。

### 安装 Boost C++ 库

Boost 是 librime 大量依赖的第三方库，其中包含若干仅需头文件（header-only）的库。

**方案一（推荐）：** 从源码下载并构建 Boost。

``` sh
cd librime
bash install-boost.sh
```

该构建脚本会下载 Boost 源码压缩包，并解压到 `librime/deps/boost-<version>`。

在构建 librime 之前，将环境变量 `BOOST_ROOT` 设为 `boost-<version>` 目录的路径：

``` sh
export BOOST_ROOT="$(pwd)/deps/boost-1.84.0"
```

**方案二：** 通过 Homebrew 安装 Boost 库。

``` sh
brew install boost
# 如需使用 icu4c 构建，请把 icu4c 的安装路径加入 LIBRARY_PATH
export LIBRARY_PATH=${LIBRARY_PATH}:/opt/homebrew/opt/icu4c/lib:/usr/local/opt/icu4c/lib
```

如果只是为你自己的 Mac 本机构建并安装 Rime，这是一个省时的选择。

使用 Homebrew 版 Boost 构建得到的 `librime` 二进制在未安装相应 Homebrew formula 的机器上将不可移植。

**方案三：** 通过 Homebrew 安装较旧版本的 Boost。

自 1.68 版本起，Homebrew 提供的 `boost::locale` 依赖 `icu4c`，而 macOS 并不自带该库。

构建目标 `xcode/release-with-icu` 会指示 cmake 链接到通过 Homebrew 安装在本地的 ICU 库。这仅在使用
[`librime-charcode`](https://github.com/rime/librime-charcode) 插件时需要。

若要在使用该插件时制作可移植的构建，请安装一个不依赖 `icu4c` 的更早版本 `boost`：

``` sh
brew install boost@1.60
brew link --force boost@1.60
```

### 构建第三方库

除 Boost 外的必需第三方库已作为 git 子模块包含：

``` sh
# cd librime

# 若克隆时未使用 --recursive，请现在初始化子模块：
# git submodule update --init

make deps
```

上述命令会构建 `librime/deps/*` 下的各库，并将产物安装到 `librime/include`、`librime/lib` 和 `librime/bin`。

你也可以仅构建某个库，例如 `opencc`：

``` sh
make deps/opencc
```

### 构建 librime

``` sh
make release

make
```
这会生成 `build/lib/Release/librime*.dylib` 和命令行工具 `build/bin/Release/rime_*`。

或者创建调试构建：

``` sh
make debug
```

### 运行单元测试

``` sh
make test
```

或者测试调试构建：

``` sh
make test-debug
```

### 在控制台试用

``` sh
(
  cd debug/bin;
  echo "congmingdeRime{space}shurufa" | Debug/rime_api_console
)
```

作为 REPL 使用，按下 <kbd>Control+d</kbd> 退出：

``` sh
(cd debug/bin; ./rime_api_console)


只构建插件：
```
cmake --build build --target rime-aipara

# 这个是将rime构建出来的文件全部复制到squirrel构建目录当中
```
rm -rf /Users/yangxinyi/opt/100_code/100_rime_gui/squirrel-workspace/gpt5-scroll/lib/*

cp -a /Users/yangxinyi/opt/100_code/100_rime_gui/squirrel-workspace/gpt5-scroll/librime/build/lib/. /Users/yangxinyi/opt/100_code/100_rime_gui/squirrel-workspace/gpt5-scroll/lib/

sudo cp -a  /Users/yangxinyi/opt/100_code/100_rime_gui/squirrel-workspace/gpt5-scroll/librime/build/lib/rime-plugins/. /Library/Input\ Methods/Aipara.app/Contents/Frameworks/rime-plugins/

sudo codesign --force --sign - "/Library/Input Methods/Aipara.app"
```


重启 Squirrel，再验证 log_cpp 目录。
```
sudo killall Squirrel
open -a /Library/Input\ Methods/Aipara.app
/Library/Input\ Methods/Aipara.app/Contents/MacOS/Squirrel --reload
```
