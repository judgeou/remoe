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

    fun moveRemotePointer(
        delta: TouchGestureEngine.Point,
        currentX: Int,
        currentY: Int,
    ): Pair<Int, Int>? {
        val displayedWidth = contentView.width * transform.scale
        val displayedHeight = contentView.height * transform.scale
        if (displayedWidth <= 1f || displayedHeight <= 1f) return null
        return (
            currentX + delta.x * 65_535f / displayedWidth
            ).toInt().coerceIn(0, 65_535) to (
            currentY + delta.y * 65_535f / displayedHeight
            ).toInt().coerceIn(0, 65_535)
    }

    fun viewCoordinates(remoteX: Int, remoteY: Int): TouchGestureEngine.Point? {
        if (contentView.width <= 1 || contentView.height <= 1 ||
            inputView.width <= 1 || inputView.height <= 1
        ) return null
        val contentX = remoteX.coerceIn(0, 65_535) * (contentView.width - 1f) / 65_535f
        val contentY = remoteY.coerceIn(0, 65_535) * (contentView.height - 1f) / 65_535f
        return TouchGestureEngine.Point(
            x = inputView.width / 2f +
                (contentX - contentView.width / 2f) * transform.scale + transform.translationX,
            y = inputView.height / 2f +
                (contentY - contentView.height / 2f) * transform.scale + transform.translationY,
        )
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
