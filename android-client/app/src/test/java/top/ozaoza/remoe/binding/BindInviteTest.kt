package top.ozaoza.remoe.binding

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class BindInviteTest {
    private val token = "abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJKLMN1234"

    @Test
    fun parsesVersionedHttpsInvite() {
        val invite = BindInviteParser.parse(
            "remoe://bind?v=1&server=https%3A%2F%2Fremoe.example&token=$token",
        )
        assertEquals("https://remoe.example/", invite.server.toString())
        assertEquals(token, invite.token)
    }

    @Test
    fun rejectsCleartextAndForeignSchemes() {
        assertThrows(IllegalArgumentException::class.java) {
            BindInviteParser.parse(
                "remoe://bind?v=1&server=http%3A%2F%2Fremoe.example&token=$token",
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            BindInviteParser.parse("https://remoe.example/?token=$token")
        }
    }
}
