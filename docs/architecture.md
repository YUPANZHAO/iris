# Iris 架构

## 设计目标

Iris 的媒体平面必须由项目自主控制。PJSUA2 负责 SIP 信令、注册和呼叫控制，
但不拥有业务音视频数据。项目自行生成和使用 SDP，自行管理媒体会话、采集、
编解码策略、RTP/RTCP、丢包恢复和拥塞控制。

当前已采用“App-managed SDP + 自定义 transport”作为过渡方案，并为后续
WebRTC 媒体后端预留稳定边界。接口草案见
[media-interface-draft.md](media-interface-draft.md)。

Windows 图形客户端使用 Rust-only Native Shell。UI 通过
`iris_bridge.dll` 的 C ABI 与 libiris 通信，控制面使用低频 JSON
命令/事件，视频面使用 BGRA frame sink；NSH Direct2D 原生组件在视图树内
绘制远端和本地画面。详细设计见 [ui-architecture.md](ui-architecture.md)。

## 两条平面

```text
信令平面
  PjEndpoint / PjAccount / PjCall
  AccountManager / CallManager
  SdpController
          │
          │ 呼叫状态和协商后的媒体参数
          ▼
媒体平面
  MediaEngine / MediaSession
  IMediaBackend
  IMediaTransport
  SdpController
```

信令平面和媒体平面只通过明确的数据结构和回调交互，不共享 PJMEDIA stream、
会议桥或音频端口对象。

## 当前代码结构

```text
src/api/runtime.cpp          Runtime 公共门面
src/ffi/c_api.cpp            NSH/C ABI 桥接
src/core/runtime_impl.*      Core 协调与事件队列
src/core/task_dispatcher.*   单 Core 线程命令调度
src/core/account_manager.*   账户表与注册生命周期
src/core/call_manager.*      呼叫表与呼叫生命周期
src/sip/pj_endpoint.*        PJSUA Endpoint 与 SIP Transport 生命周期
src/sip/pj_account.*         pj::Account 适配器
src/sip/pj_call.*            pj::Call 适配器
src/sip/state_mapper.*       PJSUA 到 Iris 的状态映射
src/media/engine/              媒体会话状态机与 IMediaBackend/Transport
src/media/sdp/sdp_controller.* SDP 策略及 PJPROJECT parser 适配
src/media/sdp/codec_registry.* 编码能力、优先级和远端 fmtp 匹配
src/media/transport/           ioqueue、UDP RTP/RTCP 和基础 RTP 封装
src/media/backend/ffmpeg/      FFmpeg 采集、编解码和测试媒体源
src/media/render/              Win32 视频窗口与 WinMM 音频输出
apps/iris-ui/                  Rust-only Native Shell 图形客户端
```

后续媒体阶段计划增加：

```text
src/media/backend/webrtc/*            后续 WebRTC 媒体实现
src/media/transport/webrtc.*          后续 WebRTC 网络传输
```

## App-managed SDP

账户默认启用：

```cpp
accountConfig.callConfig.mediaAppManaged = true;
```

呼叫创建时由 `SdpController` 生成完整 offer，并通过
`CallOpParam::sdp.wholeSdp` 提供给 PJSUA2。远端 answer 由媒体平面解析，
不会让 PJSUA 根据自身媒体配置重新生成 SDP。

SDP 的语法解析和基础合法性校验复用 PJPROJECT
`pjmedia_sdp_parse()` / `pjmedia_sdp_validate2()`；Iris 只保留业务层所需的
媒体方向、连接地址、Payload Type、rtpmap、fmtp 和 rtcp-mux 等字段。
offer/answer 匹配、CodecRegistry、Session ID/version 和媒体启停仍由 Iris
管理。answer 会保留远端 m-line 的数量和顺序，不支持或未同意的媒体以端口 0
和 `a=inactive` 拒绝，避免未知 m-line 破坏 RFC 3264 结构。

`CodecRegistry` 是唯一编码能力入口。当前注册 Opus/48000/2 和
H.264 Baseline/90000；选择时按 Iris 本地优先级遍历，同时检查 clock rate、
channels、packetization-mode、profile-level-id 和 fmtp。远端 Payload Type
会在 answer 和实际 RTP pipeline 中原样保留。

PJSUA 只负责：

- 发送和接收 INVITE、ACK、UPDATE、BYE 等 SIP 请求
- SIP 事务、对话和注册状态
- 传递应用提供的 SDP offer/answer
- 提供线路、代理、认证等信令配置

PJSUA 不负责：

- 选择实际采集和编码设备
- 创建 PJMEDIA audio/video stream
- 管理媒体 RTP/RTCP
- 处理 NACK、RTX、FEC 或拥塞控制
- 通过会议桥连接媒体

## 去呼流程

```text
CallManager.startCall()
    │
    ├─ MediaEngine.prepareOutgoing()
    │      ├─ 创建 MediaSession
    │      ├─ 打开 IMediaTransport
    │      ├─ 生成 SDP offer
    │      └─ 返回 CallOpParam.sdp
    │
    ├─ PjCall.makeCall(destination, callOpParam)
    │
    └─ PjCall.onCallTsxState() 收到 2xx SDP answer
           ├─ SdpController.applyRemoteAnswer()
           ├─ 更新 transport 目标与 CodecRegistry 选择结果
           └─ MediaEngine.reconcileActivity()
```

## 入呼流程

```text
PjAccount.onIncomingCall()
    │
    ├─ MediaEngine.prepareAnswer()
    │      ├─ 解析远端 SDP
    │      ├─ 创建本端 transport
    │      ├─ 选择 codec 和 RTP 参数
    │      └─ 生成 SDP answer
    │
    ├─ Call.answer(answerSdp)
    │
    └─ 200 OK/ACK 完成后
           └─ MediaEngine.start()
```

## 重协商与 hold

带 SDP 的 re-INVITE 由 `PjCall.onCallRxReinvite()` 异步交给 Core 线程。
Core 先解析和验证候选 SDP，再生成 Iris answer；验证失败返回 488，既有媒体
配置不会被替换。成功应答后更新远端地址、方向、编码和 Payload Type。

UPDATE offer 通过 `PjCall.onCallRxOffer()` 同步协商，但实际 SDP 构建仍在
Core 线程执行。`onCallTxOffer()` 强制保持 `PJSUA_CALL_MEDIA_APP_MANAGED`，
防止 PJSUA 重新接管媒体设置。

本地 hold/resume 不调用 PJSUA 的 `setHold()`。Iris 使用当前 transport 端口
生成 `sendonly` 或 `sendrecv` offer，通过 `Call.reinvite()` 的 wholeSdp
发送；收到远端 answer 后才暂停或恢复 backend。远端 hold 则根据远端 offer
方向生成反向 answer，并在双方均为 inactive 时暂停媒体。

重协商采用“先验证、后提交、再重启 backend”的顺序，不存在“先停旧媒体、
再发现 SDP 失败”的路径。backend 重启本身存在短暂切换窗口，但不会在协商
失败后丢失原有 SDP。

## 媒体数据路径

### 发送

```text
摄像头 / 麦克风
    ↓ FFmpeg 或原生采集接口
格式转换、缩放、重采样
    ↓
编码器
    ↓
RTP packetizer
    ↓ 完整 RTP 包
IMediaTransport.sendRtp()
```

### 接收

```text
IMediaTransport 接收回调
    ↓ 完整 RTP/RTCP 包
IMediaBackend.onReceivedPacket()
    ↓
抖动缓冲、丢包恢复、解码
    ↓
音频渲染 / 视频渲染
```

媒体后端与 transport 的边界是完整 RTP/RTCP 数据报。任何一层都不能对已经
封装好的 RTP 再次封装。

## FFmpeg 边界

当前过渡阶段使用 FFmpeg 完成：

- 音视频采集
- 视频缩放
- 像素格式转换
- 音频格式和采样率转换
- Opus、H.264 等部分编解码

FFmpeg 不负责：

- SIP 和 SDP 事务
- 媒体发送状态机
- 网络 RTP/RTCP 传输
- NACK、RTX、FEC
- 拥塞控制

过渡阶段会提供项目自有的基础 RTP packetizer 和 RTCP 处理逻辑，使
FFmpeg backend 能独立完成首轮媒体验证。后续 WebRTC backend 会替换这些
策略，但保持 `IMediaBackend` 和 `IMediaTransport` 边界不变。

## WebRTC 边界

后续 WebRTC 媒体后端负责：

- RTP 封装与解包
- 音频 NetEq、抖动缓冲
- NACK、RTX 和丢包恢复
- AEC、噪声抑制、自动增益
- 发送 pacing 和拥塞控制
- RTCP feedback、统计和码率估算
- 可选的 DTLS/SRTP 和 ICE

如果 WebRTC 继续使用项目 transport，则后端输出完整 RTP/RTCP 数据报，由
transport 发送。如果 WebRTC 自己管理 ICE、DTLS、SRTP 和 socket，则只替换
`IMediaTransport` 实现，SIP、SDP 和呼叫状态机不需要重写。

## 不依赖 PJMEDIA 媒体

本项目的媒体路径不启用或调用：

- `PJMEDIA_WITH_FFMPEG`
- PJMEDIA FFmpeg video device/codec backend
- PJMEDIA stream、会议桥和 `AudioMediaPort`
- `pjmedia_transport_send_rtp()`

PJMEDIA 库可能因为 PJSUA2 链接而存在，但 Iris 媒体后端不得调用其采集、编解码、
stream 或会议桥 API。

`3rdparty_media_sample` 展示了完全替换 pjsua 内部媒体实现的方法，但它依赖
`pjsua_internal.h` 和内部符号覆盖，不适合作为跨平台产品主架构。本项目采用
该 fork 的 application-managed SDP 能力，在公开边界上完成同样的媒体剥离。

## 线程模型

- SIP 和 SDP 控制操作在 Iris Core 线程执行。
- 请求的 SIP UDP 端口不可用时会自动回退到随机端口。
- 每路媒体拥有独立采集、编码和解码线程。
- 生产运行时，媒体 socket 注册到 PJSUA 媒体 endpoint 的共享 ioqueue。
- 纯媒体测试未初始化 PJSUA 时，使用一个进程内共享的 ioqueue 工作线程。
- RTP/RTCP 热路径直接连接 `IMediaTransport` 与 `IMediaBackend`。
- 每个媒体包不会进入 Core 事件队列。
- 媒体线程只向 Core 投递低频状态变化、错误和统计快照。
- 媒体回调不得阻塞、等待 UI 或持有 Core 状态锁。
- transport 退出码必须在销毁媒体对象前完成。

## 关闭顺序

每个呼叫的媒体关闭顺序固定为：

1. 将会话标记为非活动，阻止新的 packet handler 进入 backend。
2. 关闭 transport，通过 `pj_ioqueue_clear_key()` 取消挂起操作并等待
   ioqueue 回调退出，然后调用 `pj_ioqueue_unregister()`。
3. 停止 backend，等待采集、编解码和渲染线程退出。
4. 销毁后端编解码器、帧队列和原生窗口。
5. 释放会话并发布 `MediaStopped` 状态。

先关闭 transport 是当前实现的安全约束：backend 停止期间仍可能有已经进入
回调路径的 packet，只有等待 ioqueue 回调和挂起操作退出后，才能安全销毁
backend。关闭 socket 后，采集线程的发送会返回失败，并在随后 backend 停止
时退出。

Runtime 关闭时先发出所有挂断和注销请求，再由呼叫计数条件变量等待呼叫进入
Disconnected，并由注册计数条件变量等待 unregister 终态；各自最多等待
3 秒后才执行强制本地清理。随后按 Call、Media、ioqueue、Account、PJSUA
Endpoint 的顺序销毁对象。固定 250 ms 睡眠已移除。

## 构建边界

vcpkg 动态库阶段引入：

- FFmpeg：只链接到 Iris 的 FFmpeg media backend
- OpenSSL：SIP TLS 和后续安全媒体
- Opus：音频编解码
- OpenH264：视频编解码

pjproject 不启用其 FFmpeg backend。FFmpeg 的 include、lib 和 DLL 由 Iris
media target 自己管理，不通过 PJSUA2 的媒体实现转发。

Windows 默认使用 Visual Studio/MSBuild 生成器。部分中文版 MSVC 的
`/showIncludes` 前缀会被 CMake 错误解码，Ninja 因而无法可靠生成头文件依赖。
项目不再用“每个对象依赖全部项目头文件”的方式绕过该问题；MSBuild 提供
精确的增量依赖跟踪。构建脚本会根据已安装 Visual Studio 版本选择 2022 或
2026 生成器。

## 阶段状态

已经完成：

1. vcpkg 动态依赖接入和接口编译检查。
2. application-managed SDP 和 UDP RTP transport。
3. FFmpeg 音频采集、重采样、Opus 和基础 RTP。
4. 视频采集、缩放、像素转换、H.264 和 RTP FU-A 分片。
5. Win32 视频窗口与 WinMM 音频播放。
6. 本地媒体回环和双进程 SIP 音视频端到端测试。
7. DirectShow 设备枚举、运行时数据源切换和空数据源。
8. PJLIB ioqueue 音视频 UDP transport，并修复 RTCP 类型识别。
9. PJPROJECT SDP parser、CodecRegistry、本地编码优先级和保留 m-line 顺序。
10. re-INVITE/UPDATE 回调、Iris 管理的 hold/resume 和媒体参数重启。
11. 双进程 SIP 呼叫、hold/resume 与双向音频的 CTest 自动回归。

后续阶段：

1. 引入 WebRTC 音频处理、NACK/RTX、NetEq 和拥塞控制。
2. 根据平台与安全需求决定 ICE/DTLS/SRTP 由谁负责。
3. 增加多路 m-line、RTX/FEC、SSRC 和 RTP header extension 的 SDP 模型。

