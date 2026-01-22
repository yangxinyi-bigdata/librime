# Windows（Win11）构建 librime（合并所有 plugins）并替换 Weasel 的 rime.dll

本文档用于在 Windows 上从源码构建 `librime`，并将 `plugins/` 目录下的所有插件（含 `aipara`、`librime-lua`、`librime-octagram`、`librime-predict`）合并进同一个 `rime.dll`，最后替换到小狼毫（Weasel）安装目录中使用。

> 说明：本仓库的 `plugins/librime-*` 在 git 里是 “gitlink(160000)” 形式；如果你的工作区里它们是空目录，需要按本文“准备插件源码”把内容拉下来，否则无法构建这些插件。

---

## 1. 前置条件

### 1.1 必备工具

- Visual Studio（建议 2022+，需要 C++ 桌面开发组件）  
  构建命令建议在 “x64 Native Tools Command Prompt for VS” 中运行。
- CMake（已安装即可）
- Git

### 1.2 依赖（建议用 vcpkg）

本文假设你的 vcpkg 安装在：`C:\opt\code\vcpkg`，并使用 triplet：`x64-windows`。

至少需要这些包：

- `zeromq`（提供 `libzmq*.dll`，`aipara` 需要）
- `libsodium`（`zeromq` 可能会依赖它）
- `lua`（`librime-lua` 需要）

你可以用类似命令安装（在 vcpkg 根目录执行）：

```bat
vcpkg install zeromq:x64-windows libsodium:x64-windows lua:x64-windows
```

### 1.3 Boost

工程需要 Boost（至少包含 `regex/filesystem/system/atomic` 等；本项目 CMake 会用到 Boost）。

本仓库默认 `env.bat` 里把 `BOOST_ROOT` 指向 `deps/boost_1_89_0`（可自行修改）。

---

## 2. 准备构建环境（env.bat）

仓库根目录有 `env.bat`（若没有可复制 `env.bat.template` 后修改）。

关键变量（可按你环境修改）：

- `BOOST_ROOT`：Boost 源码目录（其中应包含 `boost/`，并已编译出需要的 `.lib`）
- `ARCH=x64`
- `CMAKE_GENERATOR`：你的 VS 版本对应生成器（例如 VS2022 用 `"Visual Studio 17 2022"`）
- `EXTRA_CMAKE_PREFIX_PATH`：额外 CMake 包搜索路径（这里用 vcpkg 的 installed triplet）
  - 示例：`C:\opt\code\vcpkg\installed\x64-windows`

---

## 3. 准备插件源码（plugins/）

确保这些目录存在且包含源码（都有 `CMakeLists.txt`）：

- `plugins/aipara`
- `plugins/librime-lua`
- `plugins/librime-octagram`
- `plugins/librime-predict`

如果你的 `plugins/librime-*` 是空目录，用下面方式按指定 commit 拉取（示例使用本仓库记录的 gitlink 提交）：

```bat
cd /d C:\opt\code\librime

rem librime-lua
rmdir /s /q plugins\librime-lua
git clone --recursive https://github.com/hchunhui/librime-lua.git plugins\librime-lua
git -C plugins\librime-lua checkout 68f9c364a2d25a04c7d4794981d7c796b05ab627

rem librime-octagram
rmdir /s /q plugins\librime-octagram
git clone --recursive https://github.com/lotem/librime-octagram.git plugins\librime-octagram
git -C plugins\librime-octagram checkout dfcc15115788c828d9dd7b4bff68067d3ce2ffb8

rem librime-predict
rmdir /s /q plugins\librime-predict
git clone --recursive https://github.com/rime/librime-predict.git plugins\librime-predict
git -C plugins\librime-predict checkout 920bd41ebf6f9bf6855d14fbe80212e54e749791
```

---

## 4. 构建（deps + librime）

### 4.1 打开 VS x64 开发者命令行

建议从开始菜单打开：

- “x64 Native Tools Command Prompt for VS”

然后进入仓库根目录：

```bat
cd /d C:\opt\code\librime
```

### 4.2 构建第三方依赖（deps）

```bat
build.bat deps
```

### 4.3 构建 librime（合并 plugins）

关键点：不要设置 `RIME_PLUGINS`（保持未定义），这样会自动扫描 `plugins/` 下的所有插件目录并一起构建。

在 `cmd.exe` 下推荐这样显式“取消定义”：

```bat
set "RIME_PLUGINS="
build.bat librime
```

构建完成后输出在：

- `dist\lib\rime.dll`
- `dist\lib\rime.pdb`
- `dist\bin\*`

同时也会生成 `build\` 目录（VS 工程文件与中间产物）。

> 备注：构建日志里可能会看到 `pwsh.exe 不是内部或外部命令`、`LNK4044 /llibcmt` 之类警告，通常不影响最终生成 `rime.dll`。

---

## 5. 打包/部署到 Weasel（小狼毫）

假设 Weasel 安装目录为：`C:\Program Files\Rime\weasel-0.17.4`

### 5.1 将需要拷贝的文件复制到weasel-overlay文件夹当中

合并了插件后的 `rime.dll` 运行时依赖会增加，通常需要把下面这些 DLL 放到 Weasel 同目录（与 `WeaselServer.exe` 同级）：

- `dist\lib\rime.dll`（替换 Weasel 自带的 `rime.dll`）
- `lua.dll`（来自 vcpkg 的 `lua`，`librime-lua` 需要）
- `libzmq-mt-4_3_5.dll`（来自 vcpkg 的 `zeromq`，`aipara` 需要）
- `libsodium.dll`（来自 vcpkg 的 `libsodium`，`zeromq` 可能依赖它）

vcpkg 默认位置（Release）：

- `C:\opt\code\vcpkg\installed\x64-windows\bin\lua.dll`
- `C:\opt\code\vcpkg\installed\x64-windows\bin\libzmq-mt-4_3_5.dll`
- `C:\opt\code\vcpkg\installed\x64-windows\bin\libsodium.dll`

### 5.2 PowerShell 下带空格路径的执行方式

PowerShell 里运行 `.bat` 时建议用 `&` + 引号：

```powershell
& "C:\Program Files\Rime\weasel-0.17.4\stop_service.bat"
```

### 5.3 建议的替换流程

1) 停服务：

```powershell
& "C:\Program Files\Rime\weasel-0.17.4\stop_service.bat"
```

2) 备份原文件（至少备份 `rime.dll`）。

3) 复制文件到 Weasel 目录（示例 PowerShell）：

```powershell
Copy-Item -Force "C:\opt\code\librime\dist\lib\rime.dll" "C:\Program Files\Rime\weasel-0.17.4\rime.dll"
Copy-Item -Force "C:\opt\code\vcpkg\installed\x64-windows\bin\lua.dll" "C:\Program Files\Rime\weasel-0.17.4\lua.dll"
Copy-Item -Force "C:\opt\code\vcpkg\installed\x64-windows\bin\libzmq-mt-4_3_5.dll" "C:\Program Files\Rime\weasel-0.17.4\libzmq-mt-4_3_5.dll"
Copy-Item -Force "C:\opt\code\vcpkg\installed\x64-windows\bin\libsodium.dll" "C:\Program Files\Rime\weasel-0.17.4\libsodium.dll"
```

4) 启动服务：

```powershell
& "C:\Program Files\Rime\weasel-0.17.4\start_service.bat"
```

---

## 6. 常见错误排查

- **提示缺 `libsodium.dll`**：把 `libsodium.dll` 放到 Weasel 安装目录同级（与 `WeaselServer.exe` 同目录）。
- **提示缺 `VCRUNTIME140*.dll` / `MSVCP140.dll`**：安装 “Microsoft Visual C++ 2015-2022 Redistributable (x64)”。
- **只想构建某一个插件**：可设置 `RIME_PLUGINS` 只指定插件目录名，例如只构建 aipara：
  - `set "RIME_PLUGINS=aipara"`

