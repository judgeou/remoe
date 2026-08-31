package top.ozaoza.remoe.settings

import android.content.Context
import top.ozaoza.remoe.protocol.ClientConfig
import top.ozaoza.remoe.protocol.VideoRateControl

data class VideoConnectionSettings(
    val fps: Int = 60,
    val bitrateMbps: Int = 20,
    val scalePercent: Int = 100,
    val rateControl: VideoRateControl = VideoRateControl.CBR,
    val quality: Int = 28,
) {
    init {
        require(fps in 1..240)
        require(bitrateMbps in 1..1000)
        require(scalePercent in 10..100)
        require(quality in 1..51)
    }

    fun toClientConfig(): ClientConfig = ClientConfig(
        fpsNum = fps,
        bitrateBps = bitrateMbps * 1_000_000,
        scalePercent = scalePercent,
        rateControl = rateControl,
        quality = if (rateControl == VideoRateControl.FIXED_QUALITY) quality else 0,
    )
}

class VideoSettingsStore(context: Context) {
    private val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    fun load(): VideoConnectionSettings = runCatching {
        VideoConnectionSettings(
            fps = preferences.getInt(KEY_FPS, 60),
            bitrateMbps = preferences.getInt(KEY_BITRATE_MBPS, 20),
            scalePercent = preferences.getInt(KEY_SCALE_PERCENT, 100),
            rateControl = VideoRateControl.valueOf(
                preferences.getString(KEY_RATE_CONTROL, VideoRateControl.CBR.name)
                    ?: VideoRateControl.CBR.name,
            ),
            quality = preferences.getInt(KEY_QUALITY, 28),
        )
    }.getOrDefault(VideoConnectionSettings())

    fun save(settings: VideoConnectionSettings) {
        preferences.edit()
            .putInt(KEY_FPS, settings.fps)
            .putInt(KEY_BITRATE_MBPS, settings.bitrateMbps)
            .putInt(KEY_SCALE_PERCENT, settings.scalePercent)
            .putString(KEY_RATE_CONTROL, settings.rateControl.name)
            .putInt(KEY_QUALITY, settings.quality)
            .apply()
    }

    private companion object {
        const val PREFERENCES_NAME = "video_connection_settings"
        const val KEY_FPS = "fps"
        const val KEY_BITRATE_MBPS = "bitrate_mbps"
        const val KEY_SCALE_PERCENT = "scale_percent"
        const val KEY_RATE_CONTROL = "rate_control"
        const val KEY_QUALITY = "quality"
    }
}
