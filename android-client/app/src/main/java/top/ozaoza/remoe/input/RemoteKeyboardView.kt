package top.ozaoza.remoe.input

import android.content.Context
import android.text.InputType
import android.text.Editable
import android.text.Selection
import android.view.KeyEvent
import android.view.KeyCharacterMap
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager

// A focused remote editor. Only composing text lives on the phone; committed
// text is sent once, then forgotten so IMEs cannot rewrite remote history.
class RemoteKeyboardView(context: Context) : View(context) {
    var sendText: (String) -> Boolean = { false }
    var sendKey: (KeyEvent) -> Boolean = { false }
    var tapKey: (Int) -> Boolean = { false }
    private var connection: RemoteConnection? = null
    private var generation = 0

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        importantForAutofill = IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
        contentDescription = "远程键盘输入"
    }

    override fun onCheckIsTextEditor(): Boolean = isFocusable

    override fun dispatchKeyEventPreIme(event: KeyEvent): Boolean {
        if (event.deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD &&
            event.flags and KeyEvent.FLAG_SOFT_KEYBOARD == 0 && sendKey(event)
        ) return true
        return super.dispatchKeyEventPreIme(event)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        sendKey(event) || super.onKeyDown(keyCode, event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        sendKey(event) || super.onKeyUp(keyCode, event)

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!isFocusable) return null
        connection?.discard()
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or EditorInfo.IME_ACTION_NONE
        outAttrs.initialSelStart = 0
        outAttrs.initialSelEnd = 0
        return RemoteConnection(++generation).also { connection = it }
    }

    fun discardComposition() {
        generation++
        connection?.discard()
        connection = null
    }

    fun finishComposition(): Boolean = connection?.finishComposingText() ?: true

    private inner class RemoteConnection(private val id: Int) : BaseInputConnection(this@RemoteKeyboardView, true) {
        private val active: Boolean get() = id == generation && isFocused && isAttachedToWindow
        private var batchDepth = 0
        private var selectionDirty = false

        override fun getEditable(): Editable = requireNotNull(super.getEditable())

        init {
            Selection.setSelection(editable, 0)
        }

        override fun beginBatchEdit(): Boolean {
            if (!active) return false
            batchDepth++
            return true
        }

        override fun endBatchEdit(): Boolean {
            if (batchDepth == 0) return false
            batchDepth--
            if (batchDepth == 0 && selectionDirty) updateSelection()
            return batchDepth > 0
        }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
            if (!active) return false
            val accepted = super.setComposingText(text, newCursorPosition)
            updateSelection()
            return accepted
        }

        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
            if (!active) return false
            super.commitText(text, newCursorPosition)
            return flushText()
        }

        override fun finishComposingText(): Boolean {
            if (!active) return false
            super.finishComposingText()
            return flushText()
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean =
            deleteText(beforeLength, afterLength, codePoints = false)

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean =
            deleteText(beforeLength, afterLength, codePoints = true)

        private fun deleteText(before: Int, after: Int, codePoints: Boolean): Boolean {
            if (!active || before !in 0..256 || after !in 0..256) return false
            val text = editable
            val start = minOf(Selection.getSelectionStart(text), Selection.getSelectionEnd(text)).coerceAtLeast(0)
            val end = maxOf(Selection.getSelectionStart(text), Selection.getSelectionEnd(text)).coerceAtLeast(start)
            val localBefore = if (codePoints) Character.codePointCount(text, 0, start) else start
            val localAfter = if (codePoints) Character.codePointCount(text, end, text.length) else text.length - end
            var deleteStart = if (codePoints) {
                Character.offsetByCodePoints(text, start, -minOf(before, localBefore))
            } else (start - before).coerceAtLeast(0)
            var deleteEnd = if (codePoints) {
                Character.offsetByCodePoints(text, end, minOf(after, localAfter))
            } else minOf(end + after, text.length)
            // Even old IMEs that request UTF-16 units must not leave half an Emoji.
            if (deleteStart in 1 until text.length &&
                Character.isHighSurrogate(text[deleteStart - 1]) && Character.isLowSurrogate(text[deleteStart])
            ) deleteStart--
            if (deleteEnd in 1 until text.length &&
                Character.isHighSurrogate(text[deleteEnd - 1]) && Character.isLowSurrogate(text[deleteEnd])
            ) deleteEnd++
            text.delete(end, deleteEnd)
            text.delete(deleteStart, start)
            Selection.setSelection(text, deleteStart, end - (start - deleteStart))
            updateSelection()
            var accepted = true
            repeat((before - localBefore).coerceAtLeast(0)) {
                accepted = tapKey(KeyEvent.KEYCODE_DEL) && accepted
            }
            repeat((after - localAfter).coerceAtLeast(0)) {
                accepted = tapKey(KeyEvent.KEYCODE_FORWARD_DEL) && accepted
            }
            return accepted
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            if (!active) return false
            if (event.keyCode == KeyEvent.KEYCODE_DEL && editable.isNotEmpty()) {
                return event.action != KeyEvent.ACTION_DOWN || deleteText(1, 0, codePoints = true)
            }
            if (event.action == KeyEvent.ACTION_DOWN && !finishComposingText()) return false
            return sendKey(event)
        }

        override fun performEditorAction(actionCode: Int): Boolean =
            active && finishComposingText() && tapKey(KeyEvent.KEYCODE_ENTER)

        override fun closeConnection() {
            // Closing or replacing an IME connection must not commit stale text.
            discard()
            if (id == generation) generation++
            super.closeConnection()
        }

        private fun flushText(): Boolean {
            val text = editable.toString()
            discard()
            return text.isEmpty() || sendText(text)
        }

        fun discard() {
            removeComposingSpans(editable)
            editable.clear()
            Selection.setSelection(editable, 0)
            updateSelection()
        }

        private fun updateSelection() {
            selectionDirty = true
            if (batchDepth == 0 && id == generation && isAttachedToWindow) {
                selectionDirty = false
                context.getSystemService(InputMethodManager::class.java)
                    .updateSelection(
                        this@RemoteKeyboardView,
                        Selection.getSelectionStart(editable), Selection.getSelectionEnd(editable),
                        getComposingSpanStart(editable), getComposingSpanEnd(editable),
                    )
            }
        }
    }
}
