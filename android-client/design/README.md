# Remoe App Icon

图标把远程桌面窗口与鼠标指针合并为一个符号。深色背景沿用 App 界面，薄荷绿代表连接状态，
高对比度白色指针呼应 Android 客户端的触控板操作方式。

- `app-icon-master.png`：图像生成母版，用于保留最初概念；
- `app-icon.svg`：可编辑的干净矢量源稿，也是所有平台的规范源文件；
- Android 实际使用 `res/drawable/ic_launcher_foreground.xml` 和 adaptive icon 资源；
- Android 13 及以上支持 themed icon 单色图层。

运行 `tools/generate-brand-icons.ps1` 可从矢量源稿生成 Web favicon、主屏幕图标和 Windows
多尺寸 ICO；生成环境需要 Chrome/Edge，以及 PATH 中的 FFmpeg。不要单独修改生成产物，以免
各平台视觉不一致。

核心图形保持在自适应图标安全区域内，不依赖固定圆形或圆角方形遮罩。
