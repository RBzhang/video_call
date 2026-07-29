# video_call

基于 Qt Widgets 和 OpenCV 的 Windows 桌面视频通话项目。当前已完成本机摄像头预览，以及 UDP 视频帧的真实收发、分片与重组基础；暂未接入摄像头帧编码或远端图像显示。

## 开发环境

- Windows 11
- Qt 6.11.1（`msvc2022_64` Kit）
- MSVC x64，C++17
- CMake
- OpenCV 4.12.0（x64 / `vc16` 预编译包）

## 当前已完成

- CMake 已接入 OpenCV `core`、`imgproc`、`videoio` 组件，并完成 Debug/x64 构建验证。
- 主窗口提供可缩放的视频预览区域、摄像头编号输入框、启动/停止按钮和独立状态标签。
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
- 已加入 CTest 协议、重组器、双端点真实 UDP 回环和 JPEG 编解码测试，覆盖单分片、多分片、最大负载、常见格式错误、乱序重组和 JPEG 边界标记与尺寸验证。

## 明确尚未实现

- Qt 端远端视频显示
- Qt 双向视频界面
- 音频
- ACK、丢包重传与前向纠错
- TCP、GStreamer 与 FFmpeg API
- 加密和身份认证

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

当前 CTest 包含三个独立控制台测试目标：

- `video_packet_protocol_test`：协议序列化、解析与分片。
- `video_frame_reassembler_test`：乱序、重复、冲突、超时和缓存限制重组测试。
- `video_udp_transport_test`：两个 `VideoUdpTransport` 端点使用系统分配端口进行真实 UDP 回环测试。

上述测试不依赖 Widgets 或 OpenCV。UDP 回环测试不固定占用 5000 或 5001 端口，测试完成后关闭两个 Socket。

## 同机双实例测试

可启动两个 `video_call` 实例并分别应用以下配置：

| 实例 | 对端 IP | 本地端口 | 对端端口 |
| --- | --- | ---: | ---: |
| A | `127.0.0.1` | 5000 | 5001 |
| B | `127.0.0.1` | 5001 | 5000 |

两边成功绑定后，任一实例点击“发送测试帧”，另一实例应显示收到有效的 50000 bytes 测试帧。停止网络后可再次绑定。两台真实电脑可以都使用本地端口 5000，但对端 IP 必须填写另一台电脑的局域网 IPv4 地址。

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

发送状态每秒显示实际成功提交给 `QUdpSocket::writeDatagram()` 的 JPEG payload 统计，例如实际 FPS、平均 JPEG KB/帧、JPEG payload Mbit/s 和平均分片数。该码率不包含 IP、UDP 或以太网开销；`writeDatagram()` 成功也不表示对端已收到。发送端不等待 ACK，也不检测对端是否在线。

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

## 摄像头后端诊断与退出行为

摄像头打开是设备驱动调用，`cv::VideoCapture::open()` 对 Windows 的 DirectShow 和 Media Foundation 后端可能同步阻塞。OpenCV 的 `CAP_PROP_OPEN_TIMEOUT_MSEC` 只适用于 FFmpeg/GStreamer，不能用来可靠限制本机摄像头后端的打开时间。

关闭主窗口时，GUI 线程会确认自己不是摄像头线程，再以 `Qt::BlockingQueuedConnection` 在 `CameraWorker` 所在线程同步调用 `stopCamera()`。该调用会停止 `QTimer`、释放 `cv::VideoCapture`，随后在摄像头线程调用 `QThread::quit()`，并由 GUI 线程无超时地 `QThread::wait()`。仅在 `wait()` 确认工作线程结束后才删除 `QThread`；不会让摄像头线程在 `QApplication` 退出后继续运行，也不会手动删除 `CameraWorker`。

调试输出会记录停止 `CameraWorker`、`stopCamera()` 完成、开始 `quit()`、`wait()` 返回、`CameraWorker` 析构和 `QThread` 删除的顺序，可用于核对退出阶段的对象生命周期。

## 下一步

将摄像头帧接入 JPEG 编码、远端图像解码显示，并在需要时设计音频和可靠性策略。
