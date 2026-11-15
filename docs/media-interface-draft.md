# 媒体接口草案

本文档描述媒体平面的第一阶段接口。当前已有可运行的
`application-managed SDP + UDP/RTP + FFmpeg` 实现，并保留接口供后续
WebRTC 后端替换。

## 稳定边界

```text
CallManager
    ↓
IMediaEngine
    ├── ISdpController
    ├── IMediaTransport
    └── IMediaBackend
```

`IMediaBackend` 是最重要的替换边界：

- 过渡阶段由 FFmpeg 和项目基础 RTP 策略实现
- 后续由 WebRTC 实现
- CallManager、账户和信令代码不随 backend 改变

`IMediaTransport` 只交换完整 RTP/RTCP 数据报：

- 过渡阶段使用 PJLIB ioqueue 封装 UDP socket；音视频各使用一个独立端口
- 后续可替换成 WebRTC ICE/DTLS/SRTP transport
- transport 不编码媒体，也不重新封装 RTP

## SDP 流程

去呼：

```text
MediaEngine.prepareOutgoing()
    → IMediaTransport.open()
    → ISdpController.createOffer()
    → CallOpParam.sdp.wholeSdp = offer
    → PjCall.makeCall()

远端 answer
    → ISdpController.applyRemoteAnswer()
    → IMediaBackend.start()
```

入呼：

```text
PjCall.onCallRxOffer(remoteOffer)
    → IMediaTransport.open()
    → ISdpController.createAnswer(remoteOffer)
    → Call.answer(answerSdp)
    → ACK 后 IMediaBackend.start()
```

重协商：

```text
PjCall.onCallRxReinvite()
    → 解析并验证新候选 SDP
    → CodecRegistry.select()
    → 生成新 answer/offer
    → Core 线程提交 transport 与媒体参数
    → 必要时停启 backend
```

UPDATE 使用 `onCallRxOffer()` 同步返回 wholeSdp answer；本地 re-INVITE 和
hold/resume 通过 `Call.reinvite()` 显式提供 Iris 生成的 wholeSdp offer。
所有路径都保持 `PJSUA_CALL_MEDIA_APP_MANAGED`。

## 后端职责

`IMediaBackend` 的职责是完整的媒体流水线：

- 打开采集与渲染设备
- 管理帧队列和实时线程
- 音频重采样与视频像素转换
- 编解码
- RTP/RTCP 封装与解析
- 抖动缓冲和丢包恢复
- 音视频同步
- 码率控制与拥塞控制
- 统计信息

WebRTC 的 packetizer、NACK/RTX 和拥塞控制是同一个状态机，不应被拆成多个
独立抽象接口。统一保留在 `IMediaBackend` 内部，可以避免重复维护 RTP 状态。

## Transport 职责

`IMediaTransport` 负责：

- 本地地址和端口或 ICE candidate
- 发送完整 RTP/RTCP 数据报
- 接收完整 RTP/RTCP 数据报
- DTLS/SRTP 等链路安全（由具体实现决定）
- transport 统计和网络错误

`IMediaTransport` 不负责：

- 解包 RTP header
- 处理 NACK/RTX
- 维护 jitter buffer
- 编解码
- 修改 SSRC、序号或时间戳

如未来使用 WebRTC 的全栈网络模块，`WebRtcTransport` 可以在内部完成 ICE、
DTLS 和 SRTP，但仍遵守同一条完整数据报边界。

## 状态机

```text
Idle
  ↓
Preparing
  ↓
Negotiating
  ↓
Starting
  ↓
Active ⇄ Reconfiguring
  ↓
Stopping
  ↓
Stopped
```

失败可以从 `Preparing`、`Negotiating` 或 `Starting` 进入 `Failed`。

## 线程约束

- `prepareOutgoing()`、`prepareAnswer()` 和 `applyRemoteAnswer()` 在 Core 线程调用。
- transport 接收回调运行在 PJLIB ioqueue 工作线程，必须在回调内复制或
  立即消费数据。
- backend 接收 RTP/RTCP 后直接进入媒体线程，不经过 Core 队列。
- `stop()` 必须可重复调用，并等待媒体线程安全退出。
- transport 的回调解除必须早于 transport 对象销毁。

## 当前接口文件

- 公共媒体类型：[include/iris/media.h](../include/iris/media.h)
- 内部后端、传输、SDP 和引擎接口：
  [src/media/engine/media_interfaces.h](../src/media/engine/media_interfaces.h)

接口仍处于演进阶段；但 `IMediaBackend`、
`IMediaTransport` 和完整 RTP/RTCP 数据报边界应保持稳定。

设备目录和输入选择使用公共类型 `MediaDeviceInfo`、
`MediaDeviceCatalog` 与 `queryMediaDevices()`。`Runtime::setMediaInput()`
更新下一通呼叫使用的数据源，不中断当前通话。支持设备序号、设备 ID、
`auto`、`null` 以及 FFmpeg `lavfi:`/`dshow:` 原始输入。

## 当前实现状态

已经实现：

- PJSUA application-managed SDP
- SDP offer/answer 保留对端给出的动态 RTP Payload Type
- PJPROJECT `pjmedia_sdp_parse()` / `pjmedia_sdp_validate2()` 解析验证
- CodecRegistry 管理 Opus/H.264、本地优先级和远端 fmtp 匹配
- answer 保留远端 m-line 顺序，不支持媒体以端口 0 拒绝
- SDP Session ID 稳定、re-offer version 递增
- re-INVITE、UPDATE、hold、resume 和 backend 参数重启
- Opus/48000/2 音频 SDP 与 RTP
- H.264/90000 视频 SDP 与 FU-A 分片
- 每个 H.264 IDR 随带 SPS/PPS；解码端等待首个可用关键帧
- FFmpeg DirectShow/lavfi 采集
- DirectShow 音视频设备枚举和按序号/设备 ID 选择
- `null` 空源：持续发送 Opus 静音帧，以及微软雅黑白字居中显示账号 URI
  的黑灰底 H.264 画面
- FFmpeg 缩放、像素格式转换和音频重采样
- Opus 编解码
- OpenH264 编解码
- UDP RTP 数据报发送与接收
- transport 固定音视频类型，不通过 Payload Type 猜测媒体种类
- PJLIB ioqueue 异步接收、128 个发送缓冲槽和预挂 8 个接收操作
- 完整修复 RTCP 8 位 packet type 分类
- Win32 原生视频窗口
- WinMM WaveOut 原生音频播放
- RTP 发送失败计数 `MediaStats::sendErrors`
- 本地媒体回环测试和双进程 SIP 端到端测试

尚未实现，保留给 WebRTC 后端：

- RTP 序列恢复、NACK/RTX 和 FEC
- 拥塞控制和发送 pacing
- WebRTC NetEq
- AEC、噪声抑制和自动增益
- ICE、DTLS 和 SRTP
- 多路 m-line、RTX/FEC、SSRC 和 RTP header extension 的完整 SDP 模型

