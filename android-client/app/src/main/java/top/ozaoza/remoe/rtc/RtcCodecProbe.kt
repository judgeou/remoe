package top.ozaoza.remoe.rtc

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.opengl.GLES20
import android.os.Build
import org.webrtc.HardwareVideoDecoderFactory
import org.webrtc.VideoCodecInfo
import org.webrtc.VideoCodecStatus
import org.webrtc.VideoDecoder
import org.webrtc.WrappedNativeVideoDecoder
import java.util.Locale

class RtcCodecProbe(private val runtime: RtcRuntime) {
    fun run(): String = buildString {
        appendLine("RESULT: ${probeResult()}")
        appendLine()
        appendLine("libwebrtc AAR: $LIBWEBRTC_VERSION")
        appendLine("libwebrtc revision: $LIBWEBRTC_REVISION")
        appendLine("Device: ${Build.MANUFACTURER} ${Build.MODEL}")
        appendLine("Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        appendLine()
        appendLine("EGL / GPU")
        appendLine(probeGl().prependIndent("  "))
        appendLine()
        appendLine("libwebrtc decoder factory")
        appendLine(probeWebRtcCodecs().prependIndent("  "))
        appendLine()
        appendLine("Android MediaCodec")
        append(probeMediaCodecs().prependIndent("  "))
    }

    private fun probeResult(): String {
        val lowLatencyFactory = HardwareVideoDecoderFactory(runtime.eglBase.eglBaseContext) { info ->
            info.name.endsWith(".low_latency", ignoreCase = true)
        }
        val av1Codecs = lowLatencyFactory.supportedCodecs
            .filter { it.name.equals("AV1", ignoreCase = true) }
        if (av1Codecs.isEmpty()) return "FAIL — libwebrtc 未暴露 low-latency AV1 MediaCodec"

        val outcomes = mutableListOf<String>()
        for (codec in av1Codecs) {
            try {
                val decoder = lowLatencyFactory.createDecoder(codec)
                if (decoder == null) {
                    outcomes += "${codec.params}: unavailable"
                    continue
                }
                if (decoder.isNativeWrapper()) {
                    outcomes += "${codec.params}: ${decoder.typeName()} 已创建"
                    continue
                }
                val implementation = runCatching { decoder.implementationName }
                    .getOrDefault(decoder.typeName())
                val status = try {
                    decoder.initDecode(
                        VideoDecoder.Settings(Runtime.getRuntime().availableProcessors(), 1920, 1080),
                    ) { _, _, _ -> }
                } finally {
                    runCatching { decoder.release() }
                }
                outcomes += "${codec.params}: $status ($implementation)"
                if (status == VideoCodecStatus.OK) {
                    return "PASS — low-latency AV1 decoder 已初始化 ($implementation)"
                }
            } catch (error: Throwable) {
                outcomes += "${codec.params}: ${error.javaClass.simpleName}: ${error.message}"
            }
        }
        return "FAIL — low-latency AV1 decoder 未能初始化；${outcomes.joinToString(" | ")}"
    }

    private fun probeWebRtcCodecs(): String = buildString {
        val codecs = runtime.decoderFactory.supportedCodecs
        appendLine("supported=${codecs.size}")
        codecs.forEach { codec ->
            append("- ${codec.name}")
            if (codec.params.isNotEmpty()) {
                append(" ")
                append(codec.params.toSortedMap().entries.joinToString("; ") { "${it.key}=${it.value}" })
            }
            append(" | decoder=")
            append(probeDecoderCreation(codec))
            appendLine()
        }
    }.trimEnd()

    private fun probeDecoderCreation(codec: VideoCodecInfo): String = try {
        val decoder = runtime.decoderFactory.createDecoder(codec) ?: return "unavailable"
        if (decoder.isNativeWrapper()) {
            "${decoder.typeName()} (created; native lifecycle)"
        } else {
            val name = runCatching { decoder.implementationName }
                .getOrDefault(decoder.typeName())
            runCatching { decoder.release() }
            "$name (${decoder.typeName()})"
        }
    } catch (error: Throwable) {
        "ERROR(${error.javaClass.simpleName})"
    }

    private fun VideoDecoder.isNativeWrapper(): Boolean = this is WrappedNativeVideoDecoder

    private fun VideoDecoder.typeName(): String =
        javaClass.simpleName.ifEmpty { javaClass.name.substringAfterLast('.') }

    private fun probeMediaCodecs(): String = buildString {
        val wantedTypes = setOf("video/av01", "video/avc")
        val decoders = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos
            .asSequence()
            .filterNot { it.isEncoder }
            .flatMap { info ->
                info.supportedTypes.asSequence()
                    .filter { type -> wantedTypes.any { it.equals(type, ignoreCase = true) } }
                    .map { type -> info to type }
            }
            .sortedWith(compareBy({ it.second }, { it.first.name }))
            .toList()

        if (decoders.isEmpty()) appendLine("未发现 AV1/H.264 decoder")
        decoders.forEach { (info, type) ->
            val capabilities = runCatching { info.getCapabilitiesForType(type) }.getOrNull()
            val hardware = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                info.isHardwareAccelerated.toString()
            } else {
                "unknown(API<29)"
            }
            val lowLatency = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && capabilities != null) {
                capabilities.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency).toString()
            } else {
                "unknown(API<30)"
            }
            appendLine("- ${info.name}")
            appendLine("  mime=${type.lowercase(Locale.US)}, hardware=$hardware, lowLatency=$lowLatency")
        }
    }.trimEnd()

    private fun probeGl(): String = synchronized(runtime.eglBase) {
        try {
            runtime.eglBase.createDummyPbufferSurface()
            runtime.eglBase.makeCurrent()
            listOf(
                "vendor=${GLES20.glGetString(GLES20.GL_VENDOR)}",
                "renderer=${GLES20.glGetString(GLES20.GL_RENDERER)}",
                "version=${GLES20.glGetString(GLES20.GL_VERSION)}",
            ).joinToString("\n")
        } catch (error: Throwable) {
            "ERROR(${error.javaClass.simpleName}): ${error.message}"
        } finally {
            if (runtime.eglBase.hasSurface()) {
                runtime.eglBase.detachCurrent()
                runtime.eglBase.releaseSurface()
            }
        }
    }

    private companion object {
        const val LIBWEBRTC_VERSION = "144.7559.09"
        const val LIBWEBRTC_REVISION = "b1800a61db8320af5c14456c13622d8b85b1ed39"
    }
}
