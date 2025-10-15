<meta charset="UTF-8">

RIME: 中州韵输入法引擎
===
![Build status](https://github.com/rime/librime/actions/workflows/commit-ci.yml/badge.svg)
[![GitHub release](https://img.shields.io/github/release/rime/librime.svg)](https://github.com/rime/librime/releases)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

Rime，伴随你的每一次敲击。

项目主页
---
[rime.im](https://rime.im)

许可证
---
[3-Clause BSD 许可证](https://opensource.org/licenses/BSD-3-Clause)

特性
===
  - 一个模块化、可扩展的跨平台 C++ 输入法引擎，基于开源技术构建
  - 覆盖多种中文输入法的特性，包含形码与音码
  - 原生支持繁体中文，并可通过 OpenCC 转换为简体及其他地区规范
  - Rime 输入方案：使用 YAML 语法的 DSL，便于快速尝试创新的输入法设计
  - 拼写代数（Spelling Algebra）机制，可生成多样拼写，尤其适合中文方言
  - 支持在通用 Qwerty 键盘上的和弦输入（Chord-typing）

安装
===
在除 Linux 之外的平台，请按以下文档构建 librime：
  - [macOS 安装与构建指南](README-mac.zh-CN.md)
  - [Windows 安装与构建指南](README-windows.zh-CN.md)

构建依赖
---
  - 支持 C++17 的编译器
  - cmake>=3.12
  - libboost>=1.74
  - libglog>=0.7（可选）
  - libleveldb
  - libmarisa
  - libopencc>=1.0.2
  - libyaml-cpp>=0.5
  - libgtest（可选）

运行时依赖
---
  - libboost
  - libglog（可选）
  - libleveldb
  - libmarisa
  - libopencc
  - libyaml-cpp

在 Linux 上构建并安装 librime
---
```
make
sudo make install
```

前端
===

Official:
  - [ibus-rime](https://github.com/rime/ibus-rime)：Linux 的 IBus 前端
  - [Squirrel](https://github.com/rime/squirrel)：macOS 前端
  - [Weasel](https://github.com/rime/weasel)：Windows 前端

Community:
  - [emacs-rime](https://github.com/DogLooksGood/emacs-rime)：Emacs 前端
  - [coc-rime](https://github.com/tonyfettes/coc-rime)：Vim 前端
  - [rime.nvim](https://github.com/Freed-Wu/rime.nvim)：Vim 前端
  - [fcitx5.nvim](https://github.com/tonyfettes/fcitx5.nvim)：Vim 的 Fcitx5 前端
  - [fcitx5-ui.nvim](https://github.com/black-desk/fcitx5-ui.nvim)：Vim 的 Fcitx5 UI 前端
  - [zsh-rime](https://github.com/Freed-Wu/zsh-rime)：Zsh 前端
  - [pyrime](https://github.com/Freed-Wu/pyrime)：Ptpython 前端
  - [fcitx-rime](https://github.com/fcitx/fcitx-rime)：Linux 的 Fcitx 前端
  - [fcitx5-rime](https://github.com/fcitx/fcitx5-rime)：Linux 的 Fcitx5 前端
  - [fcitx5-macos](https://github.com/fcitx-contrib/fcitx5-macos)：macOS 的 Fcitx5 前端
  - [XIME](https://github.com/stackia/XIME)：macOS 前端
  - [PIME](https://github.com/EasyIME/PIME)：Windows 前端
  - [rabbit](https://github.com/amorphobia/rabbit)：Windows 前端
  - [Trime](https://github.com/osfans/trime)：Android 前端
  - [fcitx5-android](https://github.com/fcitx5-android/fcitx5-android)：Android 前端
  - [Hamster](https://github.com/imfuxiao/Hamster)：iOS 前端
  - [My RIME](https://github.com/LibreService/my_rime)：Web 前端

插件
===
  - [librime-charcode](https://github.com/rime/librime-charcode)（已废弃）：处理字符编码；依赖 boost::locale 与 ICU 库
  - [librime-legacy](https://github.com/rime/librime-legacy)（已废弃）：包含 GPL 许可代码的遗留模块
  - [librime-lua](https://github.com/hchunhui/librime-lua)：Lua 脚本扩展
  - [librime-octagram](https://github.com/lotem/librime-octagram)：语言模型
  - [librime-predict](https://github.com/rime/librime-predict)：下一词预测
  - [librime-proto](https://github.com/lotem/librime-proto)：使用 CapnProto 的进程间通信（IPC）

相关项目
===
  - [plum](https://github.com/rime/plum)：Rime 配置（配方）安装器
  - [combo-pinyin](https://github.com/rime/home/wiki/ComboPinyin)：一种创新的和弦输入式拼音练习
  - [rime-essay](https://github.com/rime/rime-essay)：预置词库
  - [SCU](https://github.com/neolee/SCU)：Squirrel 配置工具

致谢
===
感谢以下开源库的作者：

  - [Boost C++ Libraries](http://www.boost.org/)（Boost Software License）
  - [google-glog](https://github.com/google/glog)（3-Clause BSD 许可证）
  - [Google Test](https://github.com/google/googletest)（3-Clause BSD 许可证）
  - [LevelDB](https://github.com/google/leveldb)（3-Clause BSD 许可证）
  - [marisa-trie](https://github.com/s-yata/marisa-trie)（BSD 2-Clause、LGPL 2.1）
  - [OpenCC](https://github.com/BYVoid/OpenCC)（Apache License 2.0）
  - [yaml-cpp](https://github.com/jbeder/yaml-cpp)（MIT 许可证）

贡献者
===
  - [佛振](https://github.com/lotem)
  - [鄒旭](https://github.com/zouxu09)
  - [Weng Xuetian](http://csslayer.info)
  - [Chongyu Zhu](http://lembacon.com)
  - [Zhiwei Liu](https://github.com/liuzhiwei)
  - [BYVoid](http://www.byvoid.com)
  - [雪齋](https://github.com/LEOYoon-Tsaw)
  - [瑾昀](https://github.com/kunki)
  - [osfans](https://github.com/osfans)
  - [jakwings](https://github.com/jakwings)
  - [Prcuvu](https://github.com/Prcuvu)
  - [hchunhui](https://github.com/hchunhui)
  - [Qijia Liu](https://github.com/eagleoflqj)
  - [WhiredPlanck](https://github.com/WhiredPlanck)