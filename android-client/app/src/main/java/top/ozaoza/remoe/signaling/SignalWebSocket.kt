package top.ozaoza.remoe.signaling

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString

class SignalWebSocket(
    private val client: OkHttpClient,
    private val url: String,
    private val listener: Listener,
) {
    interface Listener {
        fun onRegistered()
        fun onBinaryMessage(bytes: ByteArray)
        fun onFailure(message: String, cause: Throwable? = null)
        fun onClosed(reason: String)
    }

    @Volatile
    private var socket: WebSocket? = null
    @Volatile
    private var registered = false
    @Volatile
    private var closedByClient = false

    fun connect() {
        check(socket == null) { "signal WebSocket already started" }
        socket = client.newWebSocket(Request.Builder().url(url).build(), SocketListener())
    }

    fun send(bytes: ByteArray): Boolean = socket?.send(bytes.toByteString()) == true

    fun close() {
        closedByClient = true
        socket?.close(1000, "Client stopped")
        socket = null
    }

    private inner class SocketListener : WebSocketListener() {
        override fun onMessage(webSocket: WebSocket, text: String) {
            if (text == "registered" && !registered) {
                registered = true
                listener.onRegistered()
                return
            }
            val message = when (text) {
                "error:invite-not-found" -> "邀请不存在、已经过期，或 Host 不在线"
                "error:invite-in-use" -> "这个邀请已经有 Client 在使用"
                "error:service-unavailable" -> "信令服务器当前不可用"
                else -> "信令服务器返回了未知响应"
            }
            listener.onFailure(message)
        }

        override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
            if (!registered) {
                listener.onFailure("信令服务器未先确认邀请注册")
                return
            }
            listener.onBinaryMessage(bytes.toByteArray())
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            if (!closedByClient) listener.onFailure("无法连接 WSS 信令服务器", t)
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            if (!closedByClient) listener.onClosed(reason.ifBlank { "WSS 已关闭" })
        }
    }
}
