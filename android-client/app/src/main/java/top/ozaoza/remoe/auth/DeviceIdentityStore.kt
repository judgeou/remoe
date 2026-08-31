package top.ozaoza.remoe.auth

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.nio.charset.StandardCharsets
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.ECGenParameterSpec

data class DeviceIdentity(
    val deviceId: String,
    val publicKey: String,
)

object AndroidDeviceProtocol {
    fun registrationMessage(
        ceremonyId: String,
        challenge: String,
        bindingId: String,
        deviceId: String,
        publicKey: String,
    ): ByteArray = message(
        "register", ceremonyId, challenge, bindingId, deviceId, publicKey,
    )

    fun loginMessage(
        ceremonyId: String,
        challenge: String,
        deviceId: String,
    ): ByteArray = message("login", ceremonyId, challenge, deviceId)

    private fun message(type: String, vararg fields: String): ByteArray =
        (listOf("remoe-android-device-$type-v1") + fields)
            .joinToString("\n")
            .toByteArray(StandardCharsets.UTF_8)
}

class DeviceIdentityStore(context: Context) {
    private val preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
    private val keyStore = KeyStore.getInstance(KEY_STORE).apply { load(null) }

    @Synchronized
    fun current(): DeviceIdentity? {
        val deviceId = preferences.getString(DEVICE_ID, null) ?: return null
        val certificate = keyStore.getCertificate(KEY_ALIAS) ?: return null
        return DeviceIdentity(deviceId, base64Url(certificate.publicKey.encoded))
    }

    @Synchronized
    fun getOrCreate(): DeviceIdentity {
        current()?.let { return it }
        keyStore.deleteEntry(KEY_ALIAS)
        val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, KEY_STORE)
        generator.initialize(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_SIGN)
                .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                .setDigests(KeyProperties.DIGEST_SHA256)
                .build(),
        )
        val pair = generator.generateKeyPair()
        val deviceId = base64Url(ByteArray(24).also(SecureRandom()::nextBytes))
        preferences.edit().putString(DEVICE_ID, deviceId).apply()
        return DeviceIdentity(deviceId, base64Url(pair.public.encoded))
    }

    @Synchronized
    fun sign(message: ByteArray): String {
        val privateKey = keyStore.getKey(KEY_ALIAS, null)
            ?: throw IllegalStateException("本机设备密钥不存在，请重新扫码绑定")
        val signature = Signature.getInstance("SHA256withECDSA")
        signature.initSign(privateKey as java.security.PrivateKey)
        signature.update(message)
        return base64Url(signature.sign())
    }

    private fun base64Url(bytes: ByteArray): String = Base64.encodeToString(
        bytes, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING,
    )

    companion object {
        private const val KEY_STORE = "AndroidKeyStore"
        private const val KEY_ALIAS = "remoe_android_device_identity_v1"
        private const val PREFERENCES = "remoe_device_identity"
        private const val DEVICE_ID = "device_id"
    }
}
