package top.ozaoza.remoe.input

import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import top.ozaoza.remoe.protocol.RemoteInputEvent

class RemoteTouchController(
    private val view: View,
    private val contentView: View,
    send: (RemoteInputEvent) -> Boolean,
    private val onPointerMoved: (TouchGestureEngine.Point) -> Unit = {},
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
                engine.pointerDown(event.points())
            }
            MotionEvent.ACTION_MOVE -> engine.move(event.points())
            MotionEvent.ACTION_POINTER_UP -> {
                engine.pointerUp(event.getPointerId(event.actionIndex), event.points(event.actionIndex))
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
        viewport.viewCoordinates(remoteX, remoteY)?.let(onPointerMoved)
    }

    private fun panViewport(deltaX: Float, deltaY: Float): Boolean =
        viewport.panBy(deltaX, deltaY).also { consumed ->
            if (consumed) engine.refreshPointer()
        }

    private fun zoomViewport(
        scale: Float,
        focus: TouchGestureEngine.Point,
        delta: TouchGestureEngine.Point,
    ) {
        viewport.zoomBy(scale, focus, delta)
        engine.refreshPointer()
    }

    private fun MotionEvent.point(index: Int) = TouchGestureEngine.Point(getX(index), getY(index))

    private fun MotionEvent.points(excludedIndex: Int = -1): Map<Int, TouchGestureEngine.Point> =
        buildMap {
            for (index in 0 until pointerCount) {
                if (index != excludedIndex) put(getPointerId(index), point(index))
            }
        }
}
