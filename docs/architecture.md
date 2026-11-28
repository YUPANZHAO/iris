# Iris 架构

Iris 由三个明确层次组成：

```text
include/iris/   公共 C++ API
src/            SIP、Runtime、CallManager 和业务编排
iris-media/     可复用的媒体执行实现
```

## Runtime

```text
Runtime
    └── RuntimeImpl
            ├── PjEndpoint
            ├── AccountManager
            ├── CallManager
            └── TaskDispatcher
```

- 所有 Core 状态修改通过 `TaskDispatcher` 串行执行。
- `PjEndpoint`、`PjAccount` 和 `PjCall` 只负责 PJSUA2 适配。
- `RuntimeImpl` 负责将公共 API 命令转发给对应的 Manager。

## 呼叫与媒体

```text
CallManager
    └── CallRecord
            ├── PjCall
            └── MediaSession
                    └── SdpMediaControl
```

- `CallManager::calls_` 是呼叫的唯一会话表。
- 每个 `CallRecord` 直接持有一个 `MediaSession`，不再额外维护媒体会话表。
- `MediaSession` 保存一个呼叫的 SDP、采集/编码/RTP 收发状态与统计。
- `SdpMediaControl` 属于 `iris-media`，负责 SDP 协商和流创建。

媒体发送链路：

```text
CaptureSource
    → CaptureStream
    → Opus/H264 encoder
    → RTPEngine
    → UDP
```

媒体接收链路：

```text
UDP
    → RTPEngine
    → RtpMediaStream
    → Opus/H264 decoder
    → video callback
```

## 类型边界

`iris-media` 是设备、codec、SDP、stream、transport 和媒体格式类型的唯一定义
来源。`include/iris/media.h` 只为公共 API 提供别名：

```text
iris::MediaKind          = iris::media::MediaKind
iris::MediaDirection     = iris::media::MediaDirection
iris::AudioFormat        = iris::media::codec::AudioFormat
iris::VideoFormat        = iris::media::codec::VideoFormat
```

SDP 层保留 `SdpDirection` 名称，但它也是 `iris::media::MediaDirection` 的别名。
不存在第二套媒体枚举。

## 线程

- SIP 回调把状态更新投递到 Core 队列。
- RTP 接收运行在 transport 接收线程。
- 视频解码和回调使用媒体线程，帧数据只在回调期间有效。
- `MediaControl::startAll()` 先打开接收流并注册 sink，再 attach RTPEngine，
  最后启动采集发送。

## 构建目标

- `iris::media`：engine、capture-source 和共享类型。
- `iris::media::codec`：FFmpeg Opus/H264。
- `iris::media::sdp`：SDP parser、builder 和 negotiator。
- `iris::media::pipeline`：CaptureStream 与 RtpMediaStream。
- `iris::media::transport`：UDP、RTPEngine 和端口分配。
- `iris::media::control`：MediaControl 与 SdpMediaControl。
- `iris::iris`：SIP、Runtime 与业务逻辑。

## 测试

- `iris-media` 测试覆盖 codec、SDP、RTP、采集、流编排和空采集源。
- `iris-media-loopback` 通过两个 `MediaSession` 验证真实本机 RTP 音视频链路。
- `iris-c-api-test` 验证 UI bridge 和公共 JSON 命令。
- `iris-runtime-smoke` 验证 Runtime 生命周期与媒体设备 API。
