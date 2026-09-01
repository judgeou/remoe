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
    private val relativeCoordinates: (
        deltaX: Float,
        deltaY: Float,
        currentX: Int,
        currentY: Int,
        result: IntArray,
    ) -> Boolean,
    private val onPointerMoved: (x: Int, y: Int) -> Unit = { _, _ -> },
    private val panViewport: (deltaX: Float, deltaY: Float) -> Boolean = { _, _ -> false },
    private val zoomViewport: (
        scaleFactor: Float,
        focusX: Float,
        focusY: Float,
        deltaX: Float,
        deltaY: Float,
    ) -> Unit = { _, _, _, _, _ -> },
    private val scrollScale: Float = 3f,
    private val tapDistancePx: Float = 18f,
    private val doubleTapDistancePx: Float = 28f,
    private val clockMs: () -> Long = { System.nanoTime() / 1_000_000L },
) {
    data class Point(val x: Float, val y: Float)

    private data class Tap(val timeMs: Long, val point: Point)
    private data class PointerValues(
        val firstId: Int,
        val firstX: Float,
        val firstY: Float,
        val secondId: Int,
        val secondX: Float,
        val secondY: Float,
    )
    private enum class Mode { IDLE, SINGLE, DRAG, MULTI }
    private enum class MultiMode { PAN, PINCH }

    private var pointerCount = 0
    private var firstPointerId = -1
    private var firstPointerX = 0f
    private var firstPointerY = 0f
    private var secondPointerId = -1
    private var secondPointerX = 0f
    private var secondPointerY = 0f
    private var mode = Mode.IDLE
    private var primaryPointerId = -1
    private var startedAtMs = 0L
    private var start = Point(0f, 0f)
    private var gestureDistance = 0f
    private var maximumPointers = 0
    private var lastTap: Tap? = null
    private var leftPressed = false
    private var multiMode: MultiMode? = null
    private var multiStartCenterX = 0f
    private var multiStartCenterY = 0f
    private var lastCenterX = 0f
    private var lastCenterY = 0f
    private var startSpread = 0f
    private var lastSpread = 0f
    private var verticalWheelRemainder = 0f
    private var horizontalWheelRemainder = 0f
    private var mouseX = 32_768
    private var mouseY = 32_768
    private val coordinateResult = IntArray(2)

    init {
        require(touchSlopPx >= 0f)
        require(scrollScale > 0f)
        require(tapDistancePx > 0f)
        require(doubleTapDistancePx > 0f)
    }

    fun down(pointerId: Int, point: Point) {
        releaseLeft()
        resetGesture()
        setPointers(pointerId, point.x, point.y)
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
        val values = firstTwo(activePointers)
        pointerDown(
            values.firstId,
            values.firstX,
            values.firstY,
            values.secondId,
            values.secondX,
            values.secondY,
            activePointers.size,
        )
    }

    fun pointerDown(
        firstId: Int,
        firstX: Float,
        firstY: Float,
        secondId: Int,
        secondX: Float,
        secondY: Float,
        activeCount: Int,
    ) {
        setPointers(firstId, firstX, firstY, secondId, secondX, secondY, activeCount)
        if (pointerCount < 2) return
        releaseLeft()
        lastTap = null
        mode = Mode.MULTI
        maximumPointers = maxOf(maximumPointers, activeCount)
        multiMode = null
        multiStartCenterX = centerX()
        multiStartCenterY = centerY()
        lastCenterX = multiStartCenterX
        lastCenterY = multiStartCenterY
        startSpread = spread()
        lastSpread = startSpread
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    fun move(activePointers: Map<Int, Point>) {
        val values = firstTwo(activePointers)
        move(
            values.firstId,
            values.firstX,
            values.firstY,
            values.secondId,
            values.secondX,
            values.secondY,
            activePointers.size,
        )
    }

    fun move(
        firstId: Int,
        firstX: Float,
        firstY: Float,
        secondId: Int = -1,
        secondX: Float = 0f,
        secondY: Float = 0f,
        activeCount: Int = 1,
    ) {
        if (mode == Mode.IDLE) return
        val previousFirstId = firstPointerId
        val previousFirstX = firstPointerX
        val previousFirstY = firstPointerY
        val previousSecondId = secondPointerId
        val previousSecondX = secondPointerX
        val previousSecondY = secondPointerY
        gestureDistance += movementDistance(
            firstId, firstX, firstY,
            previousFirstId, previousFirstX, previousFirstY,
            previousSecondId, previousSecondX, previousSecondY,
        )
        if (activeCount > 1) {
            gestureDistance += movementDistance(
                secondId, secondX, secondY,
                previousFirstId, previousFirstX, previousFirstY,
                previousSecondId, previousSecondX, previousSecondY,
            )
        }
        setPointers(firstId, firstX, firstY, secondId, secondX, secondY, activeCount)

        if (mode == Mode.MULTI) {
            if (pointerCount < 2) return
            val centerX = centerX()
            val centerY = centerY()
            val spread = spread()
            var deltaX = centerX - lastCenterX
            var deltaY = centerY - lastCenterY
            var scaleFactor = if (lastSpread > 0f) spread / lastSpread else 1f
            if (multiMode == null) {
                val panDistance = hypot(centerX - multiStartCenterX, centerY - multiStartCenterY)
                val pinchDistance = abs(spread - startSpread)
                if (maxOf(panDistance, pinchDistance) >= touchSlopPx) {
                    multiMode = if (pinchDistance > panDistance) MultiMode.PINCH else MultiMode.PAN
                    deltaX = centerX - multiStartCenterX
                    deltaY = centerY - multiStartCenterY
                    scaleFactor = if (startSpread > 0f) spread / startSpread else 1f
                }
            }
            when (multiMode) {
                MultiMode.PINCH -> zoomViewport(scaleFactor, centerX, centerY, deltaX, deltaY)
                MultiMode.PAN -> if (!panViewport(deltaX, deltaY)) {
                    horizontalWheelRemainder = sendWheel(
                        InputType.MOUSE_HORIZONTAL_WHEEL,
                        horizontalWheelRemainder + deltaX * scrollScale,
                    )
                    verticalWheelRemainder = sendWheel(
                        InputType.MOUSE_WHEEL,
                        verticalWheelRemainder + deltaY * scrollScale,
                    )
                }
                null -> Unit
            }
            lastCenterX = centerX
            lastCenterY = centerY
            lastSpread = spread
            return
        }

        val currentX: Float
        val currentY: Float
        when (primaryPointerId) {
            firstPointerId -> {
                currentX = firstPointerX
                currentY = firstPointerY
            }
            secondPointerId -> {
                currentX = secondPointerX
                currentY = secondPointerY
            }
            else -> return
        }
        val previousX: Float
        val previousY: Float
        when (primaryPointerId) {
            previousFirstId -> {
                previousX = previousFirstX
                previousY = previousFirstY
            }
            previousSecondId -> {
                previousX = previousSecondX
                previousY = previousSecondY
            }
            else -> {
                previousX = currentX
                previousY = currentY
            }
        }
        moveRelative(currentX - previousX, currentY - previousY)
    }

    fun pointerUp(pointerId: Int, remainingPointers: Map<Int, Point>) {
        val values = firstTwo(remainingPointers)
        pointerUp(
            pointerId,
            values.firstId,
            values.firstX,
            values.firstY,
            values.secondId,
            values.secondX,
            values.secondY,
            remainingPointers.size,
        )
    }

    fun pointerUp(
        pointerId: Int,
        firstId: Int,
        firstX: Float,
        firstY: Float,
        secondId: Int = -1,
        secondX: Float = 0f,
        secondY: Float = 0f,
        activeCount: Int = 1,
    ) {
        setPointers(firstId, firstX, firstY, secondId, secondX, secondY, activeCount)
        if (mode == Mode.MULTI && pointerCount > 0) {
            lastCenterX = if (pointerCount > 1) centerX() else firstPointerX
            lastCenterY = if (pointerCount > 1) centerY() else firstPointerY
        }
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

    private fun moveRelative(deltaX: Float, deltaY: Float) {
        if (deltaX == 0f && deltaY == 0f) return
        if (!relativeCoordinates(deltaX, deltaY, mouseX, mouseY, coordinateResult)) return
        val x = coordinateResult[0]
        val y = coordinateResult[1]
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
        setPointers(-1, 0f, 0f, activeCount = 0)
        mode = Mode.IDLE
        primaryPointerId = -1
        startedAtMs = 0L
        gestureDistance = 0f
        maximumPointers = 0
        multiMode = null
        multiStartCenterX = 0f
        multiStartCenterY = 0f
        lastCenterX = 0f
        lastCenterY = 0f
        startSpread = 0f
        lastSpread = 0f
        verticalWheelRemainder = 0f
        horizontalWheelRemainder = 0f
    }

    private fun distance(left: Point, right: Point): Float = hypot(right.x - left.x, right.y - left.y)

    private fun setPointers(
        firstId: Int,
        firstX: Float,
        firstY: Float,
        secondId: Int = -1,
        secondX: Float = 0f,
        secondY: Float = 0f,
        activeCount: Int = 1,
    ) {
        pointerCount = activeCount
        firstPointerId = firstId
        firstPointerX = firstX
        firstPointerY = firstY
        secondPointerId = if (activeCount > 1) secondId else -1
        secondPointerX = if (activeCount > 1) secondX else 0f
        secondPointerY = if (activeCount > 1) secondY else 0f
    }

    private fun firstTwo(pointers: Map<Int, Point>): PointerValues {
        var firstId = -1
        var firstX = 0f
        var firstY = 0f
        var secondId = -1
        var secondX = 0f
        var secondY = 0f
        for ((id, point) in pointers) {
            if (firstId < 0) {
                firstId = id
                firstX = point.x
                firstY = point.y
            } else if (secondId < 0) {
                secondId = id
                secondX = point.x
                secondY = point.y
                break
            }
        }
        return PointerValues(firstId, firstX, firstY, secondId, secondX, secondY)
    }

    private fun movementDistance(
        id: Int,
        x: Float,
        y: Float,
        previousFirstId: Int,
        previousFirstX: Float,
        previousFirstY: Float,
        previousSecondId: Int,
        previousSecondX: Float,
        previousSecondY: Float,
    ): Float = when (id) {
        previousFirstId -> hypot(x - previousFirstX, y - previousFirstY)
        previousSecondId -> hypot(x - previousSecondX, y - previousSecondY)
        else -> 0f
    }

    private fun centerX(): Float = (firstPointerX + secondPointerX) / 2f

    private fun centerY(): Float = (firstPointerY + secondPointerY) / 2f

    private fun spread(): Float = if (pointerCount < 2) 0f else
        hypot(secondPointerX - firstPointerX, secondPointerY - firstPointerY)

    private companion object {
        const val TAP_TIMEOUT_MS = 500L
        const val DOUBLE_TAP_TIMEOUT_MS = 350L
    }
}
