package top.ozaoza.remoe.input

import android.app.Activity
import android.app.Application
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class)
class RemoteKeyboardViewTest {
    private val activityController = Robolectric.buildActivity(Activity::class.java)
    private val sent = mutableListOf<String>()
    private val keys = mutableListOf<Int>()
    private lateinit var view: RemoteKeyboardView
    private lateinit var connection: InputConnection

    @Before
    fun setUp() {
        val activity = activityController.setup().visible().get()
        view = RemoteKeyboardView(activity).apply {
            sendText = { sent.add(it) }
            tapKey = { keys.add(it) }
            sendKey = { keys.add(it.keyCode) }
        }
        activity.setContentView(view)
        assertTrue(view.requestFocus())
        connection = requireNotNull(view.onCreateInputConnection(EditorInfo()))
    }

    @After
    fun tearDown() { activityController.pause().stop().destroy() }

    @Test
    fun chineseCompositionAndEmojiAreCommittedExactlyOnce() {
        connection.setComposingText("n", 1)
        connection.setComposingText("ni", 1)
        assertTrue(sent.isEmpty())
        assertTrue(connection.commitText("你🚀", 1))
        connection.finishComposingText()
        assertEquals(listOf("你🚀"), sent)
        assertEquals("", connection.getTextBeforeCursor(20, 0).toString())
    }

    @Test
    fun finishingCompositionCommitsAndLocalBackspaceDoesNotDeleteRemoteText() {
        connection.setComposingText("你好🚀", 1)
        connection.deleteSurroundingTextInCodePoints(1, 0)
        connection.finishComposingText()
        assertEquals(listOf("你好"), sent)
        assertTrue(keys.isEmpty())
        connection.deleteSurroundingText(2, 1)
        assertEquals(listOf(KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_FORWARD_DEL), keys)
    }

    @Test
    fun editorEnterCommitsCompositionBeforeSendingEnter() {
        connection.setComposingText("hello", 1)
        assertTrue(connection.performEditorAction(EditorInfo.IME_ACTION_DONE))
        assertEquals(listOf("hello"), sent)
        assertEquals(listOf(KeyEvent.KEYCODE_ENTER), keys)
    }

    @Test
    fun utf16BackspaceAlsoPreservesEmojiBoundaries() {
        connection.setComposingText("a🚀", 1)
        connection.deleteSurroundingText(1, 0)
        connection.finishComposingText()
        assertEquals(listOf("a"), sent)
        assertTrue(keys.isEmpty())
    }

    @Test
    fun hardwareKeysCanBypassLocalIme() {
        val event = KeyEvent(1, 1, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, 0, 0, 2, 30)
        assertTrue(view.dispatchKeyEventPreIme(event))
        assertEquals(listOf(KeyEvent.KEYCODE_A), keys)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun replacedOrCancelledConnectionsCannotTypeStaleText() {
        connection.setComposingText("stale", 1)
        val replacement = requireNotNull(view.onCreateInputConnection(EditorInfo()))
        assertFalse(connection.commitText("old", 1))
        replacement.commitText("new", 1)
        replacement.setComposingText("discard", 1)
        view.discardComposition()
        assertFalse(replacement.finishComposingText())
        assertFalse(replacement.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)))
        assertEquals(listOf("new"), sent)
        assertTrue(keys.isEmpty())
    }
}
