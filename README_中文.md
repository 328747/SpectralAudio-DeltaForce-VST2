# SpectralAudio-DeltaForce：Equalizer APO 的 VST2 插件封装（实验版）

## 版本和完成程度

- 实现了标准 VST2 处理接口，输出目标为 Windows x64 `.dll`。
- 使用 [white-give-man/spectral-audio](https://github.com/white-give-man/spectral-audio) 的原版模型（game=2, level=4）以及 `spectral_stream_process`，不改动模型参数。
- 不依赖原版 Spectral Audio 的 APO 安装或注册流程；不会改系统 APO 注册表。
- 代码已在 Linux 运行**模拟核心**的宿主测试：ABI、左右声道、任意大小的宿主缓冲区、1024 帧延迟对齐、旁路和 44.1 kHz 安全降级均通过。
- **重要：本软件包没有预编译的 Windows DLL，尚未在 Windows + 原版模型上进行真实编译或游戏实测。** 不要把 `tests/mock_spectral.cpp` 当作 AI 引擎。

## 一键构建（Windows 11 x64）

1. 安装 `Visual Studio 2022 Build Tools`，勾选 **使用 C++ 的桌面开发**（MSVC x64 和 Windows SDK）；安装 [CMake](https://cmake.org/download/) 并加入 PATH。建议安装 Git（可选；不安装时通过 PowerShell 下载源代码压缩包）。
2. 解压本项目到一个没有中文和空格的路径，例如 `C:\Audio\SpectralAudio-DeltaForce-VST2`。
3. 双击 `BUILD_PLUGIN.cmd`。脚本从作者 GitHub 仓库获取原版代码和编译进 C++ 的模型权重，编译我们的 VST2 封装。
4. 成功后输出：`dist\SpectralAudio_DeltaForce.dll`。只有这个是真正可以导入 Equalizer APO 的 Windows 插件。无需复制原版 EXE，也不要运行作者的 APO 安装程序。

没有安装 VS 2022 的替代方案：将此工程上传至**自己的 GitHub 仓库**，开启 Actions，运行 `Build Spectral Audio DeltaForce VST2 (Windows)` 工作流并从 Artifacts 下载 DLL。此方案依赖 GitHub Actions 的权限和实际构建结果。

## Equalizer APO 导入方法

1. **先备份** Equalizer APO 的 `config.txt` 和 PUBG 配置。避免同时运行原版 Spectral Audio 的 Windows APO 会话。
2. 将编译好的 DLL 复制到稳定、音频服务可读的目录（例如管理员创建的 `C:\Program Files\EqualizerAPO\config\VSTPlugins\`）。不要直接从临时下载目录加载。
3. 打开 Equalizer APO **Configuration Editor**，新建三角洲配置文件（例如 `DeltaForce.txt`）。
4. 添加 **Plugin → VST plugin** 效果，浏览加载 `SpectralAudio_DeltaForce.dll`；检查名称为 `SpectralAudio DeltaForce`。
5. 设备格式设为 **48,000 Hz、立体声**，启用该播放设备上的 Equalizer APO。
6. 初次测试使用 `AI Strength 100%`、`Wet Mix 100%`、`Output Trim 0 dB`、`Bypass Off`。建议 Equalizer APO 前级增益先设 `-3 dB`，检查是否有削波；如果担心风声过强，可将强度试降至 75%，再判断是否加入 ReaComp、LoudMax。
7. 单独用 Equalizer APO 处理游戏输出。若声音仍通过 SteelSeries Sonar，先关闭其重叠 EQ 和动态处理；先确认实际音频路由。
8. 试听左右方向、远近脚步、风声、枪声，并比较打开/关闭插件的差异。旁路仍保留 1024 帧处理链延迟，用于避免切换时序错位。

## VST 参数

| 参数 | VST 原始归一化值 | 对应实际值 | 默认 |
|---|---|---|---|
| AI Strength | 0…1 | 0…200% | 100% |
| Wet Mix | 0…1 | 干湿混合 0…100% | 100% |
| Output Trim | 0…1 | -12…+6 dB | 0 dB |
| Bypass | 0…1 | 关/开（>=0.5 时开） | 关 |

注意：**AI Strength 0% 不等于绕过 AI 算法**，因为模型仍保留衰减。需要纯原音比较时用 `Bypass On` 或 `Wet Mix 0%`。

## 音频时序与安全设计

- 原算法需要每次调用处理 **512 帧交错立体声**；VST 宿主提供的是任意长度、非交错的双声道缓冲。
- 封装先收集 512 帧并调用原版 C 接口，再通过下一段宿主音频输出，因此相对输入**总共对齐到 1024 帧**（21.33 ms @ 48 kHz）。
- 插件保留一条独立的 1024 帧纯音延迟线，以便与 AI 输出对齐混音、旁路；不会在实时处理回调中分配动态内存。
- 强度变化由原算法按块平滑；干湿/旁路使用插件级短暂平滑。
- 44.1 kHz 等不支持的采样率自动绕过 AI 计算，输出延迟后的原音，**不是**自动重采样。
- 输入采样被限制在 [-1,1]（原版核心的约束）；输出做有限值保护与 [-1,1] 限幅；不要依赖它代替完整峰值限制器。
- **没有自定义插件编辑器 GUI**；通过 Equalizer APO 的通用 VST 参数编辑界面调整控制项。
- 这不是按游戏进程识别的插件；Equalizer APO 对挂载输出设备上的音频统一生效。需要手动切换不同游戏配置。

## 构建验证（开发者）

Linux 下可对纯 VST 封装运行模拟核心测试：

```bash
cmake -S . -B build-test -DSPECTRAL_MOCK=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-test -j4
ctest --test-dir build-test --output-on-failure
```

**模拟测试不是实际 Windows DLL 的音质验证。** Windows 构建脚本还会执行一个真实原版模型的 VST 宿主冒烟测试，检查动态库导出、旁路延迟对齐、非静音/有限值输出；**Equalizer APO 实际加载及声卡游戏效果仍需用户在 Windows 上验证**。

## 来源与授权提示

封装源码为本项目独立实现，不附带原作者模型、核心 C++ 源码或编译后的模型文件。构建时直接从原作者公共仓库获取对应资源。请遵守原仓库许可条款；公开仓库并不自动等于允许随意再分发模型及二进制。VST2 兼容层只使用必要的二进制 ABI 定义，不捆绑 Steinberg SDK。

本项目为社区实验性适配，不是 white-give-man 原作者或 Equalizer APO 官方提供的软件。使用前备份配置，第一次测试优先选非关键音频设备。
