package top.ozaoza.remoe

import android.app.Application
import okhttp3.OkHttpClient
import top.ozaoza.remoe.diagnostics.DiagnosticLog
import top.ozaoza.remoe.rtc.RtcRuntime

class RemoeApplication : Application() {
    lateinit var rtcRuntime: RtcRuntime
        private set
    lateinit var httpClient: OkHttpClient
        private set
    lateinit var diagnosticLog: DiagnosticLog
        private set

    override fun onCreate() {
        super.onCreate()
        diagnosticLog = DiagnosticLog(this)
        httpClient = OkHttpClient.Builder().build()
        rtcRuntime = RtcRuntime.create(this)
        diagnosticLog.append("application", "WebRTC runtime initialized")
    }
}
