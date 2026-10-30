package top.ozaoza.remoe.input

import android.view.KeyEvent
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

data class WindowsKey(val scanCode: Int, val extended: Boolean = false)

object AndroidKeyMap {
    // Android's scanCode is a Linux code, so it cannot be sent as a Windows code.
    fun windowsKey(keyCode: Int): WindowsKey? = keys[keyCode]

    private val keys = buildMap {
        fun key(android: Int, scan: Int, extended: Boolean = false) {
            put(android, WindowsKey(scan, extended))
        }
        key(KeyEvent.KEYCODE_ESCAPE, 0x01)
        val digits = intArrayOf(0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a)
        digits.forEachIndexed { digit, scan -> key(KeyEvent.KEYCODE_0 + digit, scan) }
        val letters = intArrayOf(
            0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
        )
        letters.forEachIndexed { letter, scan -> key(KeyEvent.KEYCODE_A + letter, scan) }
        key(KeyEvent.KEYCODE_MINUS, 0x0c)
        key(KeyEvent.KEYCODE_EQUALS, 0x0d)
        key(KeyEvent.KEYCODE_DEL, 0x0e)
        key(KeyEvent.KEYCODE_TAB, 0x0f)
        key(KeyEvent.KEYCODE_LEFT_BRACKET, 0x1a)
        key(KeyEvent.KEYCODE_RIGHT_BRACKET, 0x1b)
        key(KeyEvent.KEYCODE_ENTER, 0x1c)
        key(KeyEvent.KEYCODE_CTRL_LEFT, 0x1d)
        key(KeyEvent.KEYCODE_SEMICOLON, 0x27)
        key(KeyEvent.KEYCODE_APOSTROPHE, 0x28)
        key(KeyEvent.KEYCODE_GRAVE, 0x29)
        key(KeyEvent.KEYCODE_SHIFT_LEFT, 0x2a)
        key(KeyEvent.KEYCODE_BACKSLASH, 0x2b)
        key(KeyEvent.KEYCODE_COMMA, 0x33)
        key(KeyEvent.KEYCODE_PERIOD, 0x34)
        key(KeyEvent.KEYCODE_SLASH, 0x35)
        key(KeyEvent.KEYCODE_SHIFT_RIGHT, 0x36)
        key(KeyEvent.KEYCODE_NUMPAD_MULTIPLY, 0x37)
        key(KeyEvent.KEYCODE_ALT_LEFT, 0x38)
        key(KeyEvent.KEYCODE_SPACE, 0x39)
        key(KeyEvent.KEYCODE_CAPS_LOCK, 0x3a)
        for (index in 0..9) key(KeyEvent.KEYCODE_F1 + index, 0x3b + index)
        key(KeyEvent.KEYCODE_F11, 0x57)
        key(KeyEvent.KEYCODE_F12, 0x58)
        key(KeyEvent.KEYCODE_NUM_LOCK, 0x45)
        key(KeyEvent.KEYCODE_SCROLL_LOCK, 0x46)
        val numpad = intArrayOf(0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49)
        numpad.forEachIndexed { digit, scan -> key(KeyEvent.KEYCODE_NUMPAD_0 + digit, scan) }
        key(KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 0x4a)
        key(KeyEvent.KEYCODE_NUMPAD_ADD, 0x4e)
        key(KeyEvent.KEYCODE_NUMPAD_DOT, 0x53)
        key(KeyEvent.KEYCODE_NUMPAD_ENTER, 0x1c, true)
        key(KeyEvent.KEYCODE_CTRL_RIGHT, 0x1d, true)
        key(KeyEvent.KEYCODE_NUMPAD_DIVIDE, 0x35, true)
        key(KeyEvent.KEYCODE_SYSRQ, 0x37, true)
        key(KeyEvent.KEYCODE_ALT_RIGHT, 0x38, true)
        key(KeyEvent.KEYCODE_MOVE_HOME, 0x47, true)
        key(KeyEvent.KEYCODE_DPAD_UP, 0x48, true)
        key(KeyEvent.KEYCODE_PAGE_UP, 0x49, true)
        key(KeyEvent.KEYCODE_DPAD_LEFT, 0x4b, true)
        key(KeyEvent.KEYCODE_DPAD_RIGHT, 0x4d, true)
        key(KeyEvent.KEYCODE_MOVE_END, 0x4f, true)
        key(KeyEvent.KEYCODE_DPAD_DOWN, 0x50, true)
        key(KeyEvent.KEYCODE_PAGE_DOWN, 0x51, true)
        key(KeyEvent.KEYCODE_INSERT, 0x52, true)
        key(KeyEvent.KEYCODE_FORWARD_DEL, 0x53, true)
        key(KeyEvent.KEYCODE_META_LEFT, 0x5b, true)
        key(KeyEvent.KEYCODE_META_RIGHT, 0x5c, true)
        key(KeyEvent.KEYCODE_MENU, 0x5d, true)
    }
}

class RemoteKeyboardController(private val send: (RemoteInputEvent) -> Boolean) {
    private val pressed = linkedMapOf<Int, WindowsKey>()

    fun keyDown(keyCode: Int, repeatCount: Int = 0): Boolean {
        val key = AndroidKeyMap.windowsKey(keyCode) ?: return false
        if (keyCode !in pressed || repeatCount > 0) {
            if (emit(key, release = false)) pressed[keyCode] = key
        }
        return true
    }

    fun keyUp(keyCode: Int): Boolean {
        if (AndroidKeyMap.windowsKey(keyCode) == null) return false
        val key = pressed[keyCode] ?: return true
        if (emit(key, release = true)) pressed.remove(keyCode)
        return true
    }

    fun tap(keyCode: Int): Boolean {
        val key = AndroidKeyMap.windowsKey(keyCode) ?: return false
        if (keyCode in pressed) return false
        if (!emit(key, release = false)) return false
        pressed[keyCode] = key
        val sent = emit(key, release = true)
        if (sent) pressed.remove(keyCode)
        return sent
    }

    fun isPressed(keyCode: Int): Boolean = keyCode in pressed

    // A latched Ctrl/Alt/Win plus a soft-keyboard letter is a shortcut, not a
    // Unicode character. Only this explicit shortcut path maps text to keys.
    fun shortcutText(text: String): Boolean? {
        val shortcutHeld = listOf(
            KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT,
            KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_ALT_RIGHT,
            KeyEvent.KEYCODE_META_LEFT, KeyEvent.KEYCODE_META_RIGHT,
        ).any(::isPressed)
        if (!shortcutHeld) return null
        val codes = text.map { character ->
            when (val lower = character.lowercaseChar()) {
                in 'a'..'z' -> KeyEvent.KEYCODE_A + (lower - 'a')
                in '0'..'9' -> KeyEvent.KEYCODE_0 + (lower - '0')
                ' ' -> KeyEvent.KEYCODE_SPACE
                else -> return false
            }
        }
        return codes.all(::tap)
    }

    fun releaseAll() {
        // Release ordinary keys before their modifiers, in reverse press order.
        pressed.values.toList().asReversed().forEach { emit(it, release = true) }
        pressed.clear()
    }

    private fun emit(key: WindowsKey, release: Boolean): Boolean = send(RemoteInputEvent(
        type = InputType.KEYBOARD,
        flags = (if (release) Protocol.INPUT_FLAG_RELEASE else 0) or
            (if (key.extended) Protocol.INPUT_FLAG_EXTENDED_KEY else 0),
        value1 = key.scanCode,
    ))
}
