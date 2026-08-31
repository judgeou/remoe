package top.ozaoza.remoe.input

import kotlin.math.abs
import kotlin.math.hypot
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

/** Converts Android touch gestures into the Web client's trackpad-style pointer controls. */
class TouchGestureEngine(
    private val touchSlopPx: Float,
    private val emit: (RemoteInputEvent) -> Boolean,
    private val relativeCoordinates: (delta: Point, currentX: Int, currentY: Int) -> Pair<Int, Int>?,
    private val onPointerMoved: (x: Int, y: Int) -> Unit = { _, _ -> },
    private val panViewport: (deltaX: Float, deltaY: Float) -> Boolean = { _, _ -> false },
    private val zoomViewport: (
        scaleFactor: Float,
        focus: Point,
        focusDelta: Point,
    ) -> Unit = { _, _, _ -> },
    private val scrollScale: Float = 3f,
    private val tapDistancePx: Float = 18f,
    private val doubleTapDistancePx: Float = 28f,
    private val clockMs: () -> Long = { System.nanoTime() / 1_000_000L },
) {
    data class Point(val x: Float, val y: Float)

    private data class Tap(val timeMs: Long, val point: Point)
    private enum class Mode { IDLE, SINGLE, DRAG, MULTI }
    private enum class MultiMode { PAN, PINCH }

    private val pointers = mutableMapOf<Int, Point>()
    private var mode = Mode.IDLE
    private var primaryPointerId = -1
    private var startedAtMs = 0L
    private var start = Point(0f, 0f)
    private var gestureDistance = 0f
    private var maximumPointers = 0
    private var lastTap: Tap? = null
    private var leftPressed = false
    private var multiMode: MultiMode? = null
    private var multiStartCenter: Point? = null
    private var lastCenter: Point? = null
    private var startSpread = 0f
    private var lastSpread = 0f
    private var verticalWheelRemainder = 0f
    private var horizontalWheelRemainder = 0f
    private var mouseX = 32_768
    private var mouseY = 32_768

    init {
        require(touchSlopPx >= 0f)
        require(scrollScale > 0f)
        require(tapDistancePx > 0f)
        require(doubleTapDistancePx > 0f)
    }

    fun down(pointerId: Int, point: Point) {
        releaseLeft()
        resetGesture()
        pointers[pointerId] = point
        primaryPointerId = pointerId
        startedAtMs = clockMs()
        start = point
        maximumPointers = 1
        val previousTap = lastTap
        if (previousTap != null && startedAtMs - previousTap.timeMs in 0 until DOUBLE_TAP_TIMEOUT_MS &&
            distance(previousTap.point, point) < doubleTapDistancePx
        ) {
            pressLeft()
            mode = Mode.DRAG
            lastTap = null
        } else {
            mode = Mode.SINGLE
        }
    }

    fun pointerDown(activePointers: Map<Int, Point>) {
        pointers.clear()
        pointers.putAll(activePointers)
        if (pointers.size < 2) return
        releaseLeft()
        lastTap = null
        mode = Mode.MULTI
        maximumPointers = maxOf(maximumPointers, 2)
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
        val previousPointers = pointers.toMap()
        gestureDistance += activePointers.entries.sumOf { (id, point) ->
            previousPointers[id]?.let { distance(it, point).toDouble() } ?: 0.0
        }.toFloat()
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
                val pinchDistance = abs(spread - startSpread)
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
        val previous = previousPointers[primaryPointerId] ?: point
        moveRelative(Point(point.x - previous.x, point.y - previous.y))
    }

    fun pointerUp(pointerId: Int, remainingPointers: Map<Int, Point>) {
        pointers.remove(pointerId)
        pointers.clear()
        pointers.putAll(remainingPointers)
        if (mode == Mode.MULTI && pointers.isNotEmpty()) lastCenter = center(pointers.values)
    }

    fun up(pointerId: Int, point: Point) {
        if (pointerId != primaryPointerId && mode != Mode.MULTI) return
        val now = clockMs()
        val isTap = now - startedAtMs in 0 until TAP_TIMEOUT_MS &&
            gestureDistance < tapDistancePx
        when (mode) {
            Mode.SINGLE -> if (isTap) {
                click(InputType.MOUSE_LEFT)
                lastTap = Tap(now, start)
            } else {
                lastTap = null
            }
            Mode.DRAG -> {
                releaseLeft()
                lastTap = null
            }
            Mode.MULTI -> {
                if (isTap && maximumPointers == 2 && multiMode == null) {
                    click(InputType.MOUSE_RIGHT)
                }
                lastTap = null
            }
            Mode.IDLE -> Unit
        }
        resetGesture()
    }

    fun cancel() {
        releaseLeft()
        lastTap = null
        resetGesture()
    }

    fun resetPointer() {
        mouseX = 32_768
        mouseY = 32_768
        onPointerMoved(mouseX, mouseY)
    }

    fun refreshPointer() = onPointerMoved(mouseX, mouseY)

    fun syncPointer() {
        emit(RemoteInputEvent(InputType.MOUSE_MOVE, value1 = mouseX, value2 = mouseY))
        onPointerMoved(mouseX, mouseY)
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

    private fun moveRelative(delta: Point) {
        if (delta.x == 0f && delta.y == 0f) return
        val (x, y) = relativeCoordinates(delta, mouseX, mouseY) ?: return
        if (x == mouseX && y == mouseY) return
        mouseX = x
        mouseY = y
        emit(RemoteInputEvent(InputType.MOUSE_MOVE, value1 = x, value2 = y))
        onPointerMoved(x, y)
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
        startedAtMs = 0L
        gestureDistance = 0f
        maximumPointers = 0
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

    private fun distance(left: Point, right: Point): Float = hypot(right.x - left.x, right.y - left.y)

    private fun spread(points: Collection<Point>): Float {
        val values = points.take(2)
        return if (values.size < 2) 0f else distance(values[0], values[1])
    }

    private companion object {
        const val TAP_TIMEOUT_MS = 500L
        const val DOUBLE_TAP_TIMEOUT_MS = 350L
    }
}
