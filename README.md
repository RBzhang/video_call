# video_call

基于 Qt Widgets、OpenCV、Qt Multimedia 和本地 WebRTC Audio Processing Module（APM/AEC3）的 Windows 桌面音视频传输项目。当前已完成本机摄像头预览、JPEG/UDP 视频传输、ACL1 PCM 音频传输、本地 PCM 回声消除，以及 Qt 端远端 JPEG 解码显示；它不是完整的商用视频通话系统。

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
- 主窗口提供视频网络设置区域：可选择本机 IPv4（显示网卡名称，可刷新）、验证对端 IPv4 和端口，并让视频与音频 UDP Socket 均绑定到选定的本机地址；支持停止网络和发送固定 50000 bytes 的确定性测试帧。
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
- 已接入本地 WebRTC APM/AEC3：播放侧在抖动缓冲调度后把实际将送往 `QAudioSink` 的 PCM（含静音补偿包）传给 `ProcessReverseStream()`；采集侧在原有 ACL1 发送前把麦克风 PCM 传给 `ProcessStream()`。UDP 协议、包长度、50 包/s 节奏和视频链路均未改变。
- 启动音频时严格检查默认输入和输出设备是否支持固定格式；不支持时不会隐式重采样或以不一致格式启动。
- 关闭窗口会先在 AudioWorker 所在线程停止 Source/Sink、定时器和 UDP Socket，再 `quit()`、无超时 `wait()` 并删除 `QThread`。为避免 Qt Multimedia 的退出锁阻止最后窗口关闭，程序显式禁用了 quit lock；不使用 `QThread::terminate()`。

## 明确尚未实现

- 音频压缩、Opus、AAC
- 混音和音视频同步
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

`AudioWorker` 位于独立 `QThread`，拥有 `AudioUdpTransport`、`QAudioSource`、`QAudioSink`、采集/播放 `QIODevice`、20 ms `Qt::PreciseTimer` 和 1 秒统计定时器。采集端把不规则 `readyRead()` 数据累积后按 640 bytes 切包，采集缓存上限为 6400 bytes；播放端正确处理部分写入，待写缓存最多五包。固定两行状态面板显示实际发送/接收 packets/s、payload Mbit/s、抖动深度、静音补偿、重复/迟到/外源/无效包和输入/播放溢出；输入/输出设备与实际 Source/Sink bufferSize 仅在 tooltip 中提供。界面另有“实际播放音量”区域：每约 100 ms 计算成功写入 `QAudioSink` 的 Int16 PCM RMS，显示百分比和 dBFS；它是数字样本幅度，不包含系统音量、功放或扬声器的物理声压。

### WebRTC APM/AEC3 本地 PCM 处理

APM 仅处理本机 PCM，不引入 WebRTC 的网络、媒体、编码或传输栈。网络格式仍固定为 16 kHz、单声道、Little Endian signed Int16、20 ms（320 samples / 640 bytes）。每个 20 ms 包在 `WebRtcAudioProcessor` 内按原顺序拆成两个 10 ms（160 samples / 320 bytes）帧：扬声器路径依次调用两次 `ProcessReverseStream()`，麦克风路径依次调用两次 `ProcessStream()` 后重新合并为原来的 640 bytes。

运行时 APM 配置启用 AEC3、high-pass filter 和 Moderate noise suppression，关闭 AGC1 与 AGC2。reverse stream 的处理结果不会替换扬声器 PCM；这样 APM 收到的参考始终与实际写入 `QAudioSink` 的字节完全相同。capture stream 的处理结果才会发送至现有 `sendAudioPayload()`。

初始本地流延迟为 `204 ms`，集中定义在 `WebRtcAudioProcessor::DefaultAecStreamDelayMs`。该值由当前默认 Realtek 扬声器/麦克风路径的四次独立测量（`200`、`203`、`204`、`209 ms`）取四值中位数后四舍五入获得。它表示从 render PCM 进入 APM 到其回声出现在麦克风 PCM 的本地音频路径延迟，不是网络 RTT，也不叠加抖动缓冲延迟；处理前会限制到 `0–500 ms`。状态 tooltip 会显示 APM 初始化状态、当前延迟、render/capture 失败数和旁路帧数；任何初始化或单帧失败都会保留原 PCM，不会中断通话。

音频启动后可点击“校准 AEC 延时（10 秒）”。它会在现有 render 路径生成固定的 300–3000 Hz 扫频 PCM，依旧经过 `ProcessReverseStream()` 和 `QAudioSink`；同时只采集本机麦克风 PCM，利用 1–5 秒区间的归一化相关性搜索 `0–500 ms` 声学路径延时。达到相关性阈值后，测得值会立即作为后续 `set_stream_delay_ms()` 的运行时延时；未测到足够强的测试音时维持原值。测试期间不发送、不播放采集 PCM，末尾还会保留 500 ms 采集尾音，因此它不会把声音送入 UDP 回环。单实例发往自身**同一 UDP 本地端点**的回送包会被永久丢弃，避免麦克风直通扬声器自激；判定同时检查 ACL1 `sessionId` 和数据报来源端点。因此，来自 FPGA 或其他对端的透明回送可以保留原 `sessionId` 并正常进入播放/AEC 路径，只要它来自界面配置的对端 IP 与端口。

要验证 AEC 是否真的抑制扬声器回声，点击相邻的“验证 AEC 效果（10 秒）”。它播放相同的固定扫频参考音，麦克风保持打开；每个采集 PCM 同时保留一份原始副本，并按通话时相同的 `ProcessStream()` 路径生成一份 AEC 后副本。测试结束后，界面显示与已知扬声器参考相关的回声分量由多少 dBFS 降到多少 dBFS，以及降低的 dB 值（正值越大越好）。这是真实的“扬声器 → 麦克风”本机声学验证：测试期间**不会发送或播放麦克风 PCM**，因此既保留了 AEC 所需的麦克风输入，又不会形成 UDP 自激回路。只有先检测到足够强的扬声器回声才会给出结果；否则会明确提示验证未完成，而不会把安静或耳机环境误报为 AEC 成功。

此校准测得的是本机扬声器→麦克风的物理/驱动路径延时；必须在界面显示“AEC 已启用”（而非“未配置”或“初始化失败”）时才会影响回声消除效果。程序无法替代人工判断残余回声、双讲或扬声器音量；建议校准时保持安静、用内置扬声器和麦克风、避免耳机或蓝牙设备，并至少重复两次以确认结果稳定。

### AEC3 开发、调试与验证记录

本次实现没有接入 WebRTC 的通话网络栈，只使用其 Audio Processing Module：实际准备写入 `QAudioSink` 的远端 PCM 先进入 `ProcessReverseStream()`，本机采集 PCM 在发送 ACL1 前进入 `ProcessStream()`。这样 AEC3 的参考与扬声器实际播放的数据一致，网络协议仍是固定 16 kHz、单声道、20 ms / 640-byte 的 ACL1 包。

为避免把“能听见声音”误判成“已经消除回声”，界面提供两个互补的十秒诊断：

- “校准 AEC 延时”播放固定扫频并测量扬声器→麦克风声学路径；测得延时仅在当前音频启动期间生效，重新启动音频会恢复默认 `204 ms`。校准与网络地址、FPGA 和 UDP 回送无关，**不需要也不应先切换至 `127.0.0.1`**。
- “验证 AEC 效果”保留同一段真实麦克风 PCM 的 AEC 前/后副本，量化与已知扬声器参考相关的回声分量；测试期间不发送或播放麦克风 PCM。一次默认 Realtek 扬声器/麦克风实测得到 `208 ms`，相关回声从 `-44.1 dBFS` 降至 `-89.8 dBFS`，降低 `45.8 dB`，相关性从 `0.1673` 降至 `0.0070`。数值会随设备、音量和环境变化。

调试中发现两类容易混淆的“回环”：单实例 `127.0.0.1:5002` 软件回环会把本机麦克风直接送回本机扬声器，必然产生正反馈，因此必须丢弃；而 PC → FPGA → PC 的透明 UDP 回送虽然保留同一 ACL1 `sessionId`，但数据报来源是 FPGA 的配置端点（例如 `192.168.10.10:5002`），应进入播放和 AEC。此前仅凭相同 `sessionId` 丢弃，导致 FPGA 回送音频被误判为软件自回环；现已修复为同时匹配本机绑定的 UDP 来源 IP 与端口才丢弃。视频没有该 `sessionId` 接收过滤，所以该问题表现为“视频正常、音频无接收”。

FPGA 实物回送的推荐顺序是：选择实际连接 FPGA 的本机网卡/IP，配置 FPGA IP 与音频端口 `5002`，应用并启动音频，随后直接执行本机 AEC 校准，最后进行 FPGA 回送测试。回送包需从界面配置的 FPGA IP/端口返回，并满足 ACL1 的固定 `672-byte` 数据报格式。修复后的闭环会真实播放回送音频；请先降低扬声器音量，因为即使 AEC 已启用，过高音量或不稳定的声学环境仍可能产生物理自激。

验证覆盖固定 PCM 协议、抖动缓冲、真实 UDP 双端点、APM 20 ms/10 ms 子帧处理、声学延时估算、AEC 回声分量量化和 GUI 退出。新增 `audio_loop_policy_test` 明确验证：相同本地 UDP 端点会被识别为软件自回环，而 `192.168.10.10:5002` 这类 FPGA 对端不会被误丢弃。Debug 与 Release 均执行 CTest，当前 `13/13` 测试通过；物理 AEC 效果数值由上述十秒诊断在真实默认音频设备上取得。

点击“应用音频设置”时复用现有“对端 IP”和“本机 IP”选择，并验证 IPv4；成功绑定后才可以点击“开始双向音频”。“本机 IP”会列出已启用网卡的 IPv4 及网卡名称，`127.0.0.1` 仅用于同机测试；需要经 Wi-Fi 或以太网通信时，应选择对应网卡的 IPv4。点击“刷新 IP”可重新扫描网卡。视频和音频 Socket 都绑定到该选择，因此发送数据报会使用指定 IP 作为源地址。启动时仅使用 `QMediaDevices` 的默认输入和输出，且两者都必须支持 16 kHz / 单声道 / Int16；不支持会显示设备描述及 preferred format，且不会启动或回退为其他网络格式。停止音频只停止音频，不停止摄像头、视频 UDP、视频编码或远端视频显示；重新应用音频设置会停止当前音频、清空抖动缓冲并重新绑定。

同机双实例音频测试应使用交叉端口：

| 实例 | 对端 IP | 本地音频端口 | 对端音频端口 |
| --- | --- | ---: | ---: |
| A | `127.0.0.1` | 5002 | 5003 |
| B | `127.0.0.1` | 5003 | 5002 |

两台电脑测试时，两边都使用本地/对端 `5002`，对端 IP 填另一台电脑的局域网 IPv4，并允许 Windows 防火墙的 UDP 入站访问。建议先使用笔记本内置麦克风和内置扬声器验证 AEC；蓝牙扬声器、不同声卡时钟、麦克风削波、扬声器音效和过大音量都会降低效果。耳机模式通常不需要 AEC，但启用 AEC 不应导致程序异常。音频和视频的端口、Socket、来源过滤、线程和状态均相互独立；协议不检测对端在线、不发送 ACK、不重传、不做 FEC。

## 构建

推荐使用 Qt Creator，选择 Qt 6.11.1 MSVC x64 的 Debug Kit 后直接配置和构建。

修改 `QObject` 派生类的成员后，建议重新运行 CMake 并执行一次全量构建，避免复用旧的目标文件或自动生成文件。

### Windows Debug 退出崩溃：必须干净重建的情形

本项目的 `MainWindow`、Camera/Audio Worker 和远端解码器均是跨线程对象；修改了这类 `QObject` 派生类的成员布局后，不能继续信任曾经部分构建失败或中断的 build 目录。曾复现过旧的 `main.cpp.obj` 按旧 `MainWindow` 大小在栈上创建对象、而新的构造函数按更大的布局写入成员的情况。最先出现的是 MSVC `/RTC1` 的“`w` 周围的栈已损坏”，随后才可能在关闭时表现为 Debug Heap 的 `_CrtIsValidHeapPointer(block)` 断言；这不是应通过 Release、`exit()` 或关闭断言规避的 CRT 问题。

如果遇到上述两种错误，关闭 Qt Creator 和程序，**删除该 Kit 对应的旧 build 配置目录后重新配置 CMake**，再进行完整构建。例如：

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\debug-clean -DOpenCV_DIR=C:\Opencv\opencv\build\x64\vc16\lib
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\debug-clean --config Debug
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\debug-clean -C Debug --output-on-failure
```

之后应从这个新目录启动程序。正常的小改动并不要求删除所有 build 目录；这里的要求仅针对类布局变更、构建中断，或已经出现栈/堆损坏诊断的配置目录。

项目在未由外部指定时使用以下 OpenCV CMake 配置目录：

```text
C:/Opencv/opencv/build/x64/vc16/lib
```

也可以在 CMake 配置时显式指定：

```text
-DOpenCV_DIR=C:/Opencv/opencv/build/x64/vc16/lib
```

要启用 AEC3，请提供本机已经构建好的**当前 WebRTC APM** target 或安装/源码根目录；工程不会下载、编译或 vendor WebRTC：

```powershell
# 推荐：由上层工程/包提供完整依赖闭包的 target。
-DVIDEO_CALL_WEBRTC_APM_TARGET=WebRTC::webrtc

# 或：根目录内可找到 api/audio/audio_processing.h 和 webrtc.lib。
-DVIDEO_CALL_WEBRTC_APM_ROOT=C:/dev/webrtc-checkout/src
```

当前接入使用 WebRTC `api/audio/audio_processing.h`、`BuiltinAudioProcessingBuilder(config).Build(CreateEnvironment())`、`ProcessReverseStream()`、`set_stream_delay_ms()` 和 `ProcessStream()`。未配置本机 APM 时，CMake 会明确报告 APM disabled，程序仍可构建运行并以安全旁路发送原 PCM；配置有效的 target 后会自动定义 `VIDEO_CALL_HAVE_WEBRTC_APM` 并链接该 target。

对 Windows + Qt MSVC，WebRTC 库的 C Runtime 必须和应用一致：Release 使用 `/MD`，Debug 使用 `/MDd`，且应以 `use_custom_libcxx=false` 构建。当前工程按 `CMAKE_BUILD_TYPE` 自动搜索 `out/Release/obj/webrtc.lib` 或 `out/Debug/obj/webrtc.lib`，不会把 Release 库链接给 Debug 程序。若以源码 checkout 直接构建静态 `webrtc.lib`，需在 `build/config/win/BUILD.gn` 的桌面 Windows 默认配置中将 `:static_crt` 改为 `:dynamic_crt`，然后分别生成并构建两个输出目录：

```powershell
# 在 C:/dev/webrtc-checkout/src 中执行；两种配置都保留 dynamic_crt 改动。
gn gen out/Release --args='is_debug=false target_cpu="x64" is_component_build=false rtc_include_tests=false rtc_build_examples=false rtc_build_tools=false use_siso=false use_custom_libcxx=false'
ninja -C out/Release webrtc

gn gen out/Debug --args='is_debug=true target_cpu="x64" is_component_build=false rtc_include_tests=false rtc_build_examples=false rtc_build_tools=false use_siso=false use_custom_libcxx=false enable_iterator_debugging=true'
ninja -C out/Debug webrtc
```

`enable_iterator_debugging=true` 仅为 Debug 输出所需：它使 WebRTC 的 `_ITERATOR_DEBUG_LEVEL` 与 Qt/MSVC Debug 保持一致。`gclient sync` 可能覆盖该 `BUILD.gn` 的本地 `dynamic_crt` 调整；同步后先重新应用它再执行 `gn gen`。Qt Creator 的 Debug Kit 应配置 `VIDEO_CALL_WEBRTC_APM_ROOT=C:/dev/webrtc-checkout/src` 并在 CMake 输出中确认 `WebRTC APM enabled`；Release Kit 同样使用此根目录即可。

本地 `out/Debug/obj/webrtc.lib` 不带 AddressSanitizer instrumentation，因此在 Qt Creator 的 AEC3 Debug 配置中必须使用 `VIDEO_CALL_ENABLE_ASAN=OFF`。若同时开启 ASan，MSVC STL 的容器注释与 WebRTC 库不一致，会产生大量 `LNK2038`；CMake 会在配置阶段直接提示。只有提供一个同样以 ASan 构建、完整依赖闭包的 `VIDEO_CALL_WEBRTC_APM_TARGET` 时，才可以启用该选项。

## 运行时 DLL

配置阶段会检查 `opencv_core`、`opencv_imgcodecs`、`opencv_imgproc` 和 `opencv_videoio` 导入目标是否同时提供 Debug/Release 的导入库和 DLL；缺少任一配置会直接报错。`video_call` 的构建后步骤会把**当前配置实际链接**的 Qt Core/Gui/Widgets/Network/Multimedia、OpenCV DLL 以及 Windows 平台插件复制到可执行文件目录：Debug 为 `Qt6*Cored.dll`/`opencv_*d.dll` 和 `platforms/qwindowsd.dll`，Release 为不带 `d` 后缀的对应文件和 `platforms/qwindows.dll`。因此 Qt Creator 可以直接运行该目录下的程序，不应依赖 `PATH` 恰好指向另一套 Qt 或 OpenCV。

MSVC 目标显式使用动态运行库：Debug 为 `/MDd`，Release 为 `/MD`。不要把 Debug 可执行文件与 Release OpenCV/Qt DLL 混用，反之亦然。该仓库不会提交 `build` 等生成目录或编译产物。

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

当前 CTest 包含十二个测试目标：

- `video_packet_protocol_test`：协议序列化、解析与分片。
- `video_frame_reassembler_test`：乱序、重复、冲突、超时和缓存限制重组测试。
- `video_udp_transport_test`：两个 `VideoUdpTransport` 端点使用系统分配端口进行真实 UDP 回环测试。
- `jpeg_frame_encoder_test`：JPEG 编码与解码边界和尺寸验证。
- `jpeg_frame_decoder_test`：确定性 JPEG 解码、灰度 JPEG、深拷贝，以及空输入、随机字节、截断、缺少 EOI 和超过 4 MiB 输入拒绝。
- `audio_packet_protocol_test`：ACL1 固定头 Big Endian 序列化、严格解析和全部固定字段拒绝测试。
- `audio_jitter_buffer_test`：三包预缓冲、乱序、重复、丢包静音、五包重缓冲、十包上限、session 重置和 sequence 回绕测试。
- `audio_udp_transport_test`：两个 `AudioUdpTransport` 端点用系统分配端口进行双向、50 包连续、外源过滤、错误数据报和重新绑定测试。
- `audio_loop_policy_test`：仅拦截同一本机 UDP 端点的软件自回环，允许 FPGA 等对端保留 ACL1 sessionId 的透明回送。
- `webrtc_audio_processor_test`：20 ms/10 ms 拆分与重组、子帧顺序、错误长度旁路，以及 initialize/reset/shutdown 生命周期；不依赖物理音频设备。
- `acoustic_delay_estimator_test`：已知声学副本延时的相关性估算、静音拒绝，以及 AEC 前后相关回声分量的量化。
- `video_frame_rate_utils_test`：1–30 FPS 的毫秒区间计算，以及非法 FPS 拒绝。
- `mainwindow_exit_smoke_test`：创建 `QApplication`/`MainWindow` 后自动关闭；无硬件时至少连续三次覆盖启动即退出和 UDP 配置即退出，有可用设备时继续覆盖摄像头、视频发送、音频和摄像头重启。它检查每个 worker 只析构一次，且没有 `QThread: Destroyed while thread is still running`。

协议、重组器、UDP 回环和帧率工具测试不依赖 Qt Widgets；视频/音频 UDP 回环测试依赖 Qt Network；JPEG 编码和解码测试依赖 Qt Core/Gui 与 OpenCV。所有 CTest 都是无交互控制台测试，不需要显示 GUI 窗口。UDP 回环测试使用系统分配端口，不固定占用 5000、5001、5002 或 5003，测试完成后关闭 Socket。

AddressSanitizer 可用独立目录启用：

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build\asan-clean -DVIDEO_CALL_ENABLE_ASAN=ON -DOpenCV_DIR=C:\Opencv\opencv\build\x64\vc16\lib
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build\asan-clean --config Debug
C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir build\asan-clean -C Debug --output-on-failure
```

该选项在 MSVC 下启用 `/fsanitize=address`、`/Zi` 和 `/INCREMENTAL:NO`，并移除与 ASan 不兼容的 `/RTC1`/`/ZI`。`udp_test_receiver.py` 只需要 Python 标准库；`udp_jpeg_receiver.py --self-test` 需要安装 Python OpenCV（`cv2`）。

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

两台真实电脑可以都使用本地端口 5000，但对端 IP 必须填写另一台电脑的局域网 IPv4 地址；每台电脑还应在“本机 IP”选择实际通向对端的 Wi-Fi 或以太网 IPv4。两边应用网络设置、各自启动摄像头并开始发送视频后，左侧是本机画面、右侧是对端画面。Windows 防火墙必须允许对应的 UDP 入站端口。本协议不发送 ACK、不重传，也不检测对端在线状态；关闭一端时另一端不会得到“离线”通知。

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

## FPGA UDP 回环视频丢包修复报告

### 现象

摄像头发送端设置为 10 FPS，经 FPGA UDP 回环后，接收端只能完成约 1 FPS 的 JPEG 重组。这里的“完成”是指一张 JPEG 的所有 VCL1 分片都已收到；并不是某个 UDP 包收到即可显示。因此，即使只丢失一片，接收端也会在重组超时后丢弃整张视频帧，帧率会被放大式地降低。

### 原因分析

VCL1 每个 UDP 数据报最大 1200 bytes，其中固定协议头占 32 bytes，实际 JPEG 数据最多为 1168 bytes。一张 50,000-byte JPEG 需 `ceil(50000 / 1168) = 43` 个 UDP 分片。原实现对一帧 JPEG 调用 `fragmentEncodedFrame()` 后，在同一个 Qt 事件循环中连续调用 43 次 `QUdpSocket::writeDatagram()`；操作系统会把这些数据报以网卡可用的最高瞬时速率发出，而不是按 10 FPS 均匀分布。

FPGA 的以太网回环通路是流式处理：TEMAC RX 没有接入可反馈给上位机的 `tready`/反压信号，接收侧只能依赖有限的 RX FIFO、UDP payload FIFO 和已完成 UDP 包的描述符 FIFO 吸收突发。当前描述符 FIFO 的深度为 16 包。即使平均视频码率远低于链路能力，几十个分片在极短时间内到达仍可能使这些有限队列积压；队列不足时，TEMAC 无法要求发送端暂停，结果只能丢弃后续数据。

全双工只保证 RX 和 TX 电气链路可同时工作，并不消除同一方向上“输入突发速度大于本地队列可吸收速度”的问题。`writeDatagram()` 返回成功也仅表示数据已交给本机 UDP/IP 栈，不表示 FPGA 已收下，更不表示回环端已经完成视频帧重组。

`iladatarxaxi.csv` 中 `rx_fifo_full` 未置位，不能据此排除该问题：导出文件中的 `rx_state[0:0]` 和 `gmii_rxd[3:0]` 分别只有 1 bit、4 bit，而当前 RTL 中相应探针应为 2 bit、8 bit。这说明该 CSV 对应的 ILA/LTX 探针定义已经过期或未重新生成；同时它只截取到一段短帧，并未覆盖摄像头的连续分片突发。因此该采样仅能说明“该窗口内 RX FIFO 未满”，不能证明视频传输期间所有缓存都未溢出。

### 修复方案

发送端已在 `VideoUdpTransport` 中加入逐分片的线速整形：

- 限速常量为 `FpgaLoopbackWireRateMbitPerSecond = 80.0`，留出充足余量给 FPGA 的跨时钟、协议解析和发送调度。
- 间隔按实际线上占用计算：前导码/SFD 8 bytes、以太网头 14 bytes、IPv4 头 20 bytes、UDP 头 8 bytes、VCL1 数据报、FCS 4 bytes 和 IFG 12 bytes。
- 每次发送前使用单调时钟等待到下一数据报截止时刻；若编码、GUI 调度或系统调度已落后，则从当前时刻重新计时，**不追赶**之前错过的时隙，避免形成补发微突发。
- 在重新绑定本地 Socket、重新配置对端和关闭传输时重置节流时钟，避免上一次会话的截止时间影响下一次会话。

以满载 VCL1 数据报为例，线上占用为 `8 + 14 + 20 + 8 + 1200 + 4 + 12 = 1266 bytes`。在 80 Mbit/s 限制下，两个此类分片至少间隔约 126.6 µs；上述 50,000-byte JPEG 的 43 个分片约占用 5.44 ms，仍显著小于 10 FPS 对应的 100 ms 帧周期。因此，这个节流限制的是**帧内突发速率**，而不是把配置的发送帧率固定降为 1 FPS。

实现位置：

- `videoudptransport.cpp`：`datagramWireTimeNs()` 计算线上时间，`paceDatagram()` 实施整形。
- `videoudptransport.h`：保存单调节流时钟和下一分片截止时间。

### 适用范围与限制

此修复解决的是上位机向 FPGA 连续灌入分片造成的微突发问题，不把 UDP 变为可靠传输：协议仍没有 ACK、重传、拥塞反馈或逐帧确认。如果 JPEG 平均码率本身持续超过 80 Mbit/s，发送过程会跨越多个帧周期，应用仍应降低分辨率、JPEG 质量或发送帧率。

FPGA 侧仍应保留缓存保护和正确的 ILA 诊断。重新采集时必须先重新生成 ILA IP、实现 bitstream 和对应的 `.ltx`，保证探针宽度与 RTL 一致；建议触发条件覆盖连续视频分片，并同时观察 RX FIFO 满、payload FIFO 接近满、描述符 FIFO 满/丢包计数和 UDP `rec_pkt_done`。仅观察 `rx_fifo_full` 不能定位全部缓冲路径。

### 验证结果与板卡复测步骤

已在本机重新构建 `video_call` 和 `video_udp_transport_test`。后者覆盖 100-byte 单分片、50,000-byte 多分片双向传输及连续五帧多分片传输，运行退出码为 0。该测试验证 VCL1 分片、节流和本机 UDP 重组逻辑；它不替代 FPGA 实物链路测试。

板卡复测应按以下方式进行：关闭所有旧的 `video_call.exe` 进程，启动重新构建的程序；保持视频端口为 FPGA 监听端口（例如 5000）；选择实际连接 FPGA 网口的本机 IPv4；以 10 FPS 连续发送至少 30 秒。发送端状态中的实际 FPS 应接近目标值，回环接收端的完成/显示 FPS 应接近发送端，且不应持续累积 VCL1 重组超时。若仍有明显帧级丢失，应使用已重新生成探针定义的 ILA 捕获连续分片，再依据上述四类队列/完成信号定位 FPGA 内部瓶颈。

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

`MainWindow::shutdownAll()` 是唯一的幂等退出入口：`closeEvent()` 调用它一次，析构函数只在尚未关闭时兜底。它一开始就设置关闭标志、停止 GUI 定时器并禁止新任务，断开跨线程连接、关闭 GUI 线程的 UDP transport；然后以 `Qt::BlockingQueuedConnection` 在各 worker 自己的线程内执行 `shutdown()`，再 `quit()`、`wait()`，最后才删除已停止的 `QThread`。worker 仍由 `QThread::finished -> QObject::deleteLater` 销毁，GUI 线程绝不手工删除 worker；`QPointer` 防止结束阶段继续访问悬空 worker。

摄像头关闭会在 `CameraWorker` 线程中停止 `QTimer`、释放 `cv::VideoCapture` 和帧缓存。音频关闭会在 `AudioWorker` 线程中停止 `QAudioSource`/`QAudioSink`、I/O 设备、定时器和 UDP transport；带有 `this` 父对象的普通 QObject 只由父子所有权销毁，不与手工 `delete`/`deleteLater` 混用。远端 JPEG 解码器同样先停止接收新任务、在线程内 shutdown，再等待线程结束。这一顺序避免 queued signal、定时器或 `readyRead` 在关闭期访问已销毁的窗口、UI 或 worker。

在全新 Debug/x64 构建中，应覆盖以下退出场景：未启动摄像头/UDP、仅绑定 UDP、摄像头运行、摄像头已停止、JPEG 发送、JPEG 接收解码，以及发送与接收同时进行。每次均应在 2 秒内以 exit code 0 结束，且没有 `video_call.exe` 残留、Visual C++ Runtime、QThread、QTimer、Socket 或 OpenCV 警告。也可以使用 `video_call --shutdown-scenario=idle|udp|camera|camera-video|audio|audio-video|camera-restart` 对单个场景进行自动关闭验证。

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
