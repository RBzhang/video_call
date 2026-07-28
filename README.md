# video_call

基于 Qt Widgets 和 OpenCV 的 Windows 桌面视频通话项目。当前已完成本机摄像头预览，以及后续 UDP 视频传输所需的网络参数界面和独立分片协议基础；尚未进行真实网络收发。

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
- 主窗口提供视频网络设置区域：可验证并保存对端 IPv4 地址、本地视频端口和对端视频端口；默认使用 `127.0.0.1:5000`，当前不会创建 Socket。
- 已定义 UDP 视频分片协议 V1，并实现了固定 32 字节网络字节序协议头、数据报序列化、解析和已编码帧分片。
- 已加入 CTest 协议自动测试，覆盖单分片、多分片、最大负载和常见格式错误。

## 明确尚未实现

- `QUdpSocket`
- 实际 UDP 发送和实际 UDP 接收
- JPEG 压缩与解压
- 视频帧重组缓存
- 双机视频显示
- 音频
- 录像、GStreamer 与 FFmpeg API

## UDP 视频分片协议 V1

协议层与 GUI、OpenCV、摄像头线程和 Socket 解耦。它只处理已经编码好的任意字节帧；当前项目尚未提供 JPEG 编码或网络收发。

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

协议使用 `QDataStream` 显式按字段以 Big Endian 序列化，不传输 C++ 结构体原始内存。解析会拒绝错误魔数、错误版本、截断数据报、多余字节及不一致的分片长度。

## 构建

推荐使用 Qt Creator，选择 Qt 6.11.1 MSVC x64 的 Debug Kit 后直接配置和构建。

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

## 协议测试

完成 CMake 配置和构建后，可在构建目录运行：

```bash
ctest --output-on-failure
```

`video_packet_protocol_test` 是不依赖 Widgets、Network 或 OpenCV 的独立控制台测试目标。

## 摄像头后端诊断与退出行为

摄像头打开是设备驱动调用，`cv::VideoCapture::open()` 对 Windows 的 DirectShow 和 Media Foundation 后端可能同步阻塞。OpenCV 的 `CAP_PROP_OPEN_TIMEOUT_MSEC` 只适用于 FFmpeg/GStreamer，不能用来可靠限制本机摄像头后端的打开时间。

关闭主窗口时，GUI 线程会确认自己不是摄像头线程，再以 `Qt::BlockingQueuedConnection` 在 `CameraWorker` 所在线程同步调用 `stopCamera()`。该调用会停止 `QTimer`、释放 `cv::VideoCapture`，随后在摄像头线程调用 `QThread::quit()`，并由 GUI 线程无超时地 `QThread::wait()`。仅在 `wait()` 确认工作线程结束后才删除 `QThread`；不会让摄像头线程在 `QApplication` 退出后继续运行，也不会手动删除 `CameraWorker`。

调试输出会记录停止 `CameraWorker`、`stopCamera()` 完成、开始 `quit()`、`wait()` 返回、`CameraWorker` 析构和 `QThread` 删除的顺序，可用于核对退出阶段的对象生命周期。

## 下一步

为 UDP 通信、视频编码和音频等后续功能设计独立协议与线程边界。
