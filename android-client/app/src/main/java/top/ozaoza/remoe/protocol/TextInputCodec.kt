package top.ozaoza.remoe.protocol

import java.nio.CharBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

object TextInputCodec {
    fun encode(value: TextInput): ByteArray {
        val payload = checkedUtf8(value.text)
        if (payload.size > Protocol.MAX_TEXT_INPUT_SIZE) {
            throw ProtocolException("text input exceeds 4096 bytes")
        }
        return encodePayload(payload, value.sequence)
    }

    fun decode(bytes: ByteArray): TextInput {
        if (bytes.size < Protocol.TEXT_INPUT_HEADER_SIZE) {
            throw ProtocolException("truncated text input")
        }
        val buffer = bytes.littleEndianView()
        validateHeader(buffer, Protocol.TEXT_INPUT_MAGIC, Protocol.TEXT_INPUT_HEADER_SIZE, "TextInput")
        val size = buffer.int
        if (size !in 0..Protocol.MAX_TEXT_INPUT_SIZE ||
            bytes.size != Protocol.TEXT_INPUT_HEADER_SIZE + size
        ) throw ProtocolException("invalid text input payload size")
        val sequence = buffer.getUnsignedIntValue()
        val text = decodeUtf8(bytes, Protocol.TEXT_INPUT_HEADER_SIZE, size, "text input")
        if ('\u0000' in text) throw ProtocolException("text input contains NUL")
        return TextInput(text, sequence)
    }

    // Large IME commits (including paste) stay below SCTP message limits and
    // never split a UTF-8 character or a Windows CRLF line ending.
    fun encodeMessages(text: String, firstSequence: UInt): List<ByteArray> {
        if (text.length > Protocol.MAX_CLIPBOARD_TEXT_SIZE) {
            throw ProtocolException("text input exceeds 1 MiB")
        }
        val payload = checkedUtf8(text.replace("\r\n", "\n"))
        if (payload.size > Protocol.MAX_CLIPBOARD_TEXT_SIZE) {
            throw ProtocolException("text input exceeds 1 MiB")
        }
        val messages = mutableListOf<ByteArray>()
        var offset = 0
        while (offset < payload.size) {
            var end = minOf(offset + Protocol.MAX_TEXT_INPUT_SIZE, payload.size)
            while (end < payload.size && payload[end].toInt() and 0xc0 == 0x80) end--
            messages += encodePayload(payload.copyOfRange(offset, end), firstSequence + messages.size.toUInt())
            offset = end
        }
        return messages
    }

    private fun checkedUtf8(text: String): ByteArray {
        if ('\u0000' in text) throw ProtocolException("text input contains NUL")
        return try {
            val encoded = StandardCharsets.UTF_8.newEncoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .encode(CharBuffer.wrap(text))
            ByteArray(encoded.remaining()).also { encoded.get(it) }
        } catch (error: CharacterCodingException) {
            throw ProtocolException("text input contains invalid Unicode", error)
        }
    }

    private fun encodePayload(payload: ByteArray, sequence: UInt): ByteArray =
        littleEndianBuffer(Protocol.TEXT_INPUT_HEADER_SIZE + payload.size).apply {
            putInt(Protocol.TEXT_INPUT_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.TEXT_INPUT_HEADER_SIZE)
            putInt(payload.size)
            putUnsignedInt(sequence)
            put(payload)
        }.array()
}
