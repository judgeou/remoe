package top.ozaoza.remoe

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import top.ozaoza.remoe.rtc.RtcCodecProbe
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private val probeExecutor = Executors.newSingleThreadExecutor()
    private lateinit var resultView: TextView
    private lateinit var retryButton: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        resultView = TextView(this).apply {
            setTextColor(Color.rgb(224, 229, 238))
            textSize = 13f
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        }
        retryButton = Button(this).apply {
            text = getString(R.string.codec_probe_retry)
            setOnClickListener { runProbe() }
        }

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(20), dp(20), dp(20), dp(32))
            addView(TextView(context).apply {
                text = getString(R.string.codec_probe_title)
                setTextColor(Color.WHITE)
                textSize = 24f
                typeface = Typeface.DEFAULT_BOLD
            })
            addView(retryButton, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(12) })
            addView(resultView, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(16) })
        }

        setContentView(ScrollView(this).apply {
            setBackgroundColor(Color.rgb(18, 21, 27))
            addView(content)
        })
        runProbe()
    }

    private fun runProbe() {
        retryButton.isEnabled = false
        resultView.text = getString(R.string.codec_probe_running)
        val runtime = (application as RemoeApplication).rtcRuntime
        probeExecutor.execute {
            val report = RtcCodecProbe(runtime).run()
            runOnUiThread {
                if (!isDestroyed) {
                    resultView.text = report
                    retryButton.isEnabled = true
                }
            }
        }
    }

    override fun onDestroy() {
        probeExecutor.shutdownNow()
        super.onDestroy()
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
