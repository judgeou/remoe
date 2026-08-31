package top.ozaoza.remoe.input

import android.view.View

class RemoteViewportController(
    private val inputView: View,
    private val contentView: View,
) {
    private val transform = ViewportTransform()

    fun zoomBy(
        scaleFactor: Float,
        focus: TouchGestureEngine.Point,
        focusDelta: TouchGestureEngine.Point,
    ) {
        transform.zoomBy(
            scaleFactor,
            ViewportTransform.Point(focus.x, focus.y),
            ViewportTransform.Point(focusDelta.x, focusDelta.y),
            contentView.width,
            contentView.height,
            inputView.width,
            inputView.height,
        )
        applyTransform()
    }

    fun panBy(deltaX: Float, deltaY: Float): Boolean {
        val consumed = transform.panBy(
            deltaX,
            deltaY,
            contentView.width,
            contentView.height,
            inputView.width,
            inputView.height,
        )
        if (consumed) applyTransform()
        return consumed
    }

    fun remoteCoordinates(point: TouchGestureEngine.Point): Pair<Int, Int>? {
        val mapped = transform.mapToContent(
            ViewportTransform.Point(point.x, point.y),
            contentView.width,
            contentView.height,
            inputView.width,
            inputView.height,
        ) ?: return null
        return normalize(mapped.x, contentView.width) to normalize(mapped.y, contentView.height)
    }

    fun reset() {
        transform.reset()
        applyTransform()
    }

    private fun applyTransform() {
        contentView.scaleX = transform.scale
        contentView.scaleY = transform.scale
        contentView.translationX = transform.translationX
        contentView.translationY = transform.translationY
    }

    private fun normalize(value: Float, size: Int): Int =
        (value.coerceIn(0f, (size - 1).toFloat()) * 65_535f / (size - 1)).toInt()
}
