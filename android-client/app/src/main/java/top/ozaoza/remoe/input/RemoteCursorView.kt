package top.ozaoza.remoe.input

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.view.View

/** High-contrast mouse cursor with a larger touch-screen-friendly footprint. */
class RemoteCursorView(context: Context) : View(context) {
    private val cursor = Path()
    private val density = resources.displayMetrics.density
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        style = Paint.Style.FILL
    }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeWidth = density * 2.5f
        strokeJoin = Paint.Join.ROUND
    }

    init {
        isClickable = false
        isFocusable = false
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val scaleX = width / BASE_WIDTH
        val scaleY = height / BASE_HEIGHT
        cursor.reset()
        cursor.moveTo(3f * scaleX, 2f * scaleY)
        cursor.lineTo(3f * scaleX, 38f * scaleY)
        cursor.lineTo(12f * scaleX, 29f * scaleY)
        cursor.lineTo(20f * scaleX, 49f * scaleY)
        cursor.lineTo(28f * scaleX, 45f * scaleY)
        cursor.lineTo(20f * scaleX, 27f * scaleY)
        cursor.lineTo(34f * scaleX, 27f * scaleY)
        cursor.close()
        canvas.drawPath(cursor, fillPaint)
        canvas.drawPath(cursor, strokePaint)
    }

    private companion object {
        const val BASE_WIDTH = 38f
        const val BASE_HEIGHT = 52f
    }
}
