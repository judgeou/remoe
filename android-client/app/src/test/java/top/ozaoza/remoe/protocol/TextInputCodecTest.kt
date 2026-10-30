package top.ozaoza.remoe.protocol

import org.junit.Assert.*
import org.junit.Test

class TextInputCodecTest {
    @Test
    fun matchesHostGoldenBytes() {
        val value = TextInput("中🚀", UInt.MAX_VALUE)
        val golden = "544558540b00100007000000ffffffffe4b8adf09f9a80"
            .chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        assertArrayEquals(golden, TextInputCodec.encode(value))
        assertEquals(value, TextInputCodec.decode(golden))
        val config = ClientConfig(flags = Protocol.CLIENT_FLAG_TEXT_INPUT)
        assertEquals(config, ClientConfigCodec.decode(ClientConfigCodec.encode(config)))
    }

    @Test
    fun chunksLargeCommitsWithoutBreakingUnicodeOrCrLf() {
        val text = "a".repeat(4094) + "中🚀\r\n" + "你好🌍".repeat(2000)
        val messages = TextInputCodec.encodeMessages(text, UInt.MAX_VALUE)
        val decoded = messages.map(TextInputCodec::decode)
        assertTrue(messages.size > 2)
        assertTrue(messages.all { it.size <= Protocol.TEXT_INPUT_HEADER_SIZE + Protocol.MAX_TEXT_INPUT_SIZE })
        assertEquals(text.replace("\r\n", "\n"), decoded.joinToString("") { it.text })
        assertEquals(UInt.MAX_VALUE, decoded[0].sequence)
        assertEquals(0u, decoded[1].sequence)
        assertTrue(TextInputCodec.encodeMessages("", 0u).isEmpty())
    }

    @Test
    fun rejectsInvalidHeadersTruncationAndUnicode() {
        val valid = TextInputCodec.encode(TextInput("xx", 0u))
        for (offset in listOf(0, 4, 6, 8)) {
            val invalid = valid.copyOf().apply { this[offset] = (this[offset].toInt() xor 1).toByte() }
            assertThrows(ProtocolException::class.java) { TextInputCodec.decode(invalid) }
        }
        assertThrows(ProtocolException::class.java) { TextInputCodec.decode(valid.copyOf(15)) }
        assertThrows(ProtocolException::class.java) { TextInputCodec.decode(valid.copyOf(valid.size - 1)) }
        val malformed = valid.copyOf().apply { this[16] = 0xc3.toByte(); this[17] = 0x28 }
        assertThrows(ProtocolException::class.java) { TextInputCodec.decode(malformed) }
        val nul = valid.copyOf().apply { this[16] = 0 }
        assertThrows(ProtocolException::class.java) { TextInputCodec.decode(nul) }
        for (invalid in listOf("a\u0000b", "\ud83d", "\ude80")) {
            assertThrows(ProtocolException::class.java) { TextInputCodec.encodeMessages(invalid, 0u) }
        }
        assertThrows(ProtocolException::class.java) {
            TextInputCodec.encode(TextInput("x".repeat(4097), 0u))
        }
        assertThrows(ProtocolException::class.java) {
            TextInputCodec.encodeMessages("🚀".repeat(262_145), 0u)
        }
    }
}
