// SPDX-License-Identifier: MIT
package ai.shadowvision.core

import android.graphics.Bitmap
import android.os.PowerManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * The one and only bridge to the native core (ABI v2).
 *
 * Rules that are not negotiable (docs/design-review-ar.md §4.2):
 *  - ONE worker thread performs all inference. Parallelism between models costs
 *    heat and RSS far more than it buys throughput on a phone.
 *  - Cancellation crosses the ABI as a flag, never as a thread interrupt: ORT's
 *    Run() cannot be interrupted, only observed between steps.
 *  - Every call returns a [Report]. Nothing is "assumed to have worked".
 */
object SvEngine {

    init {
        System.loadLibrary("shadowvision_jni")
    }

    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "sv-inference").apply { priority = Thread.NORM_PRIORITY - 1 }
    }
    private val dispatcher = worker.asCoroutineDispatcher()

    private val cancelFlag = AtomicInteger(0)

    const val OK = 0
    const val ERR_INVALID_ARGUMENT = 1
    const val ERR_OUT_OF_MEMORY = 2
    const val ERR_BUDGET_EXCEEDED = 3
    const val ERR_MODEL_NOT_LOADED = 4
    const val ERR_BACKEND_UNAVAILABLE = 5
    const val ERR_CANCELLED = 6
    const val ERR_THERMAL_THROTTLED = 7
    const val ERR_INTERNAL = 8

    fun describe(status: Int): String = when (status) {
        OK -> "ok"
        ERR_INVALID_ARGUMENT -> "invalid argument"
        ERR_OUT_OF_MEMORY -> "out of memory"
        ERR_BUDGET_EXCEEDED -> "memory budget exceeded — lower the working resolution"
        ERR_MODEL_NOT_LOADED -> "no model loaded"
        ERR_BACKEND_UNAVAILABLE -> "accelerator unavailable"
        ERR_CANCELLED -> "cancelled"
        ERR_THERMAL_THROTTLED -> "device is too hot — let it cool down"
        ERR_INTERNAL -> "internal error"
        else -> "unknown error ($status)"
    }

    data class Stage(val name: String, val ms: Double)

    data class Report(
        val status: Int,
        val totalMs: Double,
        val peakRssMb: Double,
        val trackedPeakMb: Double,
        val budgetMb: Double,
        val npuRatio: Float,
        val thermalStatus: Int,
        val stages: List<Stage>,
    ) {
        val ok: Boolean get() = status == OK
    }

    // ---- lifecycle -----------------------------------------------------

    fun init(budgetMb: Int): Int = nativeInit(budgetMb.toLong() * 1024L)
    fun version(): String = nativeVersion()
    fun release() = nativeRelease()
    fun cancel() {
        cancelFlag.set(1)
    }
    fun logDump(): String = nativeLogDump()

    /** Current process RSS in MB, for the console gauge. Cheap enough to poll 1/s. */
    fun rssMb(): Double = nativeRssMb() / 1024.0

    // ---- models --------------------------------------------------------

    /** Empty path = built-in synthetic backend (no model installed). */
    fun loadInpainter(path: String?, useNpu: Boolean, threads: Int = 2): Int =
        nativeLoadModel(path, useNpu, threads)

    fun loadSegmenter(encoderPath: String?, decoderPath: String?): Int =
        nativeSegLoad(encoderPath, decoderPath)

    fun releaseSegmenter() = nativeSegRelease()

    // ---- inference -----------------------------------------------------

    /**
     * Inpaints `bitmap` in place (ARGB_8888). `mask` is 8-bit, one byte per pixel,
     * 255 = remove. Runs on the worker thread; the caller awaits the Report.
     */
    suspend fun inpaint(
        bitmap: Bitmap,
        mask: ByteArray,
        maskWidth: Int,
        maskHeight: Int,
        workResolution: Int = 512,
        thermal: Int = -1,
    ): Report = withContext(dispatcher) {
        cancelFlag.set(0)
        val w = bitmap.width
        val h = bitmap.height
        val rgba = ByteArray(w * h * 4)
        val buffer = java.nio.ByteBuffer.wrap(rgba)
        bitmap.copyPixelsToBuffer(buffer)

        val report = LongArray(64)
        val status = nativeInpaint(
            rgba, w, h, mask, maskWidth, maskHeight, workResolution,
            intArrayOf(cancelFlag.get()), report
        )
        if (status == OK) {
            buffer.rewind()
            bitmap.copyPixelsFromBuffer(buffer)
        }
        Report(
            status = status,
            totalMs = report[1] / 1000.0,
            peakRssMb = report[2] / 1024.0,
            trackedPeakMb = report[3] / 1024.0,
            budgetMb = report[4] / 1024.0,
            npuRatio = report[5] / 1000f,
            thermalStatus = thermal,
            stages = parseStages(report),
        )
    }

    /**
     * Runs the segmentation encoder for one image. Call off the main thread; it
     * takes 0.4-1.2 s and blocks the worker, which is why the studio shows an
     * "analysis" chip instead of freezing the canvas.
     */
    suspend fun prepareSegmentation(bitmap: Bitmap): Int = withContext(dispatcher) {
        val w = bitmap.width
        val h = bitmap.height
        val rgba = ByteArray(w * h * 4)
        bitmap.copyPixelsToBuffer(java.nio.ByteBuffer.wrap(rgba))
        nativeSegPrepare(rgba, w, h)
    }

    /**
     * Answers a set of taps with a full-resolution mask. Fast (30-80 ms) once the
     * encoder has run — this is what makes "tap the object" feel instant.
     */
    suspend fun tapToMask(
        taps: List<Tap>,
        outWidth: Int,
        outHeight: Int,
    ): MaskResult = withContext(dispatcher) {
        if (taps.isEmpty()) return@withContext MaskResult(ERR_INVALID_ARGUMENT, null, 0, 0)
        val xs = IntArray(taps.size) { taps[it].x }
        val ys = IntArray(taps.size) { taps[it].y }
        val labels = ByteArray(taps.size) { if (taps[it].foreground) 1 else 0 }
        val mask = ByteArray(outWidth * outHeight)
        val dims = IntArray(2)
        val rc = nativeSegTap(xs, ys, labels, taps.size, mask, dims)
        MaskResult(rc, if (rc == OK) mask else null, dims[0], dims[1])
    }

    data class Tap(val x: Int, val y: Int, val foreground: Boolean)
    data class MaskResult(val status: Int, val mask: ByteArray?, val width: Int, val height: Int)

    private fun parseStages(r: LongArray): List<Stage> {
        val count = r[6].toInt().coerceIn(0, 8)
        return (0 until count).map { Stage("stage$it", r[7 + it * 2] / 1000.0) }
    }

    fun thermalStatus(pm: PowerManager): Int = pm.currentThermalStatus

    // ---- natives (keep in sync with jni_bridge.cpp) ---------------------
    private external fun nativeInit(budgetKb: Long): Int
    private external fun nativeLoadModel(path: String?, useNpu: Boolean, threads: Int): Int
    private external fun nativeInpaint(
        rgba: ByteArray, width: Int, height: Int,
        mask: ByteArray, maskWidth: Int, maskHeight: Int,
        workResolution: Int, cancel: IntArray, report: LongArray,
    ): Int
    private external fun nativeRelease()
    private external fun nativeVersion(): String
    private external fun nativeLogDump(): String
    private external fun nativeRssMb(): Double

    private external fun nativeSegLoad(encoder: String?, decoder: String?): Int
    private external fun nativeSegPrepare(rgba: ByteArray, width: Int, height: Int): Int
    private external fun nativeSegTap(
        xs: IntArray, ys: IntArray, labels: ByteArray, count: Int,
        outMask: ByteArray, dims: IntArray,
    ): Int
    private external fun nativeSegRelease()

    private external fun nativeCatalogLoad(manifestPath: String, pubkeyHex: String): String
    private external fun nativeCatalogError(): String
    private external fun nativeCatalogVerify(modelsDir: String, modelId: String): Int

    internal fun catalogLoad(manifestPath: String, pubkeyHex: String): String =
        nativeCatalogLoad(manifestPath, pubkeyHex)

    internal fun catalogError(): String = nativeCatalogError()

    internal fun catalogVerify(modelsDir: String, modelId: String): Int =
        nativeCatalogVerify(modelsDir, modelId)
}
