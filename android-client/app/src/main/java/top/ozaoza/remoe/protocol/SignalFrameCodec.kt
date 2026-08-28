package top.ozaoza.remoe.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder

object SignalFrameCodec {
    fun encode(frame: SignalFrame): ByteArray {
        val value = encodeUtf8(frame.value)
        val metadata = encodeUtf8(frame.metadata)
        validatePayload(frame.type, value.size, metadata.size)
        return littleEndianBuffer(Protocol.SIGNAL_HEADER_SIZE + value.size + metadata.size).apply {
            putInt(Protocol.SIGNAL_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.SIGNAL_HEADER_SIZE)
            putUnsignedShort(frame.type.wireValue)
            putUnsignedShort(0)
            putInt(value.size)
            putInt(metadata.size)
            put(value)
            put(metadata)
        }.array()
    }

    internal fun decodeFrame(bytes: ByteArray, offset: Int = 0): SignalFrame {
        if (offset < 0 || bytes.size - offset < Protocol.SIGNAL_HEADER_SIZE) {
            throw ProtocolException("truncated signal frame")
        }
        val buffer = ByteBuffer.wrap(bytes, offset, bytes.size - offset).order(ByteOrder.LITTLE_ENDIAN)
        validateHeader(buffer, Protocol.SIGNAL_MAGIC, Protocol.SIGNAL_HEADER_SIZE, "signal frame")
        val type = SignalType.fromWire(buffer.getUnsignedShortValue())
        if (buffer.getUnsignedShortValue() != 0) {
            throw ProtocolException("signal reserved field is not zero")
        }
        val valueSize = buffer.int
        val metadataSize = buffer.int
        validatePayload(type, valueSize, metadataSize)
        val frameSize = frameSize(valueSize, metadataSize)
        if (bytes.size - offset != frameSize) {
            throw ProtocolException("signal frame length does not match its header")
        }
        return SignalFrame(
            type = type,
            value = decodeUtf8(
                bytes,
                offset + Protocol.SIGNAL_HEADER_SIZE,
                valueSize,
                "signal value",
            ),
            metadata = decodeUtf8(
                bytes,
                offset + Protocol.SIGNAL_HEADER_SIZE + valueSize,
                metadataSize,
                "signal metadata",
            ),
        )
    }

    internal fun validatePayload(type: SignalType, valueSize: Int, metadataSize: Int) {
        if (valueSize < 0 || metadataSize < 0 || metadataSize > Protocol.MAX_SIGNAL_METADATA_SIZE) {
            throw ProtocolException("invalid signal payload size")
        }
        when (type) {
            SignalType.DESCRIPTION -> if (
                valueSize !in 1..Protocol.MAX_SIGNAL_VALUE_SIZE || metadataSize !in 1..Protocol.MAX_SIGNAL_METADATA_SIZE
            ) throw ProtocolException("invalid WebRTC description frame")
            SignalType.CANDIDATE -> if (
                valueSize !in 1..Protocol.MAX_SIGNAL_CANDIDATE_SIZE
            ) throw ProtocolException("invalid WebRTC candidate frame")
            SignalType.READY, SignalType.ACKNOWLEDGED -> if (valueSize != 0 || metadataSize != 0) {
                throw ProtocolException("signal marker must not contain a payload")
            }
        }
    }

    internal fun frameSize(valueSize: Int, metadataSize: Int): Int {
        val size = Protocol.SIGNAL_HEADER_SIZE.toLong() + valueSize.toLong() + metadataSize.toLong()
        if (size > Int.MAX_VALUE) throw ProtocolException("signal frame is too large")
        return size.toInt()
    }
}

class SignalFrameBuffer {
    private var buffered = ByteArray(0)

    @Synchronized
    fun push(incoming: ByteArray): List<SignalFrame> {
        if (incoming.isEmpty()) return emptyList()
        val combinedSize = buffered.size.toLong() + incoming.size.toLong()
        if (combinedSize > Int.MAX_VALUE) throw ProtocolException("signal buffer is too large")
        val combined = ByteArray(combinedSize.toInt())
        buffered.copyInto(combined)
        incoming.copyInto(combined, buffered.size)

        val frames = mutableListOf<SignalFrame>()
        var offset = 0
        while (combined.size - offset >= Protocol.SIGNAL_HEADER_SIZE) {
            val header = ByteBuffer.wrap(
                combined,
                offset,
                combined.size - offset,
            ).order(ByteOrder.LITTLE_ENDIAN)
            validateHeader(header, Protocol.SIGNAL_MAGIC, Protocol.SIGNAL_HEADER_SIZE, "signal frame")
            val type = SignalType.fromWire(header.getUnsignedShortValue())
            if (header.getUnsignedShortValue() != 0) {
                throw ProtocolException("signal reserved field is not zero")
            }
            val valueSize = header.int
            val metadataSize = header.int
            SignalFrameCodec.validatePayload(type, valueSize, metadataSize)
            val frameSize = SignalFrameCodec.frameSize(valueSize, metadataSize)
            if (combined.size - offset < frameSize) break
            frames += SignalFrameCodec.decodeFrame(combined.copyOfRange(offset, offset + frameSize))
            offset += frameSize
        }
        buffered = combined.copyOfRange(offset, combined.size)
        return frames
    }

    @Synchronized
    fun clear() {
        buffered = ByteArray(0)
    }
}
