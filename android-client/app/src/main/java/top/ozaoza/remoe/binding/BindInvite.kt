package top.ozaoza.remoe.binding

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets

data class BindInvite(val server: HttpUrl, val token: String)

object BindInviteParser {
    private val tokenPattern = Regex("^[A-Za-z0-9_-]{32,256}$")

    fun parse(value: String): BindInvite {
        val uri = try {
            URI(value.trim())
        } catch (_: Exception) {
            throw IllegalArgumentException("这不是有效的 remoe 绑定二维码")
        }
        val query = uri.rawQuery.orEmpty().split('&')
            .filter { it.isNotEmpty() }
            .associate { part ->
                val pair = part.split('=', limit = 2)
                decode(pair[0]) to decode(pair.getOrElse(1) { "" })
            }
        require(uri.scheme == "remoe" && uri.host == "bind" && query["v"] == "1") {
            "这不是有效的 remoe 绑定二维码"
        }
        val server = query["server"]?.toHttpUrlOrNull()
            ?: throw IllegalArgumentException("绑定服务器地址无效")
        require(server.scheme == "https" && server.username.isEmpty() && server.password.isEmpty()) {
            "绑定服务器必须使用 HTTPS"
        }
        val token = query["token"].orEmpty()
        require(tokenPattern.matches(token)) { "绑定令牌无效" }
        return BindInvite(server, token)
    }

    private fun decode(value: String): String =
        URLDecoder.decode(value, StandardCharsets.UTF_8.name())
}
