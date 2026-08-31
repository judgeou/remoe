package top.ozaoza.remoe.rtc

data class RtcPerformanceStats(
    val fps: Double = 0.0,
    val bitrateMbps: Double = 0.0,
    val dataRateKBps: Double = 0.0,
    val lossEvents: Long = 0,
)

internal class RtcPerformanceStatsCalculator {
    private data class Sample(
        val timestampNanos: Long,
        val bytesReceived: Long,
        val framesDecoded: Long,
        val packetsLost: Long,
    )

    private var previous: Sample? = null

    @Synchronized
    fun reset() {
        previous = null
    }

    @Synchronized
    fun update(
        timestampNanos: Long,
        bytesReceived: Long,
        framesDecoded: Long,
        packetsLost: Long,
    ): RtcPerformanceStats? {
        val current = Sample(
            timestampNanos = timestampNanos,
            bytesReceived = bytesReceived.coerceAtLeast(0),
            framesDecoded = framesDecoded.coerceAtLeast(0),
            packetsLost = packetsLost.coerceAtLeast(0),
        )
        val last = previous
        previous = current
        if (last == null || current.timestampNanos <= last.timestampNanos) return null

        val elapsedSeconds = (current.timestampNanos - last.timestampNanos) / 1_000_000_000.0
        val receivedBytes = (current.bytesReceived - last.bytesReceived).coerceAtLeast(0)
        val decodedFrames = (current.framesDecoded - last.framesDecoded).coerceAtLeast(0)
        return RtcPerformanceStats(
            fps = decodedFrames / elapsedSeconds,
            bitrateMbps = receivedBytes * 8.0 / elapsedSeconds / 1_000_000.0,
            dataRateKBps = receivedBytes / elapsedSeconds / 1_000.0,
            lossEvents = (current.packetsLost - last.packetsLost).coerceAtLeast(0),
        )
    }
}
