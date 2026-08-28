package top.ozaoza.remoe.signaling

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.fail
import org.junit.Test

class InviteParserTest {
    @Test
    fun parsesNativeAndBrowserInvitesWithoutExposingFragment() {
        val native = InviteParser.parse("wss://signal.example.com/signal#V1StGXR8_Z5jdHi6B-myT")
        assertEquals(
            "wss://signal.example.com/signal?session=V1StGXR8_Z5jdHi6B-myT&role=client",
            native.websocketUrl,
        )
        assertEquals("stun:signal.example.com:3478", native.stunUrl)
        assertFalse(native.websocketUrl.contains('#'))

        val browser = InviteParser.parse("https://signal.example.com/anything#abcdefgh")
        assertEquals(
            "wss://signal.example.com/signal?session=abcdefgh&role=client",
            browser.websocketUrl,
        )
    }

    @Test
    fun supportsIpv6StunSyntax() {
        val invite = InviteParser.parse("ws://[::1]:8080/signal#abcdefgh")
        assertEquals("ws://[::1]:8080/signal?session=abcdefgh&role=client", invite.websocketUrl)
        assertEquals("stun:[::1]:3478", invite.stunUrl)
    }

    @Test
    fun rejectsInvalidOrCredentialBearingInvites() {
        expectFailure("ftp://signal.example.com/signal#abcdefgh")
        expectFailure("wss://signal.example.com/signal#short")
        expectFailure("wss://user:secret@signal.example.com/signal#abcdefgh")
        expectFailure("not a url")
    }

    private fun expectFailure(value: String) {
        try {
            InviteParser.parse(value)
            fail("expected invalid invite: $value")
        } catch (_: IllegalArgumentException) {
        }
    }
}
