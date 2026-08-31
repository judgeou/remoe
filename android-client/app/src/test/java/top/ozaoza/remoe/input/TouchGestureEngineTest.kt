package top.ozaoza.remoe.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

class TouchGestureEngineTest {
    private val events = mutableListOf<RemoteInputEvent>()
    private val pointerPositions = mutableListOf<Pair<Int, Int>>()
    private val viewportPans = mutableListOf<TouchGestureEngine.Point>()
    private val viewportZooms = mutableListOf<Float>()
    private var viewportConsumesPan = false
    private var nowMs = 0L
    private var contentWidth = 1000f
    private var contentHeight = 500f
    private val engine = TouchGestureEngine(
        touchSlopPx = 8f,
        emit = {
            events += it
            true
        },
        relativeCoordinates = { delta, currentX, currentY ->
            (currentX + delta.x * 65_535f / contentWidth).toInt().coerceIn(0, 65_535) to
                (currentY + delta.y * 65_535f / contentHeight).toInt().coerceIn(0, 65_535)
        },
        onPointerMoved = { x, y -> pointerPositions += x to y },
        panViewport = { x, y ->
            viewportPans += point(x, y)
            viewportConsumesPan
        },
        zoomViewport = { scale, _, _ -> viewportZooms += scale },
        clockMs = { nowMs },
    )

    @Test
    fun tapClicksAtCurrentPointerWithoutJumpingToFinger() {
        engine.down(4, point(800f, 400f))
        engine.up(4, point(800f, 400f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_LEFT),
                event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
            ),
            events,
        )
        assertTrue(pointerPositions.isEmpty())
    }

    @Test
    fun singleFingerMotionMovesPointerRelativelyWithoutDragging() {
        engine.down(2, point(100f, 100f))
        engine.move(mapOf(2 to point(200f, 150f)))
        engine.up(2, point(200f, 150f))

        assertEquals(
            listOf(event(InputType.MOUSE_MOVE, value1 = 39_321, value2 = 39_321)),
            events,
        )
        assertEquals(listOf(39_321 to 39_321), pointerPositions)
    }

    @Test
    fun doubleTapThenMoveDragsAndFingerUpReleasesLeft() {
        engine.down(2, point(100f, 100f))
        engine.up(2, point(100f, 100f))
        nowMs = 120
        engine.down(2, point(105f, 104f))
        engine.move(mapOf(2 to point(125f, 114f)))
        engine.up(2, point(125f, 114f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_LEFT),
                event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
                event(InputType.MOUSE_LEFT),
                event(InputType.MOUSE_MOVE, value1 = 34_078, value2 = 34_078),
                event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
            ),
            events,
        )
    }

    @Test
    fun cancelDuringDoubleTapDragAlsoReleasesLeft() {
        engine.down(2, point(100f, 100f))
        engine.up(2, point(100f, 100f))
        nowMs = 120
        engine.down(2, point(105f, 104f))
        engine.cancel()

        assertEquals(
            event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
            events.last(),
        )
    }

    @Test
    fun twoFingerTapClicksRight() {
        engine.down(3, point(10f, 10f))
        engine.pointerDown(mapOf(3 to point(10f, 10f), 8 to point(30f, 30f)))
        engine.pointerUp(8, mapOf(3 to point(10f, 10f)))
        engine.up(3, point(10f, 10f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_RIGHT),
                event(InputType.MOUSE_RIGHT, flags = Protocol.INPUT_FLAG_RELEASE),
            ),
            events,
        )
    }

    @Test
    fun stableTwoFingerPanSendsVerticalAndHorizontalWheelOnly() {
        engine.down(3, point(10f, 10f))
        engine.pointerDown(mapOf(3 to point(10f, 10f), 8 to point(30f, 30f)))
        engine.move(mapOf(8 to point(40f, 50f), 3 to point(20f, 30f)))
        engine.pointerUp(8, mapOf(3 to point(20f, 30f)))
        engine.up(3, point(20f, 30f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_HORIZONTAL_WHEEL, value1 = 30),
                event(InputType.MOUSE_WHEEL, value1 = 60),
            ),
            events,
        )
    }

    @Test
    fun twoFingerSpreadSelectsPinchWithoutSendingWheel() {
        engine.down(3, point(40f, 50f))
        engine.pointerDown(mapOf(3 to point(40f, 50f), 8 to point(60f, 50f)))
        engine.move(mapOf(3 to point(30f, 50f), 8 to point(70f, 50f)))

        assertEquals(listOf(2f), viewportZooms)
        assertTrue(events.none {
            it.type == InputType.MOUSE_WHEEL || it.type == InputType.MOUSE_HORIZONTAL_WHEEL
        })
    }

    @Test
    fun zoomedViewportConsumesTwoFingerPanInsteadOfWheel() {
        viewportConsumesPan = true
        engine.down(3, point(10f, 10f))
        engine.pointerDown(mapOf(3 to point(10f, 10f), 8 to point(30f, 30f)))
        engine.move(mapOf(3 to point(20f, 30f), 8 to point(40f, 50f)))

        assertEquals(listOf(point(10f, 20f)), viewportPans)
        assertTrue(events.none {
            it.type == InputType.MOUSE_WHEEL || it.type == InputType.MOUSE_HORIZONTAL_WHEEL
        })
    }

    @Test
    fun relativePointerCoordinatesAreClampedToProtocolRange() {
        engine.down(1, point(0f, 0f))
        engine.move(mapOf(1 to point(5_000f, 5_000f)))
        engine.move(mapOf(1 to point(-5_000f, -5_000f)))

        assertEquals(event(InputType.MOUSE_MOVE, value1 = 65_535, value2 = 65_535), events[0])
        assertEquals(event(InputType.MOUSE_MOVE, value1 = 0, value2 = 0), events[1])
    }

    @Test
    fun resetPointerReturnsVisibleCursorToCenterWithoutSendingRemoteInput() {
        engine.resetPointer()

        assertEquals(listOf(32_768 to 32_768), pointerPositions)
        assertTrue(events.isEmpty())
    }

    @Test
    fun syncPointerAlignsRemoteCursorWithVisibleCursor() {
        engine.syncPointer()

        assertEquals(event(InputType.MOUSE_MOVE, value1 = 32_768, value2 = 32_768), events.single())
        assertEquals(listOf(32_768 to 32_768), pointerPositions)
    }

    private fun point(x: Float, y: Float) = TouchGestureEngine.Point(x, y)

    private fun event(
        type: InputType,
        flags: Int = 0,
        value1: Int = 0,
        value2: Int = 0,
    ) = RemoteInputEvent(type, flags, value1, value2)
}
