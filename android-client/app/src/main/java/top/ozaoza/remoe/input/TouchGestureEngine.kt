package top.ozaoza.remoe.input

import kotlin.math.hypot
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

/** Converts stable Android pointer IDs into protocol-v11 absolute mouse events. */
class TouchGestureEngine(
    private val touchSlopPx: Float,
    private val emit: (RemoteInputEvent) -> Boolean,
    private val scrollScale: Float = 3f,
) {
    data class Point(val x: Float, val y: Float)

    private enum class Mode { IDLE, SINGLE, DRAG, LONG_PRESS, MULTI }

    private val pointers = mutableMapOf<Int, Point>()
    private var mode = Mode.IDLE
    private var primaryPointerId = -1
    private var start = Point(0f, 0f)
    private var leftPressed = false
    private var lastCenter: Point? = null
    private var verticalWheelRemainder = 0f
    private var horizontalWheelRemainder = 0f
    private var lastMouseX = -1
    private var lastMouseY = -1

    init {
        require(touchSlopPx >= 0f)
        require(scrollScale > 0f)
    }

    fun down(pointerId: Int, point: Point, width: Int, height: Int) {
        cancel()
        pointers[pointerId] = point
        primaryPointerId = pointerId
        start = point
        mode = Mode.SINGLE
        moveAbsolute(point, width, height)
    }

    fun pointerDown(activePointers: Map<Int, Point>) {
        pointers.clear()
        pointers.putAll(activePointers)
        if (pointers.size < 2) return
        releaseLeft()
        mode = Mode.MULTI
        lastCenter = center(pointers.values)
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    fun move(activePointers: Map<Int, Point>, width: Int, height: Int) {
        if (mode == Mode.IDLE) return
        pointers.clear()
        pointers.putAll(activePointers)
        if (mode == Mode.MULTI) {
            if (pointers.size < 2) return
            val center = center(pointers.values)
            lastCenter?.let { previous ->
                horizontalWheelRemainder = sendWheel(
                    InputType.MOUSE_HORIZONTAL_WHEEL,
                    horizontalWheelRemainder + (center.x - previous.x) * scrollScale,
                )
                verticalWheelRemainder = sendWheel(
                    InputType.MOUSE_WHEEL,
                    verticalWheelRemainder + (center.y - previous.y) * scrollScale,
                )
            }
            lastCenter = center
            return
        }

        val point = pointers[primaryPointerId] ?: return
        if (mode == Mode.SINGLE && distance(start, point) > touchSlopPx) {
            pressLeft()
            mode = Mode.DRAG
        }
        moveAbsolute(point, width, height)
    }

    fun pointerUp(pointerId: Int, remainingPointers: Map<Int, Point>) {
        pointers.remove(pointerId)
        pointers.clear()
        pointers.putAll(remainingPointers)
        if (mode != Mode.MULTI) return
        if (pointers.isEmpty()) resetGesture()
        else lastCenter = center(pointers.values)
    }

    fun up(pointerId: Int, point: Point, width: Int, height: Int) {
        if (pointerId != primaryPointerId && mode != Mode.MULTI) return
        when (mode) {
            Mode.SINGLE -> {
                moveAbsolute(point, width, height)
                click(InputType.MOUSE_LEFT)
            }
            Mode.DRAG -> {
                moveAbsolute(point, width, height)
                releaseLeft()
            }
            Mode.LONG_PRESS -> moveAbsolute(point, width, height)
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

    private fun moveAbsolute(point: Point, width: Int, height: Int) {
        if (width <= 1 || height <= 1) return
        val x = normalize(point.x, width)
        val y = normalize(point.y, height)
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
        lastCenter = null
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    private fun normalize(value: Float, size: Int): Int =
        (value.coerceIn(0f, (size - 1).toFloat()) * 65_535f / (size - 1)).toInt()

    private fun center(points: Collection<Point>): Point = Point(
        points.sumOf { it.x.toDouble() }.toFloat() / points.size,
        points.sumOf { it.y.toDouble() }.toFloat() / points.size,
    )

    private fun distance(left: Point, right: Point): Float =
        hypot(right.x - left.x, right.y - left.y)
}
