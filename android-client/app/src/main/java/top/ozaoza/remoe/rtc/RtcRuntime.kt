package top.ozaoza.remoe.rtc

import android.content.Context
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.EglBase
import org.webrtc.PeerConnectionFactory

class RtcRuntime private constructor(
    val eglBase: EglBase,
    val decoderFactory: DefaultVideoDecoderFactory,
    val peerConnectionFactory: PeerConnectionFactory,
) {
    companion object {
        fun create(context: Context): RtcRuntime {
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(context.applicationContext)
                    .setEnableInternalTracer(false)
                    .createInitializationOptions(),
            )

            val eglBase = EglBase.create()
            try {
                val decoderFactory = DefaultVideoDecoderFactory(eglBase.eglBaseContext)
                val peerConnectionFactory = PeerConnectionFactory.builder()
                    .setVideoDecoderFactory(decoderFactory)
                    .createPeerConnectionFactory()
                return RtcRuntime(eglBase, decoderFactory, peerConnectionFactory)
            } catch (error: Throwable) {
                eglBase.release()
                throw error
            }
        }
    }

    fun release() {
        peerConnectionFactory.dispose()
        eglBase.release()
    }
}
