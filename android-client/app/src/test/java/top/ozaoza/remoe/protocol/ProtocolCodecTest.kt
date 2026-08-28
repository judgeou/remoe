package top.ozaoza.remoe.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ProtocolCodecTest {
    @Test
    fun clientConfigMatchesWebGoldenBytes() {
        val cbr = ClientConfig(
            fpsNum = 90,
            bitrateBps = 25_000_000,
            scalePercent = 75,
        )
        assertArrayEquals(
            hex("524d43460b0024005a0000000100000040787d014b000000010000000000000000000000"),
            ClientConfigCodec.encode(cbr),
        )
        assertEquals(cbr, ClientConfigCodec.decode(ClientConfigCodec.encode(cbr)))

        val fixedQuality = ClientConfig(
            fpsNum = 60,
            bitrateBps = 40_000_000,
            scalePercent = 100,
            rateControl = VideoRateControl.FIXED_QUALITY,
            quality = 24,
        )
        assertArrayEquals(
            hex("524d43460b0024003c00000001000000005a620264000000010000000100000018000000"),
            ClientConfigCodec.encode(fixedQuality),
        )
        assertEquals(fixedQuality, ClientConfigCodec.decode(ClientConfigCodec.encode(fixedQuality)))
    }

    @Test
    fun clientConfigRejectsBadHeadersReservedFlagsAndRateControl() {
        val valid = ClientConfigCodec.encode(ClientConfig())
        assertProtocolFailure(valid.copyOf(35)) { ClientConfigCodec.decode(it) }
        assertProtocolFailure(valid.mutatedU16(4, 10)) { ClientConfigCodec.decode(it) }
        assertProtocolFailure(valid.mutatedU16(6, 35)) { ClientConfigCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(24, 2)) { ClientConfigCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(28, 2)) { ClientConfigCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(32, 1)) { ClientConfigCodec.decode(it) }
    }

    @Test
    fun streamHeadersMatchAv1AndH264GoldenBytes() {
        val av1 = StreamHeader(
            codec = VideoCodec.AV1,
            width = 1920,
            height = 1080,
            fpsNum = 60,
            bitrateBps = 20_000_000,
        )
        val av1Golden = hex(
            "524d4f450b002c004156303180070000380400003c00000001000000002d3101" +
                "000000000000000000000000",
        )
        assertArrayEquals(av1Golden, StreamHeaderCodec.encode(av1))
        assertEquals(av1, StreamHeaderCodec.decode(av1Golden))

        val h264 = StreamHeader(
            codec = VideoCodec.H264,
            width = 1280,
            height = 720,
            fpsNum = 30,
            bitrateBps = 8_000_000,
            codecProfile = 0x42e02a,
        )
        val h264Golden = hex(
            "524d4f450b002c004832363400050000d00200001e0000000100000000127a00" +
                "2ae042000000000000000000",
        )
        assertArrayEquals(h264Golden, StreamHeaderCodec.encode(h264))
        assertEquals(h264, StreamHeaderCodec.decode(h264Golden))
    }

    @Test
    fun streamHeaderRejectsUnsupportedOrInconsistentValues() {
        val valid = StreamHeaderCodec.encode(
            StreamHeader(VideoCodec.AV1, 1920, 1080, 60, bitrateBps = 20_000_000),
        )
        assertProtocolFailure(valid.mutatedInt(0, 0)) { StreamHeaderCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(8, 0x30395056)) { StreamHeaderCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(12, 1)) { StreamHeaderCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(32, 1)) { StreamHeaderCodec.decode(it) }
        assertProtocolFailure(valid.mutatedInt(36, 2)) { StreamHeaderCodec.decode(it) }

        val h264 = StreamHeaderCodec.encode(
            StreamHeader(
                VideoCodec.H264,
                1280,
                720,
                30,
                bitrateBps = 8_000_000,
                codecProfile = 0x42e02a,
            ),
        )
        assertProtocolFailure(h264.mutatedInt(36, 1).mutatedInt(40, 24)) {
            StreamHeaderCodec.decode(it)
        }
    }

    @Test
    fun streamReadyAndClockSyncMatchPackedGoldenBytes() {
        val ready = hex("535244590b000800")
        assertArrayEquals(ready, StreamReadyCodec.encode())
        StreamReadyCodec.decode(ready)
        assertProtocolFailure(ready.mutatedU16(4, 12)) { StreamReadyCodec.decode(it) }

        val request = ClockSyncRequest(0x89abcdefu, 0x0123456789abcdefuL)
        val requestGolden = hex("43434c4b0b001800efcdab8900000000efcdab8967452301")
        assertArrayEquals(requestGolden, ClockSyncCodec.encodeRequest(request))
        assertEquals(request, ClockSyncCodec.decodeRequest(requestGolden))

        val response = ClockSyncResponse(
            sequence = 0x89abcdefu,
            clientSendUs = 0x0123456789abcdefuL,
            hostReceiveUs = 0x1112131415161718uL,
            hostSendUs = 0xf1f2f3f4f5f6f7f8uL,
        )
        val responseGolden = hex(
            "43434c4b0b002800efcdab8900000000efcdab8967452301" +
                "1817161514131211f8f7f6f5f4f3f2f1",
        )
        assertArrayEquals(responseGolden, ClockSyncCodec.encodeResponse(response))
        assertEquals(response, ClockSyncCodec.decodeResponse(responseGolden))
        assertProtocolFailure(responseGolden.mutatedInt(12, 1)) {
            ClockSyncCodec.decodeResponse(it)
        }
    }

    @Test
    fun inputEventPreservesSignedValuesAndUnsignedSequence() {
        val event = RemoteInputEvent(
            type = InputType.MOUSE_WHEEL,
            value1 = -120,
            sequence = UInt.MAX_VALUE,
        )
        val golden = hex("494e50540b0018000700000088ffffff00000000ffffffff")
        assertArrayEquals(golden, InputEventCodec.encode(event))
        assertEquals(event, InputEventCodec.decode(golden))

        val keyboard = RemoteInputEvent(
            type = InputType.KEYBOARD,
            flags = Protocol.INPUT_FLAG_RELEASE or Protocol.INPUT_FLAG_EXTENDED_KEY,
            value1 = 0x4b,
            sequence = 17u,
        )
        assertEquals(keyboard, InputEventCodec.decode(InputEventCodec.encode(keyboard)))
        assertProtocolFailure(InputEventCodec.encode(keyboard).mutatedU16(10, 4)) {
            InputEventCodec.decode(it)
        }
        assertProtocolFailure(golden.mutatedU16(8, 10)) { InputEventCodec.decode(it) }
    }

    @Test
    fun clipboardMatchesUtf8GoldenAndRejectsMalformedFrames() {
        val value = ClipboardText("remoe 剪贴板 🚀", 23u)
        val golden = hex(
            "434c49500b0010001400000017000000" +
                "72656d6f6520e589aae8b4b4e69dbf20f09f9a80",
        )
        assertArrayEquals(golden, ClipboardCodec.encode(value))
        assertEquals(value, ClipboardCodec.decode(golden))
        assertProtocolFailure(golden.copyOf(15)) { ClipboardCodec.decode(it) }
        assertProtocolFailure(golden.copyOf(golden.size - 1)) { ClipboardCodec.decode(it) }
        assertProtocolFailure(golden.mutatedInt(8, Protocol.MAX_CLIPBOARD_TEXT_SIZE + 1)) {
            ClipboardCodec.decode(it)
        }

        val malformedUtf8 = ClipboardCodec.encode(ClipboardText("xx", 1u)).apply {
            this[16] = 0xc3.toByte()
            this[17] = 0x28
        }
        assertProtocolFailure(malformedUtf8) { ClipboardCodec.decode(it) }
        val maximum = ClipboardCodec.encode(
            ClipboardText("x".repeat(Protocol.MAX_CLIPBOARD_TEXT_SIZE), UInt.MAX_VALUE),
        )
        assertEquals(
            Protocol.MAX_CLIPBOARD_TEXT_SIZE,
            ClipboardCodec.decode(maximum).text.length,
        )
        expectProtocolFailure {
            ClipboardCodec.encode(ClipboardText("🚀".repeat(262_145), 0u))
        }
    }

    @Test
    fun signalFramesMatchGoldenAndBufferSplitOrMergedInput() {
        val description = SignalFrame(SignalType.DESCRIPTION, "sdp-value", "answer")
        val golden = hex(
            "57524d530b001400010000000900000006000000" +
                "7364702d76616c7565616e73776572",
        )
        assertArrayEquals(golden, SignalFrameCodec.encode(description))

        val split = SignalFrameBuffer()
        assertTrue(split.push(golden.copyOfRange(0, 7)).isEmpty())
        assertTrue(split.push(golden.copyOfRange(7, 22)).isEmpty())
        assertEquals(listOf(description), split.push(golden.copyOfRange(22, golden.size)))

        val ready = SignalFrameCodec.encode(SignalFrame(SignalType.READY))
        val candidate = SignalFrameCodec.encode(
            SignalFrame(SignalType.CANDIDATE, "candidate:1 1 UDP 1 127.0.0.1 9 typ host", "0"),
        )
        assertEquals(
            listOf(SignalFrame(SignalType.READY), SignalFrameCodec.decodeFrame(candidate)),
            SignalFrameBuffer().push(ready + candidate),
        )
    }

    @Test
    fun signalBufferRejectsInvalidHeadersLengthsMarkersAndUtf8() {
        val valid = SignalFrameCodec.encode(SignalFrame(SignalType.DESCRIPTION, "offer", "offer"))
        assertProtocolFailure(valid.mutatedU16(4, 10)) { SignalFrameBuffer().push(it) }
        assertProtocolFailure(valid.mutatedU16(6, 19)) { SignalFrameBuffer().push(it) }
        assertProtocolFailure(valid.mutatedU16(10, 1)) { SignalFrameBuffer().push(it) }
        assertProtocolFailure(valid.mutatedU16(8, 5)) { SignalFrameBuffer().push(it) }
        assertProtocolFailure(valid.mutatedInt(12, Protocol.MAX_SIGNAL_VALUE_SIZE + 1)) {
            SignalFrameBuffer().push(it)
        }

        val marker = SignalFrameCodec.encode(SignalFrame(SignalType.READY))
        assertProtocolFailure(marker.mutatedInt(12, 1)) { SignalFrameBuffer().push(it) }

        val invalidUtf8 = SignalFrameCodec.encode(
            SignalFrame(SignalType.CANDIDATE, "xx", "0"),
        ).apply {
            this[20] = 0xc3.toByte()
            this[21] = 0x28
        }
        assertProtocolFailure(invalidUtf8) { SignalFrameBuffer().push(it) }
    }

    private fun hex(value: String): ByteArray {
        require(value.length % 2 == 0)
        return ByteArray(value.length / 2) { index ->
            value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }

    private fun ByteArray.mutatedU16(offset: Int, value: Int): ByteArray = copyOf().apply {
        ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putShort(offset, value.toShort())
    }

    private fun ByteArray.mutatedInt(offset: Int, value: Int): ByteArray = copyOf().apply {
        ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN).putInt(offset, value)
    }

    private fun assertProtocolFailure(bytes: ByteArray, action: (ByteArray) -> Unit) {
        expectProtocolFailure { action(bytes) }
    }

    private fun expectProtocolFailure(action: () -> Unit) {
        try {
            action()
            fail("expected ProtocolException")
        } catch (_: ProtocolException) {
        }
    }
}
