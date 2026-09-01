package top.ozaoza.remoe

import android.app.Application
import okhttp3.OkHttpClient
import top.ozaoza.remoe.diagnostics.DiagnosticLog
import top.ozaoza.remoe.rtc.RtcRuntime

class RemoeApplication : Application() {
    val rtcRuntime: RtcRuntime by lazy(LazyThreadSafetyMode.SYNCHRONIZED) {
        RtcRuntime.create(this).also {
            diagnosticLog.append("application", "WebRTC runtime initialized on demand")
        }
    }
    lateinit var httpClient: OkHttpClient
        private set
    lateinit var diagnosticLog: DiagnosticLog
        private set

    override fun onCreate() {
        super.onCreate()
        diagnosticLog = DiagnosticLog(this)
        httpClient = OkHttpClient.Builder().build()
    }
}
