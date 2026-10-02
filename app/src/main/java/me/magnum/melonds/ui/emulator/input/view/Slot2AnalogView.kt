package me.magnum.melonds.ui.emulator.input.view

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View
import me.magnum.melonds.R
import kotlin.math.min

class Slot2AnalogView(context: Context) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private var axisX = 0f
    private var axisY = 0f
    val travelRadius: Float get() = min(width, height) * 0.32f

    init {
        contentDescription = context.getString(R.string.input_slot2_analog)
    }

    fun setAxes(x: Float, y: Float) {
        if (axisX == x && axisY == y) return
        axisX = x
        axisY = y
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val size = min(width, height).toFloat()
        paint.color = Color.GRAY
        canvas.drawCircle(width / 2f, height / 2f, size * 0.48f, paint)
        paint.color = Color.WHITE
        canvas.drawCircle(width / 2f + axisX * travelRadius, height / 2f + axisY * travelRadius, size * 0.15f, paint)
    }
}
