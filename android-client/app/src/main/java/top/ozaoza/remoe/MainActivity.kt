package top.ozaoza.remoe

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContentView(TextView(this).apply {
            setBackgroundColor(Color.rgb(18, 21, 27))
            setTextColor(Color.WHITE)
            textSize = 22f
            gravity = Gravity.CENTER
            text = getString(R.string.environment_ready)
        })
    }
}
