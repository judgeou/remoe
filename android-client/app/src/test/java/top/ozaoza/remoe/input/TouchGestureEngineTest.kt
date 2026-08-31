package top.ozaoza.remoe.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import top.ozaoza.remoe.protocol.InputType
import top.ozaoza.remoe.protocol.Protocol
import top.ozaoza.remoe.protocol.RemoteInputEvent

class TouchGestureEngineTest {
    private val events = mutableListOf<RemoteInputEvent>()
    private val viewportPans = mutableListOf<TouchGestureEngine.Point>()
    private val viewportZooms = mutableListOf<Float>()
    private var viewportConsumesPan = false
    private var contentWidth = 1001
    private var contentHeight = 501
    private val engine = TouchGestureEngine(
        touchSlopPx = 8f,
        emit = {
            events += it
            true
        },
        absoluteCoordinates = { point ->
            normalize(point.x, contentWidth) to normalize(point.y, contentHeight)
        },
        panViewport = { x, y ->
            viewportPans += point(x, y)
            viewportConsumesPan
        },
        zoomViewport = { scale, _, _ -> viewportZooms += scale },
    )

    @Test
    fun tapMovesAbsolutelyAndClicksLeft() {
        engine.down(4, point(500f, 250f))
        engine.up(4, point(500f, 250f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_MOVE, value1 = 32_767, value2 = 32_767),
                event(InputType.MOUSE_LEFT),
                event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
            ),
            events,
        )
    }

    @Test
    fun movementBeyondSlopStartsDragAndCancelReleasesIt() {
        engine.down(2, point(100f, 100f))
        engine.move(mapOf(2 to point(120f, 110f)))
        engine.cancel()

        assertEquals(InputType.MOUSE_MOVE, events[0].type)
        assertEquals(event(InputType.MOUSE_LEFT), events[1])
        assertEquals(InputType.MOUSE_MOVE, events[2].type)
        assertEquals(
            event(InputType.MOUSE_LEFT, flags = Protocol.INPUT_FLAG_RELEASE),
            events[3],
        )
    }

    @Test
    fun stationaryLongPressClicksRightWithoutLeftClick() {
        contentWidth = 101
        contentHeight = 101
        engine.down(9, point(40f, 60f))
        assertTrue(engine.longPress(9))
        engine.up(9, point(40f, 60f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_MOVE, value1 = 26_214, value2 = 39_321),
                event(InputType.MOUSE_RIGHT),
                event(InputType.MOUSE_RIGHT, flags = Protocol.INPUT_FLAG_RELEASE),
            ),
            events,
        )
        assertFalse(engine.longPress(9))
    }

    @Test
    fun stableTwoFingerPanSendsVerticalAndHorizontalWheelOnly() {
        contentWidth = 101
        contentHeight = 101
        engine.down(3, point(10f, 10f))
        engine.pointerDown(mapOf(3 to point(10f, 10f), 8 to point(30f, 30f)))
        engine.move(mapOf(8 to point(40f, 50f), 3 to point(20f, 30f)))
        engine.pointerUp(8, mapOf(3 to point(20f, 30f)))
        engine.up(3, point(20f, 30f))

        assertEquals(
            listOf(
                event(InputType.MOUSE_MOVE, value1 = 6_553, value2 = 6_553),
                event(InputType.MOUSE_HORIZONTAL_WHEEL, value1 = 30),
                event(InputType.MOUSE_WHEEL, value1 = 60),
            ),
            events,
        )
    }

    @Test
    fun twoFingerSpreadSelectsPinchWithoutSendingWheel() {
        contentWidth = 101
        contentHeight = 101
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
        contentWidth = 101
        contentHeight = 101
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
    fun coordinatesAreClampedToProtocolRange() {
        contentWidth = 200
        contentHeight = 100
        engine.down(1, point(-20f, 500f))
        engine.up(1, point(500f, -10f))

        assertEquals(event(InputType.MOUSE_MOVE, value1 = 0, value2 = 65_535), events[0])
        assertEquals(event(InputType.MOUSE_MOVE, value1 = 65_535, value2 = 0), events[1])
    }

    private fun point(x: Float, y: Float) = TouchGestureEngine.Point(x, y)

    private fun normalize(value: Float, size: Int): Int =
        (value.coerceIn(0f, (size - 1).toFloat()) * 65_535f / (size - 1)).toInt()

    private fun event(
        type: InputType,
        flags: Int = 0,
        value1: Int = 0,
        value2: Int = 0,
    ) = RemoteInputEvent(type, flags, value1, value2)
}
