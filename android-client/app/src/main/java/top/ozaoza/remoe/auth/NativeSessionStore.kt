package top.ozaoza.remoe.auth

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

data class NativeTokens(
    val accessToken: String,
    val refreshToken: String,
    val expiresIn: Int,
)

class NativeSessionStore(context: Context) {
    private val preferences = context.getSharedPreferences("native_session", Context.MODE_PRIVATE)
    var accessToken: String? = null
        private set
    var accessExpiresAt: Long = 0
        private set

    fun save(tokens: NativeTokens) {
        updateAccess(tokens.accessToken, tokens.expiresIn)
        preferences.edit().putString(REFRESH_KEY, encrypt(tokens.refreshToken)).apply()
    }

    fun updateAccess(token: String, expiresIn: Int) {
        accessToken = token
        accessExpiresAt = System.currentTimeMillis() + expiresIn * 1_000L
    }

    fun refreshToken(): String? = preferences.getString(REFRESH_KEY, null)?.let {
        runCatching { decrypt(it) }.getOrNull()
    }

    fun clear() {
        accessToken = null
        accessExpiresAt = 0
        preferences.edit().remove(REFRESH_KEY).apply()
    }

    private fun encrypt(value: String): String {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        val payload = cipher.iv + cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        return Base64.encodeToString(payload, Base64.NO_WRAP)
    }

    private fun decrypt(value: String): String {
        val payload = Base64.decode(value, Base64.NO_WRAP)
        require(payload.size > IV_BYTES)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(128, payload, 0, IV_BYTES))
        return cipher.doFinal(payload, IV_BYTES, payload.size - IV_BYTES).toString(Charsets.UTF_8)
    }

    private fun secretKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            ).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build())
            generateKey()
        }
    }

    companion object {
        private const val KEY_ALIAS = "remoe_native_refresh_v1"
        private const val REFRESH_KEY = "refresh_ciphertext_v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val IV_BYTES = 12
    }
}
