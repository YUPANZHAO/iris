# Iris Native Shell 界面架构

## 目标与边界

Windows 是第一阶段正式目标。界面层采用 Native Shell（nsh）Rust-only
应用，不使用 TSX/QuickJS。C++ `libiris` 继续负责 SIP、SDP、账户、呼叫和
媒体生命周期；Rust UI 只负责展示、命令调度和原生视频绘制。

```text
NSH Rust UI
    │ typed state + JSON commands
    ▼
iris-ui/src/iris
    ├─ IrisClient 命令/事件工作线程
    ├─ UiState + IrisEvent
    └─ FrameStore / Direct2D native components
    │ stable C ABI
    ▼
iris_bridge.dll
    │
    ▼
libiris (PJSUA2 + FFmpeg + PJLIB ioqueue)
```

UI 不直接链接 PJSUA2 或 FFmpeg，也不持有 Call/Account 对象。

## 应用文件

```text
apps/iris-ui/
  nsh.json
  Cargo.toml
  src/
    lib.rs                  NSH 根组件与原生组件注册
    screens.rs              账号、拨号、来电和通话页面
    iris/
      client.rs             Runtime 工作线程与命令 API
      ffi.rs                iris_bridge.dll 动态加载和 C ABI
      model.rs              账户、呼叫、事件和媒体统计模型
      video.rs              帧邮箱、观察者和 Direct2D 组件
include/iris/c_api.h       桥接 C ABI
src/ffi/c_api.cpp            C++ 桥接实现
scripts/build-ui.ps1        构建并启动 UI
scripts/package-ui.ps1      打包并复制运行库 DLL
```

## 控制面

`iris_bridge.dll` 提供窄 C ABI：

- `iris_bridge_runtime_create`
- `iris_bridge_runtime_destroy`
- `iris_bridge_runtime_command`
- `iris_bridge_runtime_poll_events`
- `iris_bridge_runtime_snapshot`
- `iris_bridge_runtime_set_video_frame_sink`

控制命令使用 JSON，结果和事件也使用 JSON。频率低，便于跨语言演进和独立
测试。C++ 异常不会跨越 ABI，所有错误转换为 UTF-8 字符串。

Rust `IrisClient` 是 Runtime 的唯一所有者，运行一个命令/事件工作线程：

- UI 命令进入 `std::sync::mpsc`，不阻塞 NSH UI 线程。
- 每 80 ms 消费一次事件并刷新账户、呼叫和媒体统计快照。
- UI 每 100 ms 读取不可变 `UiState` 并更新 NSH signal。
- 退出时先解除视频 sink，再销毁 Runtime。

## 视频面

视频帧不经过 JSON。FFmpeg 解码线程调用 `VideoFrameCallback`，C++ 桥接把
BGRA 帧交给 Rust `FrameStore`：

```text
FFmpeg decode thread
    → VideoFrameView (remote/local preview)
    → iris_bridge_video_frame
    → FrameStore latest-frame map
    → ComponentContext.redraw()
    → NSH UI thread Direct2D CreateBitmap/DrawBitmap
```

`FrameStore` 每个 `(callId, kind)` 只保存最新一帧。Rust 在 C 回调返回前复制
像素；随后只保留 `Arc<StoredFrame>`，组件绘制时不会再次复制整帧。

注册两个 NSH 原生组件：

- `iris.remote-video`
- `iris.local-video`

组件使用 `NativeComponent::draw`，因此仍属于 NSH 视图树，通话信息、按钮、
弹窗可以正常叠在视频上。没有采用子 HWND，因为 Windows 上子 HWND 始终位于
所有 NSH 绘制内容之上。

## 当前页面

- SIP 账户配置和注册状态
- SIP 固定端口被占用时自动回退到系统随机端口，并将其同步到默认 URI
- 快速拨号
- 账户列表
- 来电提示与接听/拒绝
- 远端视频
- 本地视频画中画
- 麦克风静音
- 摄像头发送开关
- hold/resume
- 挂断
- 麦克风和摄像头可展开勾选的数据源列表
- `Automatic`、真实设备以及 `Empty source` 空源
- RTP 包、丢包和发送错误统计

密码目前只保存在进程内，不持久化。后续应接入 Windows Credential Manager
或 DPAPI，并增加配置文件、最近呼叫和设备设置页面。

## 窗口与输入框

主窗口使用 `frame: "none"`，由应用绘制顶部标题区，并通过
`windowDrag` 与 `windowControl` 提供拖动、最小化、最大化和关闭能力。
`defaultPosition: "center"` 让窗口在启动显示器的工作区内居中。

NSH 0.5.1 的 Windows 无窗口 RichEdit 在目标 DPI 恰好为 96 时可能绘制空
文本流，而 125% 显示器正常。输入封装因此保留 RichEdit 的编辑、IME、
光标和无障碍能力，并在 100% 显示器上用 NSH DirectWrite 文本叠加显示
当前值；密码值叠加为圆点。升级 NSH 后若该问题修复，可删除这一兼容层。

## 端口回退

`PjEndpoint` 首先尝试 `RuntimeConfig::sipPort`。UDP transport 创建失败且请求
端口不为 0 时，以端口 0 再创建一次 transport，并从 PJSUA transport info
读取操作系统分配的实际端口。`Runtime::sipPort()`、C ABI snapshot 和 UI
默认账户 URI 都使用该实际端口，因此可以在同一台机器运行多个客户端。

## 构建与运行

```powershell
.\scripts\build-ui.ps1
```

本机环回测试需要显式让 SDP 宣告可路由的媒体地址：

```powershell
.\scripts\build-ui.ps1 -MediaAddress 127.0.0.1
```

检查 Rust、NSH 和生成的类型接口：

```powershell
.\scripts\build-ui.ps1 -Check
```

生成 Windows 包：

```powershell
.\scripts\package-ui.ps1
```

输出位于 `apps/iris-ui/.nsh/out/Iris/`。脚本会把 `iris_bridge.dll` 和
vcpkg 动态运行库复制到应用 EXE 旁边。

## 后续工作

1. 增加设置持久化和 Windows 安全凭据存储。
2. 增加摄像头、麦克风和扬声器选择页面。
3. 将本地预览支持扩展到真实采集帧格式，减少 BGRA 中间转换。
4. 改用 D3D11/D2D GPU 纹理共享，避免每帧 CPU 上传。
5. 增加最近呼叫、联系人和多路视频布局。
6. 增加 NSH headless UI 测试和双进程 UI 集成测试。
7. 稳定 Windows 后，再实现 macOS/Linux 的视频 native component。
