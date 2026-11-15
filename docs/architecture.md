# Iris 架构

## 模块边界

`include/iris/` 是公共 API，不包含 PJSUA2 或 FFmpeg 类型。私有实现按职责拆分：

```text
src/api/runtime.cpp          Runtime 公共门面
src/core/runtime_impl.*      Core 协调与事件队列
src/core/task_dispatcher.*   单 Core 线程命令调度
src/core/account_manager.*   账户表与注册生命周期
src/core/call_manager.*      呼叫表与呼叫生命周期
src/sip/pj_endpoint.*        PJSUA Endpoint 与 Transport 生命周期
src/sip/pj_account.*         pj::Account 适配器与回调
src/sip/pj_call.*            pj::Call 适配器与回调
src/sip/state_mapper.*       PJSUA 到 Iris 的状态映射
```

Manager 负责持有业务状态。PJSUA 适配器只负责转换原生回调与操作请求。
`RuntimeImpl` 负责初始化、事件发布和有序关闭，不承担具体账户或呼叫逻辑。

第一阶段只实现信令，不协商媒体。下一阶段将在呼叫生命周期和传输层之间增加
独立的 `MediaEngine`。

## 线程与所有权

- 每个 `Runtime` 只能拥有一个 `pj::Endpoint`。
- Endpoint 在 Iris Core 线程中创建、使用和销毁。
- 公共操作统一提交到 Core 线程，并在 PJSUA2 接受对应操作后返回。
- PJSUA2 回调不会直接修改账户或呼叫映射，而是封装不可变数据后投递到 Core 线程。
- `PjAccount` 由 Runtime 私有的账户映射持有。
- `PjCall` 由 Runtime 私有的呼叫映射持有，直到 PJSUA2 报告 `DISCONNECTED`；
  实际移除发生在 PJSUA2 回调返回之后。
- 对外只暴露 `AccountId` 和 `CallId`。PJSUA 的原生账户 ID、呼叫 ID 始终是
  私有实现细节。

## 关闭流程

关闭任务按以下顺序执行：

1. 停止接收新的 PJSUA2 回调任务。
2. 挂断所有呼叫并请求注销所有账户。
3. 短暂等待 PJSUA2 回调线程结束。
4. 先销毁 Call 适配器，再销毁 Account 适配器。
5. 调用 `Endpoint::libDestroy()`。
6. 结束 Core 线程。

以上步骤全部在 Core 线程中执行。

## 后续媒体阶段

FFmpeg 负责媒体采集和编解码；PJMEDIA 继续负责 RTP、RTCP、SRTP、ICE、
抖动缓冲及 SDP 集成。FFmpeg 不得绕过 PJSUA2 创建并行 RTP 传输。

对于拉取式音频集成，将在 PJSUA2 `AudioMediaPort` 回调之后使用有界帧队列。
媒体回调不得阻塞、等待 UI 或持有 Core 状态锁。

