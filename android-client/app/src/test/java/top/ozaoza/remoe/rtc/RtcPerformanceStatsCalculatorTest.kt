package top.ozaoza.remoe.rtc

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class RtcPerformanceStatsCalculatorTest {
    @Test
    fun derivesTheSameMetricsAsTheWebClient() {
        val calculator = RtcPerformanceStatsCalculator()
        assertNull(calculator.update(1_000_000_000, 1_000_000, 100, 2))

        val stats = calculator.update(3_000_000_000, 6_000_000, 220, 5)!!
        assertEquals(60.0, stats.fps, 0.001)
        assertEquals(20.0, stats.bitrateMbps, 0.001)
        assertEquals(2_500.0, stats.dataRateKBps, 0.001)
        assertEquals(3, stats.lossEvents)
    }

    @Test
    fun resetRequiresAnotherBaselineSample() {
        val calculator = RtcPerformanceStatsCalculator()
        calculator.update(1, 1, 1, 1)
        calculator.reset()

        assertNull(calculator.update(2, 2, 2, 2))
    }

    @Test
    fun counterResetsDoNotProduceNegativeMetrics() {
        val calculator = RtcPerformanceStatsCalculator()
        calculator.update(1_000_000_000, 100, 100, 100)

        val stats = calculator.update(2_000_000_000, 10, 10, 10)!!
        assertEquals(0.0, stats.fps, 0.0)
        assertEquals(0.0, stats.bitrateMbps, 0.0)
        assertEquals(0.0, stats.dataRateKBps, 0.0)
        assertEquals(0, stats.lossEvents)
    }
}
