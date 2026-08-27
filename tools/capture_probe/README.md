# remoe capture probe

这是一个不依赖 remoe、oneVPL 或 NVENC 的 Windows 桌面捕捉兼容性探针。它会：

1. 使用 GDI `BitBlt` 捕捉整个虚拟桌面；
2. 枚举 DXGI adapter/output；
3. 在指定 output 上依次测试 D3D11 device、`DuplicateOutput`、
   `AcquireNextFrame` 和 CPU readback；
4. 将成功捕捉的画面保存为 BMP，方便检查黑屏、旧画面或异常颜色。

## 构建

在安装 Visual Studio 2022 C++ 工具和 Windows SDK 的机器上运行：

```powershell
cmake -S tools/capture_probe -B build-capture-probe -A x64
cmake --build build-capture-probe --config Release
```

只需要把以下文件复制到服务器，不需要额外 DLL：

```text
build-capture-probe/Release/remoe_capture_probe.exe
```

## 运行

在服务器的交互式桌面会话中运行，不要从 Windows 服务或已断开的 RDP 会话运行：

```powershell
.\remoe_capture_probe.exe
```

默认测试全局 DXGI output 0，等待画面的最长时间为 3000 ms。其他用法：

```powershell
.\remoe_capture_probe.exe --list
.\remoe_capture_probe.exe --output 1 --timeout 5000
```

输出目录中会生成：

- `gdi_virtual_desktop.bmp`
- `dxgi_output_N.bmp`（仅 DXGI 成功时）

请把控制台完整输出以及两张 BMP 的实际显示结果一起记录。API 返回成功但 BMP
为全黑或内容不更新，仍应视为该捕捉路径不可用。

