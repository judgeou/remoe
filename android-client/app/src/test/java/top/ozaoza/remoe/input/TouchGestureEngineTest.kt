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
    private val engine = TouchGestureEngine(touchSlopPx = 8f, emit = {
        events += it
        true
    })

    @Test
    fun tapMovesAbsolutelyAndClicksLeft() {
        engine.down(4, point(500f, 250f), 1001, 501)
        engine.up(4, point(500f, 250f), 1001, 501)

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
        engine.down(2, point(100f, 100f), 1001, 501)
        engine.move(mapOf(2 to point(120f, 110f)), 1001, 501)
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
        engine.down(9, point(40f, 60f), 101, 101)
        assertTrue(engine.longPress(9))
        engine.up(9, point(40f, 60f), 101, 101)

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
        engine.down(3, point(10f, 10f), 101, 101)
        engine.pointerDown(mapOf(3 to point(10f, 10f), 8 to point(30f, 30f)))
        engine.move(mapOf(8 to point(40f, 50f), 3 to point(20f, 30f)), 101, 101)
        engine.pointerUp(8, mapOf(3 to point(20f, 30f)))
        engine.up(3, point(20f, 30f), 101, 101)

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
    fun coordinatesAreClampedToProtocolRange() {
        engine.down(1, point(-20f, 500f), 200, 100)
        engine.up(1, point(500f, -10f), 200, 100)

        assertEquals(event(InputType.MOUSE_MOVE, value1 = 0, value2 = 65_535), events[0])
        assertEquals(event(InputType.MOUSE_MOVE, value1 = 65_535, value2 = 0), events[1])
    }

    private fun point(x: Float, y: Float) = TouchGestureEngine.Point(x, y)

    private fun event(
        type: InputType,
        flags: Int = 0,
        value1: Int = 0,
        value2: Int = 0,
    ) = RemoteInputEvent(type, flags, value1, value2)
}
