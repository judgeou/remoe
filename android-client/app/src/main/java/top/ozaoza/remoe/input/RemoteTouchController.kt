package top.ozaoza.remoe.input

import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import top.ozaoza.remoe.protocol.RemoteInputEvent

class RemoteTouchController(
    private val view: View,
    contentView: View,
    send: (RemoteInputEvent) -> Boolean,
) : View.OnTouchListener {
    private val handler = Handler(Looper.getMainLooper())
    private val viewport = RemoteViewportController(view, contentView)
    private val engine = TouchGestureEngine(
        ViewConfiguration.get(view.context).scaledTouchSlop.toFloat(),
        send,
        viewport::remoteCoordinates,
        viewport::panBy,
        viewport::zoomBy,
    )
    private var longPressPointerId = MotionEvent.INVALID_POINTER_ID
    private var gestureAccepted = false
    private val longPress = Runnable {
        val pointerId = longPressPointerId
        if (pointerId != MotionEvent.INVALID_POINTER_ID) engine.longPress(pointerId)
    }

    init {
        view.isClickable = true
        view.setOnTouchListener(this)
    }

    override fun onTouch(touchedView: View, event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val index = event.actionIndex
                val pointerId = event.getPointerId(index)
                val point = event.point(index)
                if (viewport.remoteCoordinates(point) == null) {
                    gestureAccepted = false
                    return false
                }
                gestureAccepted = true
                engine.down(pointerId, point)
                scheduleLongPress(pointerId)
            }
            else -> if (!gestureAccepted) return false
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN -> {
                clearLongPress()
                engine.pointerDown(event.points())
            }
            MotionEvent.ACTION_MOVE -> engine.move(event.points())
            MotionEvent.ACTION_POINTER_UP -> {
                clearLongPress()
                engine.pointerUp(event.getPointerId(event.actionIndex), event.points(event.actionIndex))
            }
            MotionEvent.ACTION_UP -> {
                clearLongPress()
                val index = event.actionIndex
                engine.up(
                    event.getPointerId(index),
                    event.point(index),
                )
                touchedView.performClick()
                gestureAccepted = false
            }
            MotionEvent.ACTION_CANCEL, MotionEvent.ACTION_OUTSIDE -> cancel()
            else -> Unit
        }
        return true
    }

    fun cancel() {
        clearLongPress()
        engine.cancel()
        gestureAccepted = false
    }

    fun resetViewport() = viewport.reset()

    fun dispose() {
        cancel()
        view.setOnTouchListener(null)
    }

    private fun scheduleLongPress(pointerId: Int) {
        clearLongPress()
        longPressPointerId = pointerId
        handler.postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
    }

    private fun clearLongPress() {
        handler.removeCallbacks(longPress)
        longPressPointerId = MotionEvent.INVALID_POINTER_ID
    }

    private fun MotionEvent.point(index: Int) = TouchGestureEngine.Point(getX(index), getY(index))

    private fun MotionEvent.points(excludedIndex: Int = -1): Map<Int, TouchGestureEngine.Point> =
        buildMap {
            for (index in 0 until pointerCount) {
                if (index != excludedIndex) put(getPointerId(index), point(index))
            }
        }
}
