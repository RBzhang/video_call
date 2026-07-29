# video_call

基于 Qt Widgets、OpenCV 和 Qt Multimedia 的 Windows 桌面音视频传输项目。当前已完成本机摄像头预览、JPEG/UDP 视频传输、ACL1 PCM 音频传输，以及 Qt 端远端 JPEG 解码显示；它不是完整的商用视频通话系统。

## 开发环境

- Windows 11
- Qt 6.11.1（`msvc2022_64` Kit）
- MSVC x64，C++17
- CMake
- OpenCV 4.12.0（x64 / `vc16` 预编译包）
- Qt Multimedia（Qt 6.11.1）

## 当前已完成

- CMake 已接入 OpenCV `core`、`imgproc`、`videoio` 组件，并完成 Debug/x64 构建验证。
- 主窗口使用并排的“本地视频 / 远端视频”区域；`VideoDisplayLabel` 根据当前 640×480 链路保持 4∶3，并以 `KeepAspectRatio` 显示完整画面。每张画面下方都有独立、不透明的状态面板，视频 QLabel 只显示画面或占位文字，状态不会叠加到图像上。视频网络与音频设置左右并排；音频统计固定为两行，设备和缓冲详情仅显示在 tooltip，因此统计刷新不会改变视频区域尺寸。
- `CameraWorker` 已通过 `moveToThread()` 运行在专用摄像头线程；GUI 线程只负责界面和 `QPixmap` 创建。
- `CameraWorker` 使用 `QTimer` 驱动 OpenCV 读取，依次尝试 DirectShow、Media Foundation 和自动后端。
- 已支持 BGR、BGRA、灰度帧到 `QImage` 的安全深拷贝转换。
- 已实现启动、停止、重复启动、连续读取失败处理，以及关闭主窗口时的线程收尾。
- 打开摄像头时会在状态标签和调试输出中依次报告 DSHOW、MSMF、ANY 三个后端的尝试、结果、OpenCV 实际后端和耗时，便于定位权限、设备占用或后端问题。
- 主窗口提供视频网络设置区域：可验证 IPv4 地址和端口、绑定本地 `QUdpSocket`、停止网络，并发送固定 50000 bytes 的确定性测试帧。
- 已实现 `QUdpSocket` 异步实际发送与接收。网络对象位于 GUI 线程，依赖 `readyRead` 和 Qt 事件循环，不创建新的网络线程。
- 已定义 UDP 视频分片协议 V1，并实现了固定 32 字节网络字节序协议头、数据报序列化、解析和已编码帧分片。
- `VideoFrameReassembler` 支持乱序分片、完全相同的重复分片、冲突重复分片丢弃、500 ms 未完成帧超时清理，以及 16 帧/16 MiB 接收缓存限制。
- 摄像头帧可在 `CameraWorker` 专用线程使用 OpenCV `cv::imencode()` 编码为 JPEG，并按目标帧率交给现有 VCL1 UDP 发送器。
- JPEG 发送使用独立的 `Qt::PreciseTimer` 调度，定时器与摄像头抓帧解耦；每次仅编码最新且尚未发送的新 BGR 帧，不会重复发送旧帧。
- 完整 JPEG 帧在 GUI 线程完成来源过滤后交给 `RemoteVideoDecoder` 专用 `QThread`；GUI 线程不执行 `cv::imdecode()`，也不从 Worker 线程操作 QWidget。
- 解码调度始终最多保留“一帧处理中 + 一帧最新待处理”。解码较慢时旧 pending 帧会被新帧覆盖，避免无限 queued-signal 积压和旧画面延迟。
- 远端接收独立于本地摄像头与本地发送；状态栏显示接收/显示 FPS、平均 JPEG 大小、JPEG payload 码率、解码耗时、覆盖帧、失败帧、外源帧和不支持帧。
- 已加入 CTest 协议、重组器、双端点真实 UDP 回环、JPEG 编解码和帧率区间计算测试，覆盖单分片、多分片、最大负载、常见格式错误、乱序重组、JPEG 边界标记与尺寸验证，以及 1–30 FPS 的调度区间。
- 已实现独立 ACL1 音频协议、独立 UDP Socket/端口、`AudioJitterBuffer` 和运行在专用 `QThread` 的 `AudioWorker`。麦克风采集、扬声器播放、定时播放和网络收发均不在 GUI 线程运行。
- 音频固定为 16 kHz、单声道、Little Endian signed Int16 PCM；每 20 ms 发送一个 640-byte payload，不压缩、不分片。
- 启动音频时严格检查默认输入和输出设备是否支持固定格式；不支持时不会隐式重采样或以不一致格式启动。
- 关闭窗口会先在 AudioWorker 所在线程停止 Source/Sink、定时器和 UDP Socket，再 `quit()`、无超时 `wait()` 并删除 `QThread`。为避免 Qt Multimedia 的退出锁阻止最后窗口关闭，程序显式禁用了 quit lock；不使用 `QThread::terminate()`。

## 明确尚未实现

- 音频压缩、Opus、AAC
- AEC、降噪、自动增益、混音和音视频同步
- 设备选择下拉框、呼叫控制和在线检测
- ACK、丢包重传与前向纠错
- TCP、GStreamer、FFmpeg API、WebRTC
- H.264、加密、NAT 穿透和身份认证

## UDP 视频分片协议 V1

协议层使用 `QDataStream` 显式按字段以 Big Endian 序列化，不传输 C++ 结构体原始内存。`VideoFrameReassembler` 仅处理已经解析的 UDP 数据报，不依赖 GUI、OpenCV、摄像头或 JPEG。

- Magic：`VCL1`（`0x56434C31`）
- Version：`1`
- 网络字节序：Big Endian
- Header：`32 bytes`
- 最大 UDP 数据报：`1200 bytes`
- 最大分片负载：`1168 bytes`
- 最大编码帧：`4 MiB`

| 字段 | 大小 |
| --- | ---: |
| magic | 4 bytes |
| version | 1 byte |
| packetType | 1 byte |
| headerSize | 2 bytes |
| sessionId | 4 bytes |
| frameId | 4 bytes |
| timestampMs | 4 bytes |
| frameSize | 4 bytes |
| fragmentIndex | 2 bytes |
| fragmentCount | 2 bytes |
| payloadSize | 2 bytes |
| flags | 2 bytes |

解析会拒绝错误魔数、错误版本、截断数据报、多余字节及不一致的分片长度。任一分片丢失时，接收端会在 500 ms 后丢弃未完成帧；当前不做重传。这是面向实时视频低延迟基础设计的取舍，不代表 UDP 可靠传输。

## UDP PCM 音频（ACL1）

音频不使用 VCL1，不占用视频 Socket，也不参与视频分片。`AudioUdpTransport` 使用独立 UDP Socket 和独立端口；默认本地、对端音频端口都是 `5002`。网络格式是固定的 16 kHz、单声道、signed Int16、Little Endian PCM：每包 20 ms、320 个采样、640-byte payload、每秒 50 包，PCM payload 码率为 `0.256 Mbit/s`。该格式没有编码器、音频分片或可变 payload。

ACL1 的 32-byte 头始终按 Big Endian 网络字节序序列化；PCM payload 则保持 Little Endian signed Int16。完整数据报严格为 `672 bytes`，解析会拒绝截断、尾随字节、未知类型和任何固定字段不一致的数据报。

| 字段 | 大小 | 固定值或含义 |
| --- | ---: | --- |
| magic | 4 bytes | `ACL1`（`0x41434C31`） |
| version | 1 byte | `1` |
| packetType | 1 byte | `1`（PCM） |
| headerSize | 2 bytes | `32` |
| sessionId | 4 bytes | 每次启动发送生成非零随机值 |
| sequence | 4 bytes | 从 `1` 开始，回绕时跳过 `0` |
| timestampSamples | 4 bytes | 每包增加 `320`，自然回绕 |
| sampleRate | 4 bytes | `16000` |
| channels | 2 bytes | `1` |
| sampleFormat | 2 bytes | `1`（signed Int16） |
| samplesPerChannel | 2 bytes | `320` |
| payloadSize | 2 bytes | `640` |

接收端使用纯逻辑 `AudioJitterBuffer`：三包预缓冲（60 ms）、最多十包（200 ms）、小范围乱序排序、重复包丢弃和迟到包丢弃。播放缺失包时写入严格的 640-byte 全零静音；连续缺失五包后退出播放状态并重新预缓冲。不会动态变速、拉伸或重采样。

`AudioWorker` 位于独立 `QThread`，拥有 `AudioUdpTransport`、`QAudioSource`、`QAudioSink`、采集/播放 `QIODevice`、20 ms `Qt::PreciseTimer` 和 1 秒统计定时器。采集端把不规则 `readyRead()` 数据累积后按 640 bytes 切包，采集缓存上限为 6400 bytes；播放端正确处理部分写入，待写缓存最多五包。固定两行状态面板显示实际发送/接收 packets/s、payload Mbit/s、抖动深度、静音补偿、重复/迟到/外源/无效包和输入/播放溢出；输入/输出设备与实际 Source/Sink bufferSize 仅在 tooltip 中提供。

点击“应用音频设置”时复用现有“对端 IP”输入框并验证 IPv4；成功绑定后才可以点击“开始双向音频”。启动时仅使用 `QMediaDevices` 的默认输入和输出，且两者都必须支持 16 kHz / 单声道 / Int16；不支持会显示设备描述及 preferred format，且不会启动或回退为其他网络格式。停止音频只停止音频，不停止摄像头、视频 UDP、视频编码或远端视频显示；重新应用音频设置会停止当前音频、清空抖动缓冲并重新绑定。

同机双实例音频测试应使用交叉端口：

| 实例 | 对端 IP | 本地音频端口 | 对端音频端口 |
| --- | --- | ---: | ---: |
| A | `127.0.0.1` | 5002 | 5003 |
| B | `127.0.0.1` | 5003 | 5002 |

两台电脑测试时，两边都使用本地/对端 `5002`，对端 IP 填另一台电脑的局域网 IPv4，并允许 Windows 防火墙的 UDP 入站访问。应使用耳机：当前没有 AEC，扬声器声音被麦克风再次采集导致的啸叫不是 UDP 故障。音频和视频的端口、Socket、来源过滤、线程和状态均相互独立；协议不检测对端在线、不发送 ACK、不重传、不做 FEC。

## 构建

推荐使用 Qt Creator，选择 Qt 6.11.1 MSVC x64 的 Debug Kit 后直接配置和构建。

修改 `QObject` 派生类的成员后，建议重新运行 CMake 并执行一次全量构建，避免复用旧的目标文件或自动生成文件。

项目在未由外部指定时使用以下 OpenCV CMake 配置目录：

```text
C:/Opencv/opencv/build/x64/vc16/lib
```

也可以在 CMake 配置时显式指定：

```text
-DOpenCV_DIR=C:/Opencv/opencv/build/x64/vc16/lib
```

## 运行时 DLL

实际调用 OpenCV 后，运行环境需要能够找到其 DLL 目录：

```text
C:\Opencv\opencv\build\x64\vc16\bin
```

可在 Qt Creator 的 Run Environment 中将其加入 `PATH`，或在后续添加 CMake 的部署/复制规则。该仓库不会提交 `build` 等生成目录或编译产物。

若需要 OpenCV 更详细的内部 videoio 日志，可仅在调试运行环境中增加：

```text
OPENCV_VIDEOIO_DEBUG=1
```

不要将该变量写入系统环境变量或提交到工程配置。

## 自动测试

完成 CMake 配置和构建后，可在构建目录运行：

```bash
ctest --output-on-failure
```

当前 CTest 包含九个独立控制台测试目标：

- `video_packet_protocol_test`：协议序列化、解析与分片。
- `video_frame_reassembler_test`：乱序、重复、冲突、超时和缓存限制重组测试。
- `video_udp_transport_test`：两个 `VideoUdpTransport` 端点使用系统分配端口进行真实 UDP 回环测试。
- `jpeg_frame_encoder_test`：JPEG 编码与解码边界和尺寸验证。
- `jpeg_frame_decoder_test`：确定性 JPEG 解码、灰度 JPEG、深拷贝，以及空输入、随机字节、截断、缺少 EOI 和超过 4 MiB 输入拒绝。
- `audio_packet_protocol_test`：ACL1 固定头 Big Endian 序列化、严格解析和全部固定字段拒绝测试。
- `audio_jitter_buffer_test`：三包预缓冲、乱序、重复、丢包静音、五包重缓冲、十包上限、session 重置和 sequence 回绕测试。
- `audio_udp_transport_test`：两个 `AudioUdpTransport` 端点用系统分配端口进行双向、50 包连续、外源过滤、错误数据报和重新绑定测试。
- `video_frame_rate_utils_test`：1–30 FPS 的毫秒区间计算，以及非法 FPS 拒绝。

协议、重组器、UDP 回环和帧率工具测试不依赖 Qt Widgets；视频/音频 UDP 回环测试依赖 Qt Network；JPEG 编码和解码测试依赖 Qt Core/Gui 与 OpenCV。所有 CTest 都是无交互控制台测试，不需要显示 GUI 窗口。UDP 回环测试使用系统分配端口，不固定占用 5000、5001、5002 或 5003，测试完成后关闭 Socket。

最近一次全新 Debug/x64 构建的 `ctest --output-on-failure` 为 `9/9` 通过。`udp_test_receiver.py` 只需要 Python 标准库；`udp_jpeg_receiver.py --self-test` 需要安装 Python OpenCV（`cv2`）。

## 同机双实例测试

可启动两个 `video_call` 实例并分别应用以下配置：

| 实例 | 对端 IP | 本地端口 | 对端端口 |
| --- | --- | ---: | ---: |
| A | `127.0.0.1` | 5000 | 5001 |
| B | `127.0.0.1` | 5001 | 5000 |

两边成功绑定后，任一实例点击“发送测试帧”，另一实例应显示收到有效的 50000 bytes 测试帧。停止网络后可再次绑定。

### Qt → Qt JPEG 预览

同机只有一个摄像头时，可只在 A 启动摄像头并按以下顺序操作：

1. A、B 分别应用上表的网络设置。
2. 在 A 启动摄像头，设置 `10 FPS`、JPEG quality `60`，再点击“开始发送视频”。
3. B 的本地画面保持“摄像头未启动”，右侧远端画面应显示 A 的视频，并约每秒刷新远端统计。
4. A 点击“停止发送视频”后，B 不再收到新帧并显示“等待对端 JPEG”；再次开始发送后显示恢复。
5. B 仍可点击“发送测试帧”，A 应显示有效的 50000 bytes 确定性测试帧。

两台真实电脑可以都使用本地端口 5000，但对端 IP 必须填写另一台电脑的局域网 IPv4 地址。两边应用网络设置、各自启动摄像头并开始发送视频后，左侧是本机画面、右侧是对端画面。Windows 防火墙必须允许对应的 UDP 入站端口。本协议不发送 ACK、不重传，也不检测对端在线状态；关闭一端时另一端不会得到“离线”通知。

### 远端接收统计与解码策略

远端统计中的 FPS 以真实统计窗口时间计算。payload 码率只计算 JPEG payload，不包含 VCL1 头、UDP、IP 或以太网开销。`superseded` 表示一个正在解码的 JPEG 后又收到更新帧，旧 pending 帧被替换的次数；这是一项降低实时延迟的正常统计，而不是 UDP 重传。

来源 IP 或端口与当前配置对端不一致的完整帧会计入 `foreign-source` 后直接丢弃。完整帧先区分确定性测试帧与 JPEG；未知格式只计入 `unsupported`，不会关闭 UDP。

## Python 跨电脑 UDP 验证

`tools/udp_test_receiver.py` 是一个只使用 Python 3.9+ 标准库的 VCL1 UDP 接收与验证程序。它严格解析 32-byte Big Endian 协议头，以“发送端 IPv4 + 发送端端口 + sessionId + frameId”为重组键，支持乱序、完全相同的重复分片、冲突分片丢弃、500 ms 超时清理，以及 16 帧 / 16 MiB 未完成帧缓存限制。

先验证 Python 程序本身：

```bash
python -m py_compile tools/udp_test_receiver.py
python tools/udp_test_receiver.py --self-test
```

自测试会生成并倒序重组一帧 50000-byte 确定性测试帧，检查 43 个分片、重复分片、错误 magic、截断数据报和超时清理；成功时输出 `SELF-TEST PASSED`。

### 同一台电脑测试

先启动 Python 接收器：

```bash
python tools/udp_test_receiver.py --bind 127.0.0.1 --port 5001 --once
```

然后在 Qt 程序中设置：

| 设置项 | 值 |
| --- | --- |
| 对端 IP | `127.0.0.1` |
| 本地视频端口 | `5000` |
| 对端视频端口 | `5001` |

点击“应用网络设置”后，再点击“发送测试帧”。Python 应输出 `[OK]`，其中 `size=50000`、`fragments=43`，并显示测试帧 sequence；`--once` 会在成功校验后以退出码 0 结束。Python 已绑定 5001 时，Qt 程序不能再绑定相同的本地端口。

### 两台电脑测试

电脑 B 运行：

```bash
python tools/udp_test_receiver.py --bind 0.0.0.0 --port 5000 --once
```

电脑 A 的 Qt 程序设置：

| 设置项 | 值 |
| --- | --- |
| 对端 IP | 电脑 B 的局域网 IPv4 |
| 本地视频端口 | `5000` |
| 对端视频端口 | `5000` |

两台不同电脑都可以使用本地 5000。Windows 可能显示 Python 防火墙提示，应允许专用网络访问；也可以手动放行 UDP 5000。Qt `writeDatagram()` 成功只表示本地写入成功，只有 Python 输出 `[OK]` 才证明接收端已经收齐并校验通过。本项目不发送 ACK，也不检测对端是否在线。

此验证工具不实现 JPEG、摄像头帧 UDP 发送、远端视频显示或音频。

## 摄像头 JPEG UDP 发送

启动本地摄像头后，可将 640×480 摄像头帧编码为 JPEG 并单向发送到 Python 接收端。默认参数为目标 `10 FPS`、JPEG quality `60`。JPEG 编码始终在 `CameraWorker` 所在线程完成；GUI 线程只接收已拥有数据的 `QByteArray` 并调用已有的 `VideoUdpTransport`，不会创建新的网络线程。

在“视频网络设置”区域完成 UDP 绑定、启动摄像头后，设置“发送帧率”和“JPEG 质量”，再点击“开始发送视频”。开始发送时会禁用确定性“发送测试帧”按钮和两个编码参数输入；停止发送后会恢复它们。停止连续发送不会停止本地预览或关闭 UDP。重新应用网络设置、停止网络、停止/报错摄像头、UDP 本地错误和关闭窗口都会停止连续发送。

发送状态约每秒显示实际成功提交给 `QUdpSocket::writeDatagram()` 的 JPEG payload 统计：目标 FPS、按真实经过时间计算的实际 FPS、平均 JPEG KB/帧、JPEG payload Mbit/s、平均分片数和 JPEG 编码耗时。该码率不包含 IP、UDP 或以太网开销；`writeDatagram()` 成功也不表示对端已收到。发送端不等待 ACK，也不检测对端是否在线。

## Python JPEG 接收

`tools/udp_jpeg_receiver.py` 复用 `udp_test_receiver.py` 中的 VCL1 协议解析和重组器，并使用 OpenCV Python 解码和显示 JPEG。标准库测试接收器仍不需要第三方依赖；JPEG 显示接收器需要安装：

```bash
python -m pip install opencv-python
```

本机或另一台电脑接收并显示：

```bash
python tools/udp_jpeg_receiver.py --bind 0.0.0.0 --port 5000
```

无窗口验证一帧：

```bash
python tools/udp_jpeg_receiver.py --bind 127.0.0.1 --port 5001 --no-display --once
```

保存第一张成功解码的原始 JPEG：

```bash
python tools/udp_jpeg_receiver.py --bind 0.0.0.0 --port 5000 --save-first received_first.jpg
```

显示窗口中按 `q` 或 `ESC` 退出。接收器首次成功解码时输出来源、帧 ID、JPEG 大小、分辨率和分片数，之后每秒输出实际 FPS、JPEG payload 码率、平均 JPEG 大小、重组完成数、JPEG 解码失败数、超时帧与拒绝数据报数。`--no-display --once` 适合无窗口验证；第一帧成功解码后以退出码 0 结束。

### 本机 JPEG 验证

Python：

```bash
python tools/udp_jpeg_receiver.py --bind 127.0.0.1 --port 5001
```

Qt：

| 设置项 | 值 |
| --- | --- |
| 对端 IP | `127.0.0.1` |
| 本地视频端口 | `5000` |
| 对端视频端口 | `5001` |
| 发送帧率 | `10 FPS` |
| JPEG 质量 | `60` |

依次应用网络设置、启动摄像头、开始发送视频。Python 应持续显示实时画面；停止发送后不应继续收到新 JPEG，本地摄像头预览仍继续。再次开始发送可恢复传输。两台电脑测试时，电脑 B 可监听 `0.0.0.0:5000`，电脑 A 的 Qt 对端 IP 填电脑 B 的局域网 IPv4；不同电脑都可使用本地端口 5000。Windows 可能要求允许 Python 使用专用网络，或手动放行对应 UDP 端口。

### JPEG 发送节流实测

使用全新 Debug/x64 构建目录 `build-pacing-clean-debug-x64`，本机 640×480、JPEG quality 60、DSHOW 摄像头和 `udp_jpeg_receiver.py --no-display`，每档连续发送至少 30 秒。Qt 和 Python 的 FPS 都按各自统计窗口内的实际经过时间计算。

| 目标 FPS | Qt 实际 FPS | Python 实际 FPS | Qt JPEG KB/帧 | payload Mbit/s | 分片/帧 | Qt 编码 ms/帧 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 5 | 5.0 | 4.9 | 10.7 | 0.44 | 10.0 | 6.9 |
| 10 | 9.9 | 9.9 | 12.0 | 0.97 | 11.0 | 6.9 |
| 15 | 15.1 | 14.6 | 12.9 | 1.60 | 12.0 | 8.3 |

三档均未出现 Python 重组超时、JPEG 解码失败或拒绝帧。停止 10 FPS 发送后，Python 已完成帧数在 3 秒观察窗口保持 `56` 不变；改为 15 FPS 并重新开始后，Qt 状态显示实际 `15.2 FPS`，接收完成帧数继续增长至 `136`。

## 摄像头后端诊断与退出行为

摄像头打开是设备驱动调用，`cv::VideoCapture::open()` 对 Windows 的 DirectShow 和 Media Foundation 后端可能同步阻塞。OpenCV 的 `CAP_PROP_OPEN_TIMEOUT_MSEC` 只适用于 FFmpeg/GStreamer，不能用来可靠限制本机摄像头后端的打开时间。

关闭主窗口时，GUI 线程会确认自己不是摄像头线程，再以 `Qt::BlockingQueuedConnection` 在 `CameraWorker` 所在线程同步调用 `stopCamera()`。该调用会停止 `QTimer`、释放 `cv::VideoCapture`，随后在摄像头线程调用 `QThread::quit()`，并由 GUI 线程无超时地 `QThread::wait()`。仅在 `wait()` 确认工作线程结束后才删除 `QThread`；不会让摄像头线程在 `QApplication` 退出后继续运行，也不会手动删除 `CameraWorker`。

关闭时还会先关闭 UDP、递增远端接收 generation、清空 pending JPEG 并停止远端统计，然后让 `RemoteVideoDecoder` 线程退出并 `wait()` 成功后删除其 `QThread`。正在进行的一次 `cv::imdecode()` 可以自然完成，但其旧 generation 结果不会再显示。调试输出会记录摄像头 Worker 和远端 JPEG 解码线程的停止、`quit()`、`wait()` 与对象析构顺序，可用于核对退出阶段的对象生命周期。

音频关闭同样不会遗留后台线程：GUI 线程确认自己不在 AudioWorker 线程后，用 `Qt::BlockingQueuedConnection` 调用 `AudioWorker::shutdown()`；该槽会停止并在线程内删除 `QAudioSource`/`QAudioSink`、播放和统计定时器、关闭独立音频 UDP Socket 并清空缓存。随后在音频线程调用 `quit()`，GUI 线程无超时 `wait()`，确认结束后才删除 `QThread`；不会使用 `terminate()`、detach 或手动删除 AudioWorker。`main.cpp` 先在 GUI 线程初始化 Qt Multimedia 的默认设备后端，并显式禁用 quit lock，避免后端定时器跨线程析构或退出锁阻止最后窗口关闭。

在全新 Debug/x64 构建中，应覆盖以下退出场景：未启动摄像头/UDP、仅绑定 UDP、摄像头运行、摄像头已停止、JPEG 发送、JPEG 接收解码，以及发送与接收同时进行。每次均应在 2 秒内以 exit code 0 结束，且没有 `video_call.exe` 残留、Visual C++ Runtime、QThread、QTimer、Socket 或 OpenCV 警告。

## 下一步

在保持无 ACK、无重传的低延迟 UDP 边界前提下，按需求设计音频压缩、回声处理、呼叫控制或可靠性策略。

## Windows 便携版打包

使用 `scripts/package_windows_release.ps1` 创建 Windows x64 Release 便携包。脚本从全新的 Release 构建目录开始，执行 CTest 和 Python UDP 自测，通过 CMake install 仅安装主程序，再以 `windeployqt --release --compiler-runtime --no-translations` 部署 Qt 与 MSVC 运行库，并把实际链接的 OpenCV Release DLL 部署到程序同级目录。

在 PowerShell 中运行（路径按本机 Qt/OpenCV 安装位置调整）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_windows_release.ps1 `
    -BuildDir build-release-package-x64 `
    -QtBinDir C:\Qt\6.11.1\msvc2022_64\bin `
    -OpenCvBinDir C:\Opencv\opencv\build\x64\vc16\bin `
    -Clean
```

默认输出目录为 `dist\`：staging 目录为 `dist\video_call-win64\`，ZIP 名称为 `video_call-win64-release-<git-short-sha>.zip`。生成的 ZIP、DLL、EXE 和构建目录均为本机产物，不应提交到仓库。

打包脚本会拒绝 Debug Qt/OpenCV DLL、PDB、构建缓存和非 x64 可执行文件；Debug DLL 与 Release DLL 不能混用。`--no-translations` 不影响本程序的中文界面，因为界面中文文本编译在程序与 `.ui` 文件中，而不是依赖 Qt 翻译包。

最终 ZIP 必须在另一台未安装 Qt、OpenCV、Visual Studio、Conda 或 Python 的 Windows 11 电脑上解压验证，包括窗口与中文显示、摄像头、双向 UDP 视频、耳机双向音频、4∶3 画面和正常退出。
