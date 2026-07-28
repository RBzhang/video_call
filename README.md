# video_call

基于 Qt Widgets 和 OpenCV 的 Windows 桌面视频通话项目。目前处于“本机摄像头预览”功能的基础设施阶段：界面、OpenCV 构建配置和独立采集工作对象已完成，尚未将摄像头采集接入主窗口。

## 开发环境

- Windows 11
- Qt 6.11.1（`msvc2022_64` Kit）
- MSVC x64，C++17
- CMake
- OpenCV 4.12.0（x64 / `vc16` 预编译包）

## 当前进度

- CMake 已接入 OpenCV `core`、`imgproc`、`videoio` 组件，并完成 Debug/x64 构建验证。
- 主窗口已提供可缩放的视频预览区域、摄像头编号输入框、启动/停止按钮和独立状态标签。
- 已创建 `CameraWorker`：继承 `QObject`，适合后续通过 `moveToThread()` 迁移到专用采集线程。
- `CameraWorker` 使用 `QTimer` 驱动 OpenCV 读取，依次尝试 DirectShow、Media Foundation 和自动后端。
- 已支持 BGR、BGRA、灰度帧到 `QImage` 的安全深拷贝转换。

## 尚未实现

- MainWindow 与 CameraWorker 的信号连接和 QThread 生命周期管理
- 摄像头预览显示
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

## 下一步

创建并管理摄像头工作线程，将 `CameraWorker` 的信号连接到 MainWindow，并在 GUI 线程中完成等比例视频显示。
