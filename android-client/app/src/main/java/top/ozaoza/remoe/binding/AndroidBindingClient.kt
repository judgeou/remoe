package top.ozaoza.remoe.binding

import android.os.Build
import android.util.Base64
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.json.JSONObject
import java.io.IOException
import java.security.SecureRandom
import top.ozaoza.remoe.auth.NativeTokens

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

data class DeviceChallenge(val ceremonyId: String, val challenge: String)
data class AccessGrant(val accessToken: String, val expiresIn: Int)
data class HostSummary(val id: String, val name: String, val online: Boolean)

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

    fun registrationOptions(
        binding: ActiveBinding,
        callback: (Result<DeviceChallenge>) -> Unit,
    ) {
        val body = JSONObject().put("bindingId", binding.bindingId)
        postJson(
            binding.server, "api/android/device/register/options", body,
            binding.clientSecret, callback,
        ) { json -> DeviceChallenge(json.getString("ceremonyId"), json.getString("challenge")) }
    }

    fun verifyRegistration(
        binding: ActiveBinding,
        ceremonyId: String,
        deviceId: String,
        publicKey: String,
        signature: String,
        callback: (Result<NativeTokens>) -> Unit,
    ) {
        val body = JSONObject()
            .put("bindingId", binding.bindingId)
            .put("ceremonyId", ceremonyId)
            .put("deviceId", deviceId)
            .put("publicKey", publicKey)
            .put("signature", signature)
        postJson(
            binding.server, "api/android/device/register/verify", body,
            binding.clientSecret, callback, ::parseTokens,
        )
    }

    fun loginOptions(deviceId: String, callback: (Result<DeviceChallenge>) -> Unit) {
        postJson(
            DEFAULT_SERVER,
            "api/android/device/login/options",
            JSONObject().put("deviceId", deviceId),
            null,
            callback,
        ) {
            DeviceChallenge(it.getString("ceremonyId"), it.getString("challenge"))
        }
    }

    fun verifyLogin(
        ceremonyId: String,
        deviceId: String,
        signature: String,
        callback: (Result<NativeTokens>) -> Unit,
    ) {
        val body = JSONObject()
            .put("ceremonyId", ceremonyId)
            .put("deviceId", deviceId)
            .put("signature", signature)
        postJson(
            DEFAULT_SERVER, "api/android/device/login/verify", body, null, callback, ::parseTokens,
        )
    }

    fun refresh(refreshToken: String, callback: (Result<AccessGrant>) -> Unit) {
        postJson(
            DEFAULT_SERVER,
            "api/client/token/refresh",
            JSONObject().put("refreshToken", refreshToken),
            null,
            callback,
        ) { json -> AccessGrant(json.getString("accessToken"), json.getInt("expiresIn")) }
    }

    fun hosts(accessToken: String, callback: (Result<List<HostSummary>>) -> Unit) {
        val request = Request.Builder()
            .url(DEFAULT_SERVER.newBuilder().addPathSegments("api/client/hosts").build())
            .header("Authorization", "Bearer $accessToken")
            .get()
            .build()
        httpClient.newCall(request).enqueue(jsonCallback(callback) { json ->
            val hosts = json.getJSONArray("hosts")
            List(hosts.length()) { index ->
                val host = hosts.getJSONObject(index)
                HostSummary(host.getString("id"), host.getString("name"), host.getBoolean("online"))
            }
        })
    }

    fun connectHost(
        hostId: String,
        accessToken: String,
        callback: (Result<String>) -> Unit,
    ) {
        val request = Request.Builder()
            .url(DEFAULT_SERVER.newBuilder()
                .addPathSegments("api/client/hosts")
                .addPathSegment(hostId)
                .addPathSegment("connect")
                .build())
            .header("Authorization", "Bearer $accessToken")
            .post(JSONObject().toString().toRequestBody(jsonType))
            .build()
        httpClient.newCall(request).enqueue(jsonCallback(callback) { it.getString("invite") })
    }

    fun logout(refreshToken: String, callback: (Result<Unit>) -> Unit) {
        postJson(
            DEFAULT_SERVER,
            "api/client/logout",
            JSONObject().put("refreshToken", refreshToken),
            null,
            callback,
        ) { Unit }
    }

    private fun <T> postJson(
        server: HttpUrl,
        path: String,
        body: JSONObject,
        bearer: String?,
        callback: (Result<T>) -> Unit,
        transform: (JSONObject) -> T,
    ) {
        val request = Request.Builder()
            .url(server.newBuilder().addPathSegments(path).build())
            .apply { if (bearer != null) header("Authorization", "Bearer $bearer") }
            .post(body.toString().toRequestBody(jsonType))
            .build()
        httpClient.newCall(request).enqueue(jsonCallback(callback, transform))
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

    private fun parseTokens(json: JSONObject) = NativeTokens(
        accessToken = json.getString("accessToken"),
        refreshToken = json.getString("refreshToken"),
        expiresIn = json.getInt("expiresIn"),
    )

    private fun createSecret(): String {
        val bytes = ByteArray(32).also(SecureRandom()::nextBytes)
        return Base64.encodeToString(bytes, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
    }

    companion object {
        val DEFAULT_SERVER: HttpUrl = "https://remoe.oza-oza.top/".toHttpUrl()
    }
}
