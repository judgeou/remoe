package top.ozaoza.remoe.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import top.ozaoza.remoe.protocol.VideoRateControl

class VideoConnectionSettingsTest {
    @Test
    fun cbrSettingsKeepRememberedQualityButClearWireQuality() {
        val settings = VideoConnectionSettings(
            fps = 90,
            bitrateMbps = 35,
            scalePercent = 75,
            rateControl = VideoRateControl.CBR,
            quality = 24,
        )

        val config = settings.toClientConfig()
        assertEquals(90, config.fpsNum)
        assertEquals(35_000_000, config.bitrateBps)
        assertEquals(75, config.scalePercent)
        assertEquals(VideoRateControl.CBR, config.rateControl)
        assertEquals(0, config.quality)
        assertEquals(24, settings.quality)
    }

    @Test
    fun fixedQualitySettingsPreserveWireQuality() {
        val config = VideoConnectionSettings(
            rateControl = VideoRateControl.FIXED_QUALITY,
            quality = 18,
        ).toClientConfig()

        assertEquals(VideoRateControl.FIXED_QUALITY, config.rateControl)
        assertEquals(18, config.quality)
    }

    @Test
    fun invalidValuesCannotBePersisted() {
        assertThrows(IllegalArgumentException::class.java) {
            VideoConnectionSettings(fps = 0)
        }
        assertThrows(IllegalArgumentException::class.java) {
            VideoConnectionSettings(quality = 52)
        }
    }
}
