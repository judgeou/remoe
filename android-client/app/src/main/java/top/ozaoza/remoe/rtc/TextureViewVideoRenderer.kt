package top.ozaoza.remoe.rtc

import android.content.Context
import android.graphics.Point
import android.graphics.SurfaceTexture
import android.util.AttributeSet
import android.view.TextureView
import org.webrtc.EglBase
import org.webrtc.EglRenderer
import org.webrtc.GlRectDrawer
import org.webrtc.RendererCommon
import org.webrtc.ThreadUtils
import org.webrtc.VideoFrame
import org.webrtc.VideoSink
import java.util.concurrent.CountDownLatch

/** WebRTC EGL renderer backed by TextureView for devices with broken SurfaceView composition. */
class TextureViewVideoRenderer @JvmOverloads constructor(
    context: Context,
    attributes: AttributeSet? = null,
) : TextureView(context, attributes), TextureView.SurfaceTextureListener, VideoSink {
    private val eglRenderer = EglRenderer("TextureViewVideoRenderer")
    private val videoLayoutMeasure = RendererCommon.VideoLayoutMeasure()
    private var rendererEvents: RendererCommon.RendererEvents? = null
    private var initialized = false
    private var firstFrameRendered = false
    private var frameWidth = 0
    private var frameHeight = 0
    private var frameRotation = 0

    init {
        surfaceTextureListener = this
        isOpaque = true
    }

    fun init(sharedContext: EglBase.Context, events: RendererCommon.RendererEvents) {
        check(!initialized) { "TextureViewVideoRenderer is already initialized" }
        rendererEvents = events
        eglRenderer.init(sharedContext, EglBase.CONFIG_PLAIN, GlRectDrawer())
        eglRenderer.addRenderListener {
            if (!firstFrameRendered) {
                firstFrameRendered = true
                rendererEvents?.onFirstFrameRendered()
            }
        }
        initialized = true
        if (isAvailable) surfaceTexture?.let(eglRenderer::createEglSurface)
        updateLayoutAspectRatio(width, height)
    }

    fun setMirror(mirror: Boolean) = eglRenderer.setMirror(mirror)

    fun setScalingType(scalingType: RendererCommon.ScalingType) {
        videoLayoutMeasure.setScalingType(scalingType)
        requestLayout()
    }

    fun clearImage() = eglRenderer.clearImage()

    fun release() {
        initialized = false
        rendererEvents = null
        eglRenderer.release()
    }

    override fun onFrame(frame: VideoFrame) {
        val width = frame.rotatedWidth
        val height = frame.rotatedHeight
        if (width != frameWidth || height != frameHeight || frame.rotation != frameRotation) {
            frameWidth = width
            frameHeight = height
            frameRotation = frame.rotation
            rendererEvents?.onFrameResolutionChanged(width, height, frame.rotation)
            post { requestLayout() }
        }
        eglRenderer.onFrame(frame)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val size: Point = videoLayoutMeasure.measure(
            widthMeasureSpec,
            heightMeasureSpec,
            frameWidth,
            frameHeight,
        )
        setMeasuredDimension(size.x, size.y)
    }

    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
        if (initialized) eglRenderer.createEglSurface(texture)
        updateLayoutAspectRatio(width, height)
    }

    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
        updateLayoutAspectRatio(width, height)
    }

    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
        if (!initialized) return true
        val released = CountDownLatch(1)
        eglRenderer.releaseEglSurface(released::countDown)
        ThreadUtils.awaitUninterruptibly(released)
        return true
    }

    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit

    private fun updateLayoutAspectRatio(width: Int, height: Int) {
        if (width > 0 && height > 0) eglRenderer.setLayoutAspectRatio(width.toFloat() / height)
    }
}
