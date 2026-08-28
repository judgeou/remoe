package top.ozaoza.remoe.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

internal fun littleEndianBuffer(size: Int): ByteBuffer =
    ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)

internal fun ByteArray.littleEndianView(): ByteBuffer =
    ByteBuffer.wrap(this).order(ByteOrder.LITTLE_ENDIAN)

internal fun ByteBuffer.putUnsignedShort(value: Int) {
    require(value in 0..0xffff) { "u16 out of range: $value" }
    putShort(value.toShort())
}

internal fun ByteBuffer.getUnsignedShortValue(): Int = short.toUShort().toInt()

internal fun ByteBuffer.putUnsignedInt(value: UInt) {
    putInt(value.toInt())
}

internal fun ByteBuffer.getUnsignedIntValue(): UInt = int.toUInt()

internal fun ByteBuffer.putUnsignedLong(value: ULong) {
    putLong(value.toLong())
}

internal fun ByteBuffer.getUnsignedLongValue(): ULong = long.toULong()

internal fun requireExactSize(bytes: ByteArray, expected: Int, name: String) {
    if (bytes.size != expected) {
        throw ProtocolException("$name length ${bytes.size}, expected $expected")
    }
}

internal fun validateHeader(
    buffer: ByteBuffer,
    expectedMagic: Int,
    expectedSize: Int,
    name: String,
) {
    val magic = buffer.int
    val version = buffer.getUnsignedShortValue()
    val headerSize = buffer.getUnsignedShortValue()
    if (magic != expectedMagic || version != Protocol.VERSION || headerSize != expectedSize) {
        throw ProtocolException("invalid $name magic, version, or header size")
    }
}

internal fun encodeUtf8(value: String): ByteArray = value.toByteArray(StandardCharsets.UTF_8)

internal fun decodeUtf8(bytes: ByteArray, offset: Int, length: Int, field: String): String = try {
    StandardCharsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes, offset, length))
        .toString()
} catch (error: CharacterCodingException) {
    throw ProtocolException("$field is not valid UTF-8", error)
}
