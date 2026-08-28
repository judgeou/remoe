package top.ozaoza.remoe.binding

import android.os.Build
import android.util.Base64
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.json.JSONObject
import java.io.IOException
import java.security.SecureRandom

data class BindingState(
    val bindingId: String,
    val status: String,
    val expiresAt: Long,
    val comparisonCode: String?,
)

data class ActiveBinding(
    val server: HttpUrl,
    val bindingId: String,
    val clientSecret: String,
)

class AndroidBindingClient(private val httpClient: OkHttpClient) {
    private val jsonType = "application/json; charset=utf-8".toMediaType()

    fun claim(invite: BindInvite, callback: (Result<Pair<ActiveBinding, BindingState>>) -> Unit) {
        val secret = createSecret()
        val body = JSONObject()
            .put("token", invite.token)
            .put("clientSecret", secret)
            .put("deviceName", Build.DEVICE.ifBlank { "Android device" })
            .put("deviceModel", "${Build.MANUFACTURER} ${Build.MODEL}".trim())
        val request = Request.Builder()
            .url(invite.server.newBuilder().addPathSegments("api/android/bind/claim").build())
            .post(body.toString().toRequestBody(jsonType))
            .build()
        httpClient.newCall(request).enqueue(jsonCallback(callback) { json ->
            val state = parseState(json)
            ActiveBinding(invite.server, state.bindingId, secret) to state
        })
    }

    fun status(binding: ActiveBinding, callback: (Result<BindingState>) -> Unit) {
        val url = binding.server.newBuilder()
            .addPathSegments("api/android/bind/status")
            .addQueryParameter("id", binding.bindingId)
            .build()
        val request = Request.Builder().url(url)
            .header("Authorization", "Bearer ${binding.clientSecret}")
            .get()
            .build()
        httpClient.newCall(request).enqueue(jsonCallback(callback, ::parseState))
    }

    private fun <T> jsonCallback(
        callback: (Result<T>) -> Unit,
        transform: (JSONObject) -> T,
    ) = object : Callback {
        override fun onFailure(call: Call, error: IOException) = callback(Result.failure(error))

        override fun onResponse(call: Call, response: Response) {
            response.use {
                try {
                    val json = JSONObject(it.body.string())
                    if (!it.isSuccessful) throw IOException(json.optString("error", "请求失败 (${it.code})"))
                    callback(Result.success(transform(json)))
                } catch (error: Exception) {
                    callback(Result.failure(error))
                }
            }
        }
    }

    private fun parseState(json: JSONObject) = BindingState(
        bindingId = json.getString("bindingId"),
        status = json.getString("status"),
        expiresAt = json.getLong("expiresAt"),
        comparisonCode = json.optString("comparisonCode").ifBlank { null },
    )

    private fun createSecret(): String {
        val bytes = ByteArray(32).also(SecureRandom()::nextBytes)
        return Base64.encodeToString(bytes, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
    }
}
