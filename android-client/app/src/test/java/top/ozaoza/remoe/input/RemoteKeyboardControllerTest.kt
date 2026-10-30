package top.ozaoza.remoe.input

import android.view.KeyEvent
import org.junit.Assert.*
import org.junit.Test
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

class RemoteKeyboardControllerTest {
    private val events = mutableListOf<RemoteInputEvent>()
    private val keyboard = RemoteKeyboardController { events.add(it) }

    @Test
    fun distinguishesExtendedKeysFromNumpadAndLeftModifiers() {
        assertEquals(WindowsKey(0x1d), AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_CTRL_LEFT))
        assertEquals(WindowsKey(0x1d, true), AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_CTRL_RIGHT))
        assertEquals(WindowsKey(0x4b, true), AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_DPAD_LEFT))
        assertEquals(WindowsKey(0x4b), AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_NUMPAD_4))
        assertNull(AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_BACK))
        assertNull(AndroidKeyMap.windowsKey(KeyEvent.KEYCODE_VOLUME_UP))
    }

    @Test
    fun shortcutsAndAutoRepeatKeepBalancedPresses() {
        keyboard.keyDown(KeyEvent.KEYCODE_CTRL_RIGHT)
        keyboard.keyDown(KeyEvent.KEYCODE_C)
        keyboard.keyDown(KeyEvent.KEYCODE_C) // Duplicate, not a repeat.
        keyboard.keyDown(KeyEvent.KEYCODE_C, repeatCount = 1)
        keyboard.keyUp(KeyEvent.KEYCODE_C)
        keyboard.keyUp(KeyEvent.KEYCODE_C)
        keyboard.keyUp(KeyEvent.KEYCODE_CTRL_RIGHT)
        assertEquals(listOf(0x1d, 0x2e, 0x2e, 0x2e, 0x1d), events.map { it.value1 })
        assertEquals(listOf(2, 0, 0, 1, 3), events.map { it.flags })
    }

    @Test
    fun cancelReleasesAllKeysOnceInReverseOrder() {
        keyboard.keyDown(KeyEvent.KEYCODE_SHIFT_LEFT)
        keyboard.keyDown(KeyEvent.KEYCODE_DPAD_LEFT)
        keyboard.releaseAll()
        keyboard.releaseAll()
        assertEquals(listOf(0x2a, 0x4b, 0x4b, 0x2a), events.map { it.value1 })
        assertEquals(listOf(0, 2, 3, 1), events.map { it.flags })
        assertFalse(keyboard.isPressed(KeyEvent.KEYCODE_SHIFT_LEFT))
    }

    @Test
    fun latchedControlAndSoftLetterSendARealShortcut() {
        assertNull(keyboard.shortcutText("中文"))
        keyboard.keyDown(KeyEvent.KEYCODE_CTRL_LEFT)
        assertEquals(true, keyboard.shortcutText("c"))
        assertEquals(false, keyboard.shortcutText("中文"))
        assertEquals(listOf(0x1d, 0x2e, 0x2e), events.map { it.value1 })
        assertEquals(Protocol.INPUT_FLAG_RELEASE, events.last().flags)
        assertTrue(keyboard.isPressed(KeyEvent.KEYCODE_CTRL_LEFT))
    }

    @Test
    fun failedTapReleaseIsRetriedOnCancel() {
        var accept = true
        val controller = RemoteKeyboardController {
            events += it
            accept.also { accept = false }
        }
        assertFalse(controller.tap(KeyEvent.KEYCODE_ENTER))
        assertTrue(controller.isPressed(KeyEvent.KEYCODE_ENTER))
        accept = true
        controller.releaseAll()
        assertEquals(listOf(0, 1, 1), events.map { it.flags })
    }
}
