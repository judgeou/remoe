package top.ozaoza.remoe.signaling

import java.net.URI
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

data class ConnectionInvite(
    val websocketUrl: String,
    val stunUrl: String,
    val sessionId: String,
    val displayHost: String,
)

object InviteParser {
    private val sessionPattern = Regex("^[A-Za-z0-9_-]{8,64}$")

    fun parse(input: String): ConnectionInvite {
        val source = try {
            URI(input.trim())
        } catch (error: Exception) {
            throw IllegalArgumentException("邀请 URL 格式无效", error)
        }
        val sourceScheme = source.scheme?.lowercase()
            ?: throw IllegalArgumentException("邀请 URL 缺少 scheme")
        val websocketScheme = when (sourceScheme) {
            "https", "wss" -> "wss"
            "http", "ws" -> "ws"
            else -> throw IllegalArgumentException("邀请必须使用 http(s) 或 ws(s)")
        }
        val host = source.host ?: throw IllegalArgumentException("邀请 URL 缺少主机名")
        if (source.rawUserInfo != null) throw IllegalArgumentException("邀请 URL 不得包含用户信息")
        val session = source.rawFragment.orEmpty()
        if (!sessionPattern.matches(session)) {
            throw IllegalArgumentException("邀请 URL 缺少有效的 #session")
        }
        val path = if (sourceScheme == "http" || sourceScheme == "https") {
            "/signal"
        } else {
            source.rawPath.takeUnless { it.isNullOrBlank() } ?: "/signal"
        }
        val query = "session=${encode(session)}&role=client"
        val websocket = URI(
            websocketScheme,
            null,
            host,
            source.port,
            path,
            query,
            null,
        )
        val stunHost = if (host.startsWith('[')) host else if (host.contains(':')) "[$host]" else host
        return ConnectionInvite(
            websocketUrl = websocket.toASCIIString(),
            stunUrl = "stun:$stunHost:3478",
            sessionId = session,
            displayHost = host,
        )
    }

    private fun encode(value: String): String =
        URLEncoder.encode(value, StandardCharsets.UTF_8.name()).replace("+", "%20")
}
