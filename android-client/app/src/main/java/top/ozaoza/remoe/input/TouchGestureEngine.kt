package top.ozaoza.remoe.input

import kotlin.math.hypot
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

/** Converts stable Android pointer IDs into protocol-v11 absolute mouse events. */
class TouchGestureEngine(
    private val touchSlopPx: Float,
    private val emit: (RemoteInputEvent) -> Boolean,
    private val absoluteCoordinates: (Point) -> Pair<Int, Int>?,
    private val panViewport: (deltaX: Float, deltaY: Float) -> Boolean = { _, _ -> false },
    private val zoomViewport: (
        scaleFactor: Float,
        focus: Point,
        focusDelta: Point,
    ) -> Unit = { _, _, _ -> },
    private val scrollScale: Float = 3f,
) {
    data class Point(val x: Float, val y: Float)

    private enum class Mode { IDLE, SINGLE, DRAG, LONG_PRESS, MULTI }
    private enum class MultiMode { PAN, PINCH }

    private val pointers = mutableMapOf<Int, Point>()
    private var mode = Mode.IDLE
    private var primaryPointerId = -1
    private var start = Point(0f, 0f)
    private var leftPressed = false
    private var multiMode: MultiMode? = null
    private var multiStartCenter: Point? = null
    private var lastCenter: Point? = null
    private var startSpread = 0f
    private var lastSpread = 0f
    private var verticalWheelRemainder = 0f
    private var horizontalWheelRemainder = 0f
    private var lastMouseX = -1
    private var lastMouseY = -1

    init {
        require(touchSlopPx >= 0f)
        require(scrollScale > 0f)
    }

    fun down(pointerId: Int, point: Point) {
        cancel()
        pointers[pointerId] = point
        primaryPointerId = pointerId
        start = point
        mode = Mode.SINGLE
        moveAbsolute(point)
    }

    fun pointerDown(activePointers: Map<Int, Point>) {
        pointers.clear()
        pointers.putAll(activePointers)
        if (pointers.size < 2) return
        releaseLeft()
        mode = Mode.MULTI
        multiMode = null
        multiStartCenter = center(pointers.values)
        lastCenter = multiStartCenter
        startSpread = spread(pointers.values)
        lastSpread = startSpread
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    fun move(activePointers: Map<Int, Point>) {
        if (mode == Mode.IDLE) return
        pointers.clear()
        pointers.putAll(activePointers)
        if (mode == Mode.MULTI) {
            if (pointers.size < 2) return
            val center = center(pointers.values)
            val spread = spread(pointers.values)
            val previousCenter = lastCenter ?: center
            var delta = Point(center.x - previousCenter.x, center.y - previousCenter.y)
            var scaleFactor = if (lastSpread > 0f) spread / lastSpread else 1f
            if (multiMode == null) {
                val initialCenter = multiStartCenter ?: center
                val panDistance = distance(initialCenter, center)
                val pinchDistance = kotlin.math.abs(spread - startSpread)
                if (maxOf(panDistance, pinchDistance) >= touchSlopPx) {
                    multiMode = if (pinchDistance > panDistance) MultiMode.PINCH else MultiMode.PAN
                    delta = Point(center.x - initialCenter.x, center.y - initialCenter.y)
                    scaleFactor = if (startSpread > 0f) spread / startSpread else 1f
                }
            }
            when (multiMode) {
                MultiMode.PINCH -> zoomViewport(scaleFactor, center, delta)
                MultiMode.PAN -> if (!panViewport(delta.x, delta.y)) {
                    horizontalWheelRemainder = sendWheel(
                        InputType.MOUSE_HORIZONTAL_WHEEL,
                        horizontalWheelRemainder + delta.x * scrollScale,
                    )
                    verticalWheelRemainder = sendWheel(
                        InputType.MOUSE_WHEEL,
                        verticalWheelRemainder + delta.y * scrollScale,
                    )
                }
                null -> Unit
            }
            lastCenter = center
            lastSpread = spread
            return
        }

        val point = pointers[primaryPointerId] ?: return
        if (mode == Mode.SINGLE && distance(start, point) > touchSlopPx) {
            pressLeft()
            mode = Mode.DRAG
        }
        moveAbsolute(point)
    }

    fun pointerUp(pointerId: Int, remainingPointers: Map<Int, Point>) {
        pointers.remove(pointerId)
        pointers.clear()
        pointers.putAll(remainingPointers)
        if (mode != Mode.MULTI) return
        if (pointers.isEmpty()) resetGesture()
        else lastCenter = center(pointers.values)
    }

    fun up(pointerId: Int, point: Point) {
        if (pointerId != primaryPointerId && mode != Mode.MULTI) return
        when (mode) {
            Mode.SINGLE -> {
                moveAbsolute(point)
                click(InputType.MOUSE_LEFT)
            }
            Mode.DRAG -> {
                moveAbsolute(point)
                releaseLeft()
            }
            Mode.LONG_PRESS -> moveAbsolute(point)
            Mode.MULTI, Mode.IDLE -> Unit
        }
        resetGesture()
    }

    fun longPress(pointerId: Int): Boolean {
        if (mode != Mode.SINGLE || pointerId != primaryPointerId || pointers.size != 1) return false
        val point = pointers[pointerId] ?: return false
        if (distance(start, point) > touchSlopPx) return false
        click(InputType.MOUSE_RIGHT)
        mode = Mode.LONG_PRESS
        return true
    }

    fun cancel() {
        releaseLeft()
        resetGesture()
    }

    private fun pressLeft() {
        if (leftPressed) return
        emit(RemoteInputEvent(InputType.MOUSE_LEFT))
        leftPressed = true
    }

    private fun releaseLeft() {
        if (!leftPressed) return
        emit(RemoteInputEvent(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE))
        leftPressed = false
    }

    private fun click(type: InputType) {
        emit(RemoteInputEvent(type))
        emit(RemoteInputEvent(type, flags = Protocol.INPUT_FLAG_RELEASE))
    }

    private fun moveAbsolute(point: Point) {
        val (x, y) = absoluteCoordinates(point) ?: return
        if (x == lastMouseX && y == lastMouseY) return
        lastMouseX = x
        lastMouseY = y
        emit(RemoteInputEvent(InputType.MOUSE_MOVE, value1 = x, value2 = y))
    }

    private fun sendWheel(type: InputType, accumulated: Float): Float {
        val integral = accumulated.toInt()
        if (integral == 0) return accumulated
        val amount = integral.coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
        emit(RemoteInputEvent(type, value1 = amount))
        return accumulated - amount
    }

    private fun resetGesture() {
        pointers.clear()
        mode = Mode.IDLE
        primaryPointerId = -1
        multiMode = null
        multiStartCenter = null
        lastCenter = null
        startSpread = 0f
        lastSpread = 0f
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    private fun center(points: Collection<Point>): Point = Point(
        points.sumOf { it.x.toDouble() }.toFloat() / points.size,
        points.sumOf { it.y.toDouble() }.toFloat() / points.size,
    )

    private fun distance(left: Point, right: Point): Float =
        hypot(right.x - left.x, right.y - left.y)

    private fun spread(points: Collection<Point>): Float {
        val values = points.take(2)
        return if (values.size < 2) 0f else distance(values[0], values[1])
    }
}
