package top.ozaoza.remoe.protocol

object Protocol {
    const val VERSION = 11

    const val CLIENT_CONFIG_MAGIC = 0x46434d52
    const val STREAM_MAGIC = 0x454f4d52
    const val STREAM_READY_MAGIC = 0x59445253
    const val CLOCK_SYNC_MAGIC = 0x4b4c4343
    const val INPUT_MAGIC = 0x54504e49
    const val CLIPBOARD_MAGIC = 0x50494c43
    const val SIGNAL_MAGIC = 0x534d5257
    const val CODEC_AV1 = 0x31305641
    const val CODEC_H264 = 0x34363248

    const val CLIENT_CONFIG_SIZE = 36
    const val STREAM_HEADER_SIZE = 44
    const val STREAM_READY_SIZE = 8
    const val CLOCK_SYNC_REQUEST_SIZE = 24
    const val CLOCK_SYNC_RESPONSE_SIZE = 40
    const val INPUT_EVENT_SIZE = 24
    const val CLIPBOARD_HEADER_SIZE = 16
    const val SIGNAL_HEADER_SIZE = 20

    const val CLIENT_FLAG_CLIPBOARD_TEXT = 1
    const val INPUT_FLAG_RELEASE = 1
    const val INPUT_FLAG_EXTENDED_KEY = 2
    const val MAX_CLIPBOARD_TEXT_SIZE = 1024 * 1024
    const val MAX_SIGNAL_VALUE_SIZE = 1024 * 1024
    const val MAX_SIGNAL_CANDIDATE_SIZE = 16 * 1024
    const val MAX_SIGNAL_METADATA_SIZE = 256
}

class ProtocolException(message: String, cause: Throwable? = null) :
    IllegalArgumentException(message, cause)

enum class VideoCodec(val wireValue: Int) {
    AV1(Protocol.CODEC_AV1),
    H264(Protocol.CODEC_H264),
    ;

    companion object {
        internal fun fromWire(value: Int): VideoCodec = entries.firstOrNull { it.wireValue == value }
            ?: throw ProtocolException("unsupported video codec: 0x${value.toUInt().toString(16)}")
    }
}

enum class VideoRateControl(val wireValue: Int) {
    CBR(0),
    FIXED_QUALITY(1),
    ;

    companion object {
        internal fun fromWire(value: Int): VideoRateControl = entries.firstOrNull {
            it.wireValue == value
        } ?: throw ProtocolException("unsupported rate control: $value")
    }
}

enum class InputType(val wireValue: Int) {
    MOUSE_MOVE(1),
    MOUSE_LEFT(2),
    MOUSE_RIGHT(3),
    MOUSE_MIDDLE(4),
    MOUSE_X1(5),
    MOUSE_X2(6),
    MOUSE_WHEEL(7),
    MOUSE_HORIZONTAL_WHEEL(8),
    KEYBOARD(9),
    ;

    companion object {
        internal fun fromWire(value: Int): InputType = entries.firstOrNull { it.wireValue == value }
            ?: throw ProtocolException("unsupported input type: $value")
    }
}

enum class SignalType(val wireValue: Int) {
    DESCRIPTION(1),
    CANDIDATE(2),
    READY(3),
    ACKNOWLEDGED(4),
    ;

    val isMarker: Boolean get() = this == READY || this == ACKNOWLEDGED

    companion object {
        internal fun fromWire(value: Int): SignalType = entries.firstOrNull { it.wireValue == value }
            ?: throw ProtocolException("unsupported signal type: $value")
    }
}

data class ClientConfig(
    val fpsNum: Int = 60,
    val fpsDen: Int = 1,
    val bitrateBps: Int = 20_000_000,
    val scalePercent: Int = 100,
    val flags: Int = Protocol.CLIENT_FLAG_CLIPBOARD_TEXT,
    val rateControl: VideoRateControl = VideoRateControl.CBR,
    val quality: Int = 0,
)

data class StreamHeader(
    val codec: VideoCodec,
    val width: Int,
    val height: Int,
    val fpsNum: Int,
    val fpsDen: Int = 1,
    val bitrateBps: Int,
    val codecProfile: Int = 0,
    val rateControl: VideoRateControl = VideoRateControl.CBR,
    val quality: Int = 0,
)

data class ClockSyncRequest(
    val sequence: UInt,
    val clientSendUs: ULong,
)

data class ClockSyncResponse(
    val sequence: UInt,
    val clientSendUs: ULong,
    val hostReceiveUs: ULong,
    val hostSendUs: ULong,
)

data class RemoteInputEvent(
    val type: InputType,
    val flags: Int = 0,
    val value1: Int = 0,
    val value2: Int = 0,
    val sequence: UInt = 0u,
)

data class ClipboardText(
    val text: String,
    val sequence: UInt,
)

data class SignalFrame(
    val type: SignalType,
    val value: String = "",
    val metadata: String = "",
)
