package top.ozaoza.remoe.diagnostics

import android.content.Context
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.time.Instant

class DiagnosticLog(context: Context) {
    private val file = File(context.filesDir, "diagnostics/latest.log")
    private val recent = ArrayDeque<String>()

    init {
        file.parentFile?.mkdirs()
        if (file.length() > MAX_FILE_BYTES) {
            file.delete()
        }
    }

    @Synchronized
    fun append(category: String, message: String) {
        val safeCategory = category.take(32).replace(unsafeCharacters, "_")
        val safeMessage = message.replace('\r', ' ').replace('\n', ' ').take(MAX_MESSAGE_LENGTH)
        val line = "${Instant.now()} +${SystemClock.elapsedRealtime()}ms [$safeCategory] $safeMessage"
        recent.addLast(line)
        while (recent.size > MAX_RECENT_LINES) recent.removeFirst()
        Log.i(TAG, "[$safeCategory] $safeMessage")
        runCatching {
            if (file.length() > MAX_FILE_BYTES) file.delete()
            file.appendText("$line\n", Charsets.UTF_8)
        }.onFailure { Log.w(TAG, "Unable to append diagnostic log", it) }
    }

    @Synchronized
    fun snapshot(): String = recent.joinToString("\n")

    fun path(): String = file.absolutePath

    private companion object {
        const val TAG = "RemoeDiagnostic"
        const val MAX_RECENT_LINES = 200
        const val MAX_MESSAGE_LENGTH = 2_048
        const val MAX_FILE_BYTES = 2L * 1024 * 1024
        val unsafeCharacters = Regex("[^A-Za-z0-9_.-]")
    }
}
