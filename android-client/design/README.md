# Remoe App Icon

图标把远程桌面窗口与鼠标指针合并为一个符号。深色背景沿用 App 界面，薄荷绿代表连接状态，
高对比度白色指针呼应 Android 客户端的触控板操作方式。

- `app-icon-master.png`：图像生成母版，用于保留最初概念；
- `app-icon.svg`：可编辑的干净矢量源稿；
- Android 实际使用 `res/drawable/ic_launcher_foreground.xml` 和 adaptive icon 资源；
- Android 13 及以上支持 themed icon 单色图层。

核心图形保持在自适应图标安全区域内，不依赖固定圆形或圆角方形遮罩。
