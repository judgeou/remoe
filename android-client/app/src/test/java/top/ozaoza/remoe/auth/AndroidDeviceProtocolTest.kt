package top.ozaoza.remoe.auth

import org.junit.Assert.assertEquals
import org.junit.Test

class AndroidDeviceProtocolTest {
    @Test
    fun registrationMessageIsCanonical() {
        assertEquals(
            "remoe-android-device-register-v1\nceremony\nchallenge\nbinding\ndevice\npublic",
            AndroidDeviceProtocol.registrationMessage(
                "ceremony", "challenge", "binding", "device", "public",
            ).decodeToString(),
        )
    }

    @Test
    fun loginMessageIsCanonical() {
        assertEquals(
            "remoe-android-device-login-v1\nceremony\nchallenge\ndevice",
            AndroidDeviceProtocol.loginMessage("ceremony", "challenge", "device").decodeToString(),
        )
    }
}
