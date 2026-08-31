package top.ozaoza.remoe.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ViewportTransformTest {
    @Test
    fun fitViewportRejectsLetterboxAndMapsVideoCenter() {
        val transform = ViewportTransform()

        assertNull(transform.mapToContent(point(100f, 600f), 2261, 1272, 2800, 1272))
        val center = transform.mapToContent(point(1400f, 636f), 2261, 1272, 2800, 1272)!!
        assertEquals(1130.5f, center.x, 0.01f)
        assertEquals(636f, center.y, 0.01f)
    }

    @Test
    fun pinchKeepsContentUnderGestureFocus() {
        val transform = ViewportTransform()
        val focus = point(700f, 400f)
        val before = transform.mapToContent(focus, 1000, 500, 1000, 500)

        transform.zoomBy(2f, focus, point(0f, 0f), 1000, 500, 1000, 500)
        val after = transform.mapToContent(focus, 1000, 500, 1000, 500)

        assertEquals(before!!.x, after!!.x, 0.01f)
        assertEquals(before.y, after.y, 0.01f)
        assertEquals(2f, transform.scale, 0.01f)
    }

    @Test
    fun panIsConsumedOnlyWhileZoomedAndChangesVisibleContent() {
        val transform = ViewportTransform()
        assertFalse(transform.panBy(100f, 0f, 1000, 500, 1000, 500))
        transform.zoomBy(2f, point(500f, 250f), point(0f, 0f), 1000, 500, 1000, 500)

        assertTrue(transform.panBy(100f, 0f, 1000, 500, 1000, 500))
        val center = transform.mapToContent(point(500f, 250f), 1000, 500, 1000, 500)!!
        assertEquals(450f, center.x, 0.01f)
    }

    @Test
    fun shrinkingToOneRecentersViewport() {
        val transform = ViewportTransform()
        transform.zoomBy(3f, point(700f, 400f), point(0f, 0f), 1000, 500, 1000, 500)
        transform.panBy(100f, 100f, 1000, 500, 1000, 500)
        transform.zoomBy(0.01f, point(500f, 250f), point(0f, 0f), 1000, 500, 1000, 500)

        assertEquals(1f, transform.scale, 0.01f)
        assertEquals(0f, transform.translationX, 0.01f)
        assertEquals(0f, transform.translationY, 0.01f)
    }

    private fun point(x: Float, y: Float) = ViewportTransform.Point(x, y)
}
