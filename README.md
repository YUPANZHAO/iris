# Iris

Iris 是一款跨平台 SIP 会议核心。当前阶段已经完成 `libiris` 的基础目录、
基于 PJSUA2 的账户与呼叫生命周期、application-managed SDP、FFmpeg
音视频采集编解码、UDP RTP 通信，以及带原生音视频播放的命令行测试客户端。

## 目录结构

```text
include/iris/       公共 API，不暴露 PJSUA2 或 FFmpeg 类型
src/api/            Runtime 公共门面
src/core/           Runtime、账户、呼叫及任务调度
src/sip/            PJSUA2 Endpoint、Account、Call 和状态适配器
src/media/engine/          媒体会话状态机与稳定接口
src/media/sdp/             SDP 策略、PJPROJECT 解析适配和 CodecRegistry
src/media/transport/       PJLIB ioqueue、UDP RTP/RTCP 和基础 RTP 封装
src/media/backend/ffmpeg/  FFmpeg 采集、编解码和测试媒体源
src/media/render/          Win32/WinMM 原生音视频渲染
apps/iris-cli/      命令行测试客户端
tests/              库、接口编解码和媒体回环测试
third_party/        固定版本的 Git submodule
```

线程模型、对象所有权、关闭顺序和后续媒体边界参见
[docs/architecture.md](docs/architecture.md)。
媒体后端、transport、SDP 和媒体会话接口参见
[docs/media-interface-draft.md](docs/media-interface-draft.md)。

## Windows 开发环境

- Visual Studio 2026 或 2022，安装“使用 C++ 的桌面开发”工作负载
- CMake 3.28 或更高版本（Visual Studio 自带版本即可）
- Git

构建脚本在 Windows 默认使用 “Visual Studio 18 2026” 生成器，以原生
MSBuild 依赖跟踪避免中文版 MSVC `/showIncludes` 前缀在 Ninja 中被错误解码。
执行：

```powershell
.\scripts\bootstrap-vcpkg.ps1
.\scripts\build.ps1
.\build\windows\iris-cli.exe --self-test
```

构建脚本会在调用 CMake 前自动加载 `vcvars64.bat`，因此无需手动打开
Visual Studio Developer Command Prompt。

vcpkg 及其安装产物位于 `.tools/`，不会作为项目源码提交。构建使用动态
`x64-windows` triplet；FFmpeg、OpenSSL、Opus 和 OpenH264 的 DLL 会在构建后
自动复制到 `iris-cli.exe` 所在目录。

如果网络需要代理，请在运行 vcpkg 前设置 `HTTP_PROXY` 和 `HTTPS_PROXY`，
例如 `http://127.0.0.1:7890`。

## 使用已初始化的开发环境构建

```powershell
cmake -S . -B build/dev -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_INSTALLED_DIR="$PWD/.tools/vcpkg-installed" `
  -DVCPKG_MANIFEST_MODE=ON
cmake --build build/dev --config Release --parallel
ctest --test-dir build/dev -C Release --output-on-failure
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

使用 FFmpeg lavfi 测试源启动原生音视频客户端：

```powershell
.\build\windows\iris-cli.exe `
  --sip-port 5060 `
  --media-address 127.0.0.1 `
  --audio-input "lavfi:sine=frequency=440:sample_rate=48000" `
  --video-input "lavfi:testsrc=size=640x360:rate=15" `
  --video-width 640 --video-height 360 --video-fps 15
```

真实设备使用 FFmpeg 设备字符串，例如：

```text
--video-input "dshow:video=Camera Name"
--audio-input "dshow:audio=Microphone Name"
```

不指定输入时，程序选择 DirectShow 默认音频/视频设备；找不到设备时会明确
返回错误。无真实设备的环境可显式使用 `lavfi:` 测试源。
`--no-audio-render` 和 `--no-video-render` 可关闭原生播放，`--auto-answer`
可用于无人值守测试。

查看当前可用的麦克风和摄像头：

```powershell
.\build\windows\iris-cli.exe --list-devices
```

使用列表中的序号选择数据源，例如音频选择 `[0]`、视频选择 `[1]`：

```powershell
.\build\windows\iris-cli.exe `
  --audio-device 0 `
  --video-device 1 `
  --media-address 127.0.0.1
```

设备选择也支持完整设备 ID，以及以下特殊源：

- `auto`：DirectShow 默认设备
- `null`：空数据源。音频持续编码静音帧；视频使用黑灰渐变底，以微软雅黑
  白色字体居中显示当前账号 URI
- `lavfi:<graph>`：FFmpeg 测试源，例如
  `lavfi:sine=frequency=440:sample_rate=48000`
- `dshow:<device>`：直接传递 FFmpeg DirectShow URL

交互模式中也可以查看和切换下一通呼叫使用的数据源：

```text
media devices
media source audio 0
media source video null
media source
```

验证本机摄像头和麦克风采集链路，但不创建原生播放窗口：

```powershell
.\build\windows\iris-media-loopback.exe --capture-real
```

常用命令：

```text
account add <sip-uri> [registrar-uri] [username] [password]
account register <account-id>
call start <account-id> <sip-uri>
call answer <call-id>
call reject <call-id>
call hangup <call-id>
media stats <call-id>
media mute-audio <call-id>
media mute-video <call-id>
events
quit
```

当前版本使用 application-managed SDP 和项目自有 UDP/RTP 层。FFmpeg 负责
采集、缩放、像素转换、Opus 和 H.264 编解码；CLI 使用 Win32 窗口显示对端
视频，并使用 WinMM WaveOut 播放对端音频。NACK/RTX、拥塞控制和 WebRTC 音频
处理属于后续媒体后端阶段。

SDP 的语法解析和合法性验证复用 PJPROJECT `pjmedia_sdp_parse()` /
`pjmedia_sdp_validate2()`，offer/answer 策略仍由 Iris 管理。Opus 和
H.264 的远端格式匹配、本地优先级、Payload Type 与 fmtp 检查集中在
`CodecRegistry`。未知或不受支持的 m-line 会在 answer 中以端口 0 拒绝，
同时保持 m-line 数量和顺序，满足后续多流扩展需要。

UDP transport 默认使用 PJLIB/PJPROJECT ioqueue：运行时共享 PJSUA 媒体
endpoint 的 ioqueue；独立媒体测试则创建进程内共享 ioqueue。每个音视频流
使用独立的 UDP socket 和 RTCP-mux。

