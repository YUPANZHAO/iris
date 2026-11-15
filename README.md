# Iris

Iris 是一款跨平台 SIP 会议核心。当前阶段已经完成 `libiris` 的基础目录、
基于 PJSUA2 的账户与呼叫生命周期，以及命令行测试客户端。媒体采集和
FFmpeg 编解码集成属于下一阶段。

## 目录结构

```text
include/iris/       公共 API，不暴露 PJSUA2 或 FFmpeg 类型
src/api/            Runtime 公共门面
src/core/           Runtime、账户、呼叫及任务调度
src/sip/            PJSUA2 Endpoint、Account、Call 和状态适配器
apps/iris-cli/      命令行测试客户端
tests/              库冒烟测试
third_party/        固定版本的 Git submodule
```

线程模型、对象所有权、关闭顺序和后续媒体边界参见
[docs/architecture.md](docs/architecture.md)。

## Windows 开发环境

- Visual Studio 2026 或 2022，安装“使用 C++ 的桌面开发”工作负载
- CMake 3.28 或更高版本
- Ninja
- Git

使用 Visual Studio 自带的 CMake 和 Ninja 即可。执行：

```powershell
.\scripts\build.ps1
.\build\windows\iris-cli.exe --self-test
```

构建脚本会在调用 CMake 前自动加载 `vcvars64.bat`，因此无需手动打开
Visual Studio Developer Command Prompt。

## 使用已初始化的开发环境构建

```powershell
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
```

## 第三方依赖与 Git

`third_party/pjproject` 是 Git submodule。主仓库只提交以下两项：

- `.gitmodules`，记录 submodule 地址和路径
- `third_party/pjproject` 的 gitlink，记录 pjproject 固定 commit

pjproject 的源码内容不需要上传到 Iris 主仓库，也不应作为普通文件提交。
克隆主仓库后执行 `git submodule update --init --recursive` 即可拉取对应版本。

## 命令行客户端

启动仅用于信令测试的交互式客户端：

```powershell
.\build\windows\iris-cli.exe --sip-port 0 --log-level 3
```

常用命令：

```text
account add <sip-uri> [registrar-uri] [username] [password]
account register <account-id>
call start <account-id> <sip-uri>
call answer <call-id>
call reject <call-id>
call hangup <call-id>
events
quit
```

当前阶段刻意不协商音频和视频媒体，以便先验证 SIP 传输、注册、入呼及呼叫做
状态管理，再引入独立媒体引擎。

