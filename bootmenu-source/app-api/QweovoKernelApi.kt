package io.qweovo.kernelapi

import android.system.Os
import android.system.OsConstants
import org.json.JSONObject
import java.io.File
import java.io.IOException

/** Blocking file I/O: call from a worker thread or Dispatchers.IO. No su. */
object QweovoKernelApi {
    private const val INFO = "/proc/qweovo_info"
    private const val CONTROL = "/proc/panic_ctl"
    const val KERNEL_ID = "qweovo.secret_kernel"

    enum class PanicAction(val wireValue: String) {
        REBOOT("reboot"), RAMDUMP("ramdump")
    }

    data class PanicSettings(
        val oopsContinue: Boolean,
        val action: PanicAction,
        val ramdumpReady: Boolean
    )

    /** Unknown fields may be ignored; only schema 1 is implemented here. */
    fun readInfo(): JSONObject {
        val info = JSONObject(File(INFO).readText(Charsets.UTF_8))
        require(info.optInt("schema_version") == 1) { "Unsupported kernel API schema" }
        require(info.optString("kernel_id") == KERNEL_ID) { "Different kernel identity" }
        return info
    }

    fun supports(info: JSONObject, capability: String): Boolean =
        info.optJSONObject("capabilities")?.optBoolean(capability, false) ?: false

    fun readPanicSettings(info: JSONObject = readInfo()): PanicSettings {
        val configuration = info.getJSONObject("panic_configuration")
        val wireValue = configuration.getJSONObject("panic_action").getString("value")
        val action = PanicAction.values().firstOrNull { it.wireValue == wireValue }
            ?: throw IOException("Unsupported panic action: $wireValue")
        return PanicSettings(
            configuration.getJSONObject("oops_continue").getBoolean("value"),
            action,
            configuration.getBoolean("ramdump_ready")
        )
    }

    /** The two options are validated together. Re-read on success. */
    fun setPanicSettings(oopsContinue: Boolean, action: PanicAction): PanicSettings {
        val info = readInfo()
        require(supports(info, "panic_control")) { "Panic control unavailable" }
        if (action == PanicAction.RAMDUMP) {
            require(readPanicSettings(info).ramdumpReady) { "Ramdump not ready" }
        }
        val command = "oops_continue=${if (oopsContinue) 1 else 0} " +
            "panic_action=${action.wireValue}\n"
        val bytes = command.toByteArray(Charsets.US_ASCII)
        require(bytes.size <= info.getJSONObject("limits").getInt("panic_write_bytes"))
        val fd = Os.open(CONTROL,
            OsConstants.O_WRONLY or OsConstants.O_TRUNC or OsConstants.O_CLOEXEC, 0)
        try {
            if (Os.write(fd, bytes, 0, bytes.size) != bytes.size) {
                throw IOException("Short kernel command write")
            }
        } finally {
            Os.close(fd)
        }
        return readPanicSettings()
    }
}
