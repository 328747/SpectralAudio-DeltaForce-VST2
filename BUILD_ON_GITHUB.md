# 无需安装 Visual Studio：用 GitHub Actions 构建 DLL

1. 在 GitHub 新建个人仓库，例如 `SpectralAudio-DeltaForce-VST2`。
2. 上传本软件包里的 `src/`、`CMakeLists.txt` 和 `.github/workflows/build-vst2.yml`。
3. 打开新仓库的 **Actions** → **Build Spectral Audio DeltaForce VST2 (Windows)** → **Run workflow**。
4. Windows runner 会下载原版 Spectral Audio 的 `generated/model_data.cpp` 等源码并用 MSVC 构建。
5. 只有构建成功且导出检查通过后，才从运行记录的 **Artifacts** 下载 `SpectralAudio-DeltaForce-VST2-Win64` 压缩包，内含 `SpectralAudio_DeltaForce.dll`。
6. 参照 `README_中文.md` 导入 Equalizer APO。

这属于你自己控制的构建任务，不涉及修改原作者仓库或把他的模型源文件打包到本附件中。
