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

data class PasskeyOptions(val ceremonyId: String, val optionsJson: String)
data class AccessGrant(val accessToken: String, val expiresIn: Int)

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
        callback: (Result<PasskeyOptions>) -> Unit,
    ) {
        val body = JSONObject().put("bindingId", binding.bindingId)
        postJson(
            binding.server, "api/android/passkey/register/options", body,
            binding.clientSecret, callback,
        ) { json -> PasskeyOptions(json.getString("ceremonyId"), json.getJSONObject("options").toString()) }
    }

    fun verifyRegistration(
        binding: ActiveBinding,
        ceremonyId: String,
        credentialJson: String,
        callback: (Result<NativeTokens>) -> Unit,
    ) {
        val body = JSONObject()
            .put("bindingId", binding.bindingId)
            .put("ceremonyId", ceremonyId)
            .put("credential", JSONObject(credentialJson))
        postJson(
            binding.server, "api/android/passkey/register/verify", body,
            binding.clientSecret, callback, ::parseTokens,
        )
    }

    fun loginOptions(callback: (Result<PasskeyOptions>) -> Unit) {
        postJson(DEFAULT_SERVER, "api/android/passkey/login/options", JSONObject(), null, callback) {
            PasskeyOptions(it.getString("ceremonyId"), it.getJSONObject("options").toString())
        }
    }

    fun verifyLogin(
        ceremonyId: String,
        credentialJson: String,
        callback: (Result<NativeTokens>) -> Unit,
    ) {
        val body = JSONObject()
            .put("ceremonyId", ceremonyId)
            .put("credential", JSONObject(credentialJson))
        postJson(
            DEFAULT_SERVER, "api/android/passkey/login/verify", body, null, callback, ::parseTokens,
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
