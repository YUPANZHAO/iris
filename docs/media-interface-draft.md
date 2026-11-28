# 媒体流程

Iris 的媒体实现位于 `iris-media`。`src` 只保留 SIP 与业务编排，不再维护另一套
媒体类型或会话实现。

## 对象所有权

```text
CallManager
    └── CallRecord
            └── MediaSession
                    └── SdpMediaControl
```

- `CallManager` 已经按 `CallId` 管理全部呼叫，因此不再额外维护
  `MediaSessionManager` 会话表。
- 每个 `CallRecord` 直接持有一个 `MediaSession`。
- `MediaSession` 只保存单个呼叫的 SDP、媒体流、输入源和统计状态。
- `MediaKind`、`MediaDirection`、音频格式和视频格式直接复用 `iris-media`
  的定义，公共 API 只提供兼容别名。

## 建立媒体

去呼：

```text
CallRecord 创建 MediaSession
    → prepareOutgoing()
    → 返回 SDP offer
    → setLocalDescription()
    → 收到远端 answer
    → applyRemoteAnswer()
```

入呼：

```text
CallRecord 创建 MediaSession
    → prepareAnswer(remoteOffer)
    → 返回 SDP answer
    → 远端 ACK 后 start()
```

重协商直接使用同一个 `MediaSession`。旧媒体流、transport 和 codec 不会被
重新抽象成另一层 engine 或 backend。

## 媒体链路

发送：

```text
EmptyCaptureSource / FFmpegCaptureSource
    → CaptureStream
    → negotiated Opus/H264 encoder
    → RtpPacketSender
    → RTPEngine
```

接收：

```text
UDP
    → RTPEngine
    → RtpMediaStream
    → negotiated decoder
    → video callback
```

`MediaSession` 同时持有发送流和接收流；`MediaControl::startAll()` 先注册接收
sink，再装配 RTPEngine，最后启动采集，避免首帧早于接收或发送链路就绪。

## 类型边界

- `iris-media` 是媒体类型的唯一定义源。
- `iris::MediaKind` 等公共类型是 `iris-media` 类型别名。
- `SdpDirection` 是 `iris::media::MediaDirection` 的兼容别名。
- 旧 `src/media`、`MediaSessionManager`、重复的媒体状态机以及多余的 backend
  抽象均已移除。

## 线程约束

- SIP 回调和 `CallManager` 的状态修改通过 Core 队列串行执行。
- RTP 接收在 transport 线程，解码后的视频通过回调交给 UI。
- 回调中的帧数据只在调用期间有效，需要异步使用时由调用方复制。
