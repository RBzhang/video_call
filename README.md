# video_call

基于 Qt Widgets 和 OpenCV 的 Windows 桌面视频通话项目。目前完成了本机摄像头预览：可选择设备编号、启动/停止采集，并将画面实时等比例显示在主窗口中。

## 开发环境

- Windows 11
- Qt 6.11.1（`msvc2022_64` Kit）
- MSVC x64，C++17
- CMake
- OpenCV 4.12.0（x64 / `vc16` 预编译包）

## 当前进度

- CMake 已接入 OpenCV `core`、`imgproc`、`videoio` 组件，并完成 Debug/x64 构建验证。
- 主窗口提供可缩放的视频预览区域、摄像头编号输入框、启动/停止按钮和独立状态标签。
- `CameraWorker` 已通过 `moveToThread()` 运行在专用摄像头线程；GUI 线程只负责界面和 `QPixmap` 创建。
- `CameraWorker` 使用 `QTimer` 驱动 OpenCV 读取，依次尝试 DirectShow、Media Foundation 和自动后端。
- 已支持 BGR、BGRA、灰度帧到 `QImage` 的安全深拷贝转换。
- 已实现启动、停止、重复启动、连续读取失败处理，以及关闭主窗口时的线程收尾。
- 打开摄像头时会在状态标签和调试输出中依次报告 DSHOW、MSMF、ANY 三个后端的尝试、结果、OpenCV 实际后端和耗时，便于定位权限、设备占用或后端问题。

## 尚未实现

- UDP 通信、音频、视频编码、录像、GStreamer 与 FFmpeg API

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

## 摄像头后端诊断与退出行为

摄像头打开是设备驱动调用，`cv::VideoCapture::open()` 对 Windows 的 DirectShow 和 Media Foundation 后端可能同步阻塞。OpenCV 的 `CAP_PROP_OPEN_TIMEOUT_MSEC` 只适用于 FFmpeg/GStreamer，不能用来可靠限制本机摄像头后端的打开时间。

因此程序会先异步请求停止并退出摄像头线程，最多等待 500 ms。通常情况下，工作线程会在该时间内停止定时器、释放摄像头并退出；若底层驱动仍卡在 `open()` 或读取调用中，主窗口不会无限等待，也不会销毁仍在运行的 `QThread`。该线程会在驱动调用返回后完成排队的释放和自销毁；若用户已退出整个程序，Windows 会回收进程资源。

## 下一步

为 UDP 通信、视频编码和音频等后续功能设计独立协议与线程边界。
