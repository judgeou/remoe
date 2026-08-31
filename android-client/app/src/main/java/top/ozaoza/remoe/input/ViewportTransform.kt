package top.ozaoza.remoe.input

import kotlin.math.max

/** Pure viewport transform used by the Android view adapter and JVM tests. */
class ViewportTransform(
    private val maximumScale: Float = 4f,
) {
    data class Point(val x: Float, val y: Float)

    var scale: Float = 1f
        private set
    var translationX: Float = 0f
        private set
    var translationY: Float = 0f
        private set

    init {
        require(maximumScale > 1f)
    }

    fun zoomBy(
        scaleFactor: Float,
        focus: Point,
        focusDelta: Point,
        contentWidth: Int,
        contentHeight: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ) {
        if (!scaleFactor.isFinite() || scaleFactor <= 0f || !validSizes(
                contentWidth, contentHeight, viewportWidth, viewportHeight,
            )
        ) return
        val oldScale = scale
        val newScale = (oldScale * scaleFactor).coerceIn(1f, maximumScale)
        val ratio = newScale / oldScale
        val centerX = viewportWidth / 2f
        val centerY = viewportHeight / 2f
        val previousFocusX = focus.x - focusDelta.x
        val previousFocusY = focus.y - focusDelta.y
        translationX = focus.x - centerX -
            (previousFocusX - centerX - translationX) * ratio
        translationY = focus.y - centerY -
            (previousFocusY - centerY - translationY) * ratio
        scale = newScale
        clamp(contentWidth, contentHeight, viewportWidth, viewportHeight)
    }

    fun panBy(
        deltaX: Float,
        deltaY: Float,
        contentWidth: Int,
        contentHeight: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ): Boolean {
        if (scale <= 1f || !validSizes(contentWidth, contentHeight, viewportWidth, viewportHeight)) {
            return false
        }
        translationX += deltaX
        translationY += deltaY
        clamp(contentWidth, contentHeight, viewportWidth, viewportHeight)
        return true
    }

    fun mapToContent(
        point: Point,
        contentWidth: Int,
        contentHeight: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ): Point? {
        if (!validSizes(contentWidth, contentHeight, viewportWidth, viewportHeight)) return null
        val centerX = viewportWidth / 2f
        val centerY = viewportHeight / 2f
        val contentX = contentWidth / 2f + (point.x - centerX - translationX) / scale
        val contentY = contentHeight / 2f + (point.y - centerY - translationY) / scale
        if (contentX < 0f || contentX > contentWidth - 1f ||
            contentY < 0f || contentY > contentHeight - 1f
        ) return null
        return Point(contentX, contentY)
    }

    fun reset() {
        scale = 1f
        translationX = 0f
        translationY = 0f
    }

    private fun clamp(
        contentWidth: Int,
        contentHeight: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ) {
        if (scale == 1f) {
            translationX = 0f
            translationY = 0f
            return
        }
        val maxX = max(0f, (contentWidth * scale - viewportWidth) / 2f)
        val maxY = max(0f, (contentHeight * scale - viewportHeight) / 2f)
        translationX = translationX.coerceIn(-maxX, maxX)
        translationY = translationY.coerceIn(-maxY, maxY)
    }

    private fun validSizes(
        contentWidth: Int,
        contentHeight: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ) = contentWidth > 1 && contentHeight > 1 && viewportWidth > 1 && viewportHeight > 1
}
