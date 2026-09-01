package top.ozaoza.remoe.input

import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import top.ozaoza.remoe.protocol.RemoteInputEvent

class RemoteTouchController(
    private val view: View,
    private val contentView: View,
    send: (RemoteInputEvent) -> Boolean,
    private val onPointerMoved: (Float, Float) -> Unit = { _, _ -> },
) : View.OnTouchListener {
    private val viewport = RemoteViewportController(view, contentView)
    private val engine = TouchGestureEngine(
        touchSlopPx = ViewConfiguration.get(view.context).scaledTouchSlop.toFloat(),
        emit = send,
        relativeCoordinates = viewport::moveRemotePointer,
        onPointerMoved = ::positionPointer,
        panViewport = ::panViewport,
        zoomViewport = ::zoomViewport,
        tapDistancePx = 18f * view.resources.displayMetrics.density,
        doubleTapDistancePx = 28f * view.resources.displayMetrics.density,
    )
    private var gestureAccepted = false
    private val layoutChangeListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
        engine.refreshPointer()
    }

    init {
        view.isClickable = true
        view.setOnTouchListener(this)
        view.addOnLayoutChangeListener(layoutChangeListener)
        contentView.addOnLayoutChangeListener(layoutChangeListener)
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
            }
            else -> if (!gestureAccepted) return false
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN -> {
                pointerDown(event)
            }
            MotionEvent.ACTION_MOVE -> move(event)
            MotionEvent.ACTION_POINTER_UP -> {
                pointerUp(event)
            }
            MotionEvent.ACTION_UP -> {
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
        engine.cancel()
        gestureAccepted = false
    }

    fun resetViewport() {
        viewport.reset()
        engine.resetPointer()
    }

    fun refreshPointerPosition() = engine.refreshPointer()

    fun syncPointerPosition() = engine.syncPointer()

    fun dispose() {
        cancel()
        view.setOnTouchListener(null)
        view.removeOnLayoutChangeListener(layoutChangeListener)
        contentView.removeOnLayoutChangeListener(layoutChangeListener)
    }

    private fun positionPointer(remoteX: Int, remoteY: Int) {
        viewport.viewCoordinates(remoteX, remoteY, onPointerMoved)
    }

    private fun panViewport(deltaX: Float, deltaY: Float): Boolean =
        viewport.panBy(deltaX, deltaY).also { consumed ->
            if (consumed) engine.refreshPointer()
        }

    private fun zoomViewport(
        scale: Float,
        focusX: Float,
        focusY: Float,
        deltaX: Float,
        deltaY: Float,
    ) {
        viewport.zoomBy(scale, focusX, focusY, deltaX, deltaY)
        engine.refreshPointer()
    }

    private fun MotionEvent.point(index: Int) = TouchGestureEngine.Point(getX(index), getY(index))

    private fun pointerDown(event: MotionEvent) {
        val secondIndex = if (event.pointerCount > 1) 1 else -1
        engine.pointerDown(
            event.getPointerId(0),
            event.getX(0),
            event.getY(0),
            if (secondIndex >= 0) event.getPointerId(secondIndex) else -1,
            if (secondIndex >= 0) event.getX(secondIndex) else 0f,
            if (secondIndex >= 0) event.getY(secondIndex) else 0f,
            event.pointerCount,
        )
    }

    private fun move(event: MotionEvent) {
        val secondIndex = if (event.pointerCount > 1) 1 else -1
        engine.move(
            event.getPointerId(0),
            event.getX(0),
            event.getY(0),
            if (secondIndex >= 0) event.getPointerId(secondIndex) else -1,
            if (secondIndex >= 0) event.getX(secondIndex) else 0f,
            if (secondIndex >= 0) event.getY(secondIndex) else 0f,
            event.pointerCount,
        )
    }

    private fun pointerUp(event: MotionEvent) {
        val removedIndex = event.actionIndex
        var firstIndex = -1
        var secondIndex = -1
        for (index in 0 until event.pointerCount) {
            if (index == removedIndex) continue
            if (firstIndex < 0) {
                firstIndex = index
            } else if (secondIndex < 0) {
                secondIndex = index
                break
            }
        }
        val remainingCount = event.pointerCount - 1
        engine.pointerUp(
            event.getPointerId(removedIndex),
            if (firstIndex >= 0) event.getPointerId(firstIndex) else -1,
            if (firstIndex >= 0) event.getX(firstIndex) else 0f,
            if (firstIndex >= 0) event.getY(firstIndex) else 0f,
            if (secondIndex >= 0) event.getPointerId(secondIndex) else -1,
            if (secondIndex >= 0) event.getX(secondIndex) else 0f,
            if (secondIndex >= 0) event.getY(secondIndex) else 0f,
            remainingCount,
        )
    }
}
