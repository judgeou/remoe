package top.ozaoza.remoe

import android.app.Application
import top.ozaoza.remoe.rtc.RtcRuntime

class RemoeApplication : Application() {
    lateinit var rtcRuntime: RtcRuntime
        private set

    override fun onCreate() {
        super.onCreate()
        rtcRuntime = RtcRuntime.create(this)
    }
}
