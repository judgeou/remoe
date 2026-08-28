package top.ozaoza.remoe.protocol

object ClientConfigCodec {
    fun encode(value: ClientConfig): ByteArray {
        validate(value)
        return littleEndianBuffer(Protocol.CLIENT_CONFIG_SIZE).apply {
            putInt(Protocol.CLIENT_CONFIG_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.CLIENT_CONFIG_SIZE)
            putInt(value.fpsNum)
            putInt(value.fpsDen)
            putInt(value.bitrateBps)
            putInt(value.scalePercent)
            putInt(value.flags)
            putInt(value.rateControl.wireValue)
            putInt(value.quality)
        }.array()
    }

    fun decode(bytes: ByteArray): ClientConfig {
        requireExactSize(bytes, Protocol.CLIENT_CONFIG_SIZE, "ClientConfig")
        val buffer = bytes.littleEndianView()
        validateHeader(buffer, Protocol.CLIENT_CONFIG_MAGIC, Protocol.CLIENT_CONFIG_SIZE, "ClientConfig")
        val value = ClientConfig(
            fpsNum = buffer.int,
            fpsDen = buffer.int,
            bitrateBps = buffer.int,
            scalePercent = buffer.int,
            flags = buffer.int,
            rateControl = VideoRateControl.fromWire(buffer.int),
            quality = buffer.int,
        )
        validate(value)
        return value
    }

    private fun validate(value: ClientConfig) {
        if (value.fpsNum !in 1..240 || value.fpsDen != 1) {
            throw ProtocolException("invalid ClientConfig frame rate")
        }
        if (value.bitrateBps !in 1_000_000..1_000_000_000) {
            throw ProtocolException("invalid ClientConfig bitrate")
        }
        if (value.scalePercent !in 10..100 ||
            value.flags and Protocol.CLIENT_FLAG_CLIPBOARD_TEXT.inv() != 0
        ) {
            throw ProtocolException("invalid ClientConfig scale or flags")
        }
        val qualityValid = when (value.rateControl) {
            VideoRateControl.CBR -> value.quality == 0
            VideoRateControl.FIXED_QUALITY -> value.quality in 1..51
        }
        if (!qualityValid) throw ProtocolException("invalid ClientConfig quality")
    }
}

object StreamHeaderCodec {
    fun encode(value: StreamHeader): ByteArray {
        validate(value)
        return littleEndianBuffer(Protocol.STREAM_HEADER_SIZE).apply {
            putInt(Protocol.STREAM_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.STREAM_HEADER_SIZE)
            putInt(value.codec.wireValue)
            putInt(value.width)
            putInt(value.height)
            putInt(value.fpsNum)
            putInt(value.fpsDen)
            putInt(value.bitrateBps)
            putInt(value.codecProfile)
            putInt(value.rateControl.wireValue)
            putInt(value.quality)
        }.array()
    }

    fun decode(bytes: ByteArray): StreamHeader {
        requireExactSize(bytes, Protocol.STREAM_HEADER_SIZE, "StreamHeader")
        val buffer = bytes.littleEndianView()
        validateHeader(buffer, Protocol.STREAM_MAGIC, Protocol.STREAM_HEADER_SIZE, "StreamHeader")
        val value = StreamHeader(
            codec = VideoCodec.fromWire(buffer.int),
            width = buffer.int,
            height = buffer.int,
            fpsNum = buffer.int,
            fpsDen = buffer.int,
            bitrateBps = buffer.int,
            codecProfile = buffer.int,
            rateControl = VideoRateControl.fromWire(buffer.int),
            quality = buffer.int,
        )
        validate(value)
        return value
    }

    private fun validate(value: StreamHeader) {
        if (value.width !in 2..16_384 || value.height !in 2..16_384 ||
            value.fpsNum < 1 || value.fpsDen != 1 || value.bitrateBps < 1_000_000
        ) {
            throw ProtocolException("invalid StreamHeader dimensions, frame rate, or bitrate")
        }
        val profileValid = when (value.codec) {
            VideoCodec.AV1 -> value.codecProfile == 0
            VideoCodec.H264 -> value.codecProfile in 1..0x00ff_ffff
        }
        val rateControlValid = when (value.rateControl) {
            VideoRateControl.CBR -> value.quality == 0
            VideoRateControl.FIXED_QUALITY ->
                value.codec == VideoCodec.AV1 && value.quality in 1..51
        }
        if (!profileValid || !rateControlValid) {
            throw ProtocolException("invalid StreamHeader codec profile or rate control")
        }
    }
}

object StreamReadyCodec {
    fun encode(): ByteArray = littleEndianBuffer(Protocol.STREAM_READY_SIZE).apply {
        putInt(Protocol.STREAM_READY_MAGIC)
        putUnsignedShort(Protocol.VERSION)
        putUnsignedShort(Protocol.STREAM_READY_SIZE)
    }.array()

    fun decode(bytes: ByteArray) {
        requireExactSize(bytes, Protocol.STREAM_READY_SIZE, "StreamReady")
        validateHeader(
            bytes.littleEndianView(),
            Protocol.STREAM_READY_MAGIC,
            Protocol.STREAM_READY_SIZE,
            "StreamReady",
        )
    }
}

object ClockSyncCodec {
    fun encodeRequest(value: ClockSyncRequest): ByteArray =
        littleEndianBuffer(Protocol.CLOCK_SYNC_REQUEST_SIZE).apply {
            putInt(Protocol.CLOCK_SYNC_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.CLOCK_SYNC_REQUEST_SIZE)
            putUnsignedInt(value.sequence)
            putInt(0)
            putUnsignedLong(value.clientSendUs)
        }.array()

    fun decodeRequest(bytes: ByteArray): ClockSyncRequest {
        requireExactSize(bytes, Protocol.CLOCK_SYNC_REQUEST_SIZE, "ClockSyncRequest")
        val buffer = bytes.littleEndianView()
        validateHeader(
            buffer,
            Protocol.CLOCK_SYNC_MAGIC,
            Protocol.CLOCK_SYNC_REQUEST_SIZE,
            "ClockSyncRequest",
        )
        val sequence = buffer.getUnsignedIntValue()
        if (buffer.int != 0) throw ProtocolException("ClockSyncRequest reserved field is not zero")
        return ClockSyncRequest(sequence, buffer.getUnsignedLongValue())
    }

    fun encodeResponse(value: ClockSyncResponse): ByteArray =
        littleEndianBuffer(Protocol.CLOCK_SYNC_RESPONSE_SIZE).apply {
            putInt(Protocol.CLOCK_SYNC_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.CLOCK_SYNC_RESPONSE_SIZE)
            putUnsignedInt(value.sequence)
            putInt(0)
            putUnsignedLong(value.clientSendUs)
            putUnsignedLong(value.hostReceiveUs)
            putUnsignedLong(value.hostSendUs)
        }.array()

    fun decodeResponse(bytes: ByteArray): ClockSyncResponse {
        requireExactSize(bytes, Protocol.CLOCK_SYNC_RESPONSE_SIZE, "ClockSyncResponse")
        val buffer = bytes.littleEndianView()
        validateHeader(
            buffer,
            Protocol.CLOCK_SYNC_MAGIC,
            Protocol.CLOCK_SYNC_RESPONSE_SIZE,
            "ClockSyncResponse",
        )
        val sequence = buffer.getUnsignedIntValue()
        if (buffer.int != 0) throw ProtocolException("ClockSyncResponse reserved field is not zero")
        return ClockSyncResponse(
            sequence = sequence,
            clientSendUs = buffer.getUnsignedLongValue(),
            hostReceiveUs = buffer.getUnsignedLongValue(),
            hostSendUs = buffer.getUnsignedLongValue(),
        )
    }
}

object InputEventCodec {
    fun encode(value: RemoteInputEvent): ByteArray {
        validate(value)
        return littleEndianBuffer(Protocol.INPUT_EVENT_SIZE).apply {
            putInt(Protocol.INPUT_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.INPUT_EVENT_SIZE)
            putUnsignedShort(value.type.wireValue)
            putUnsignedShort(value.flags)
            putInt(value.value1)
            putInt(value.value2)
            putUnsignedInt(value.sequence)
        }.array()
    }

    fun decode(bytes: ByteArray): RemoteInputEvent {
        requireExactSize(bytes, Protocol.INPUT_EVENT_SIZE, "InputEvent")
        val buffer = bytes.littleEndianView()
        validateHeader(buffer, Protocol.INPUT_MAGIC, Protocol.INPUT_EVENT_SIZE, "InputEvent")
        val value = RemoteInputEvent(
            type = InputType.fromWire(buffer.getUnsignedShortValue()),
            flags = buffer.getUnsignedShortValue(),
            value1 = buffer.int,
            value2 = buffer.int,
            sequence = buffer.getUnsignedIntValue(),
        )
        validate(value)
        return value
    }

    private fun validate(value: RemoteInputEvent) {
        val valid = when (value.type) {
            InputType.MOUSE_MOVE -> value.flags == 0 &&
                value.value1 in 0..65_535 && value.value2 in 0..65_535
            InputType.MOUSE_LEFT, InputType.MOUSE_RIGHT, InputType.MOUSE_MIDDLE,
            InputType.MOUSE_X1, InputType.MOUSE_X2 ->
                value.flags and Protocol.INPUT_FLAG_RELEASE.inv() == 0 && value.value2 == 0
            InputType.MOUSE_WHEEL, InputType.MOUSE_HORIZONTAL_WHEEL ->
                value.flags == 0 && value.value1 in -32_768..32_767 && value.value2 == 0
            InputType.KEYBOARD ->
                value.flags and (Protocol.INPUT_FLAG_RELEASE or
                    Protocol.INPUT_FLAG_EXTENDED_KEY).inv() == 0 &&
                    value.value1 in 1..0x1ff && value.value2 == 0
        }
        if (!valid) throw ProtocolException("invalid input event fields or reserved flags")
    }
}

object ClipboardCodec {
    fun encode(value: ClipboardText): ByteArray {
        val payload = encodeUtf8(value.text)
        if (payload.size > Protocol.MAX_CLIPBOARD_TEXT_SIZE) {
            throw ProtocolException("clipboard text exceeds 1 MiB")
        }
        return littleEndianBuffer(Protocol.CLIPBOARD_HEADER_SIZE + payload.size).apply {
            putInt(Protocol.CLIPBOARD_MAGIC)
            putUnsignedShort(Protocol.VERSION)
            putUnsignedShort(Protocol.CLIPBOARD_HEADER_SIZE)
            putInt(payload.size)
            putUnsignedInt(value.sequence)
            put(payload)
        }.array()
    }

    fun decode(bytes: ByteArray): ClipboardText {
        if (bytes.size < Protocol.CLIPBOARD_HEADER_SIZE) {
            throw ProtocolException("truncated clipboard message")
        }
        val buffer = bytes.littleEndianView()
        validateHeader(buffer, Protocol.CLIPBOARD_MAGIC, Protocol.CLIPBOARD_HEADER_SIZE, "Clipboard")
        val payloadSize = buffer.int
        if (payloadSize < 0 || payloadSize > Protocol.MAX_CLIPBOARD_TEXT_SIZE ||
            bytes.size != Protocol.CLIPBOARD_HEADER_SIZE + payloadSize
        ) {
            throw ProtocolException("invalid clipboard payload size")
        }
        val sequence = buffer.getUnsignedIntValue()
        return ClipboardText(
            text = decodeUtf8(
                bytes,
                Protocol.CLIPBOARD_HEADER_SIZE,
                payloadSize,
                "clipboard text",
            ),
            sequence = sequence,
        )
    }
}
