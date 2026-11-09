// SPDX-License-Identifier: MIT
package ai.shadowvision.g0

import android.graphics.Bitmap
import android.os.PowerManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * The one and only bridge to the native core.
 *
 * Contract (docs/design-review-ar.md §4.2):
 *  - Exactly ONE worker thread performs inference. Parallelism between models
 *    costs heat and RSS far more than it buys throughput on a phone.
 *  - Cancellation is cooperative and crosses the ABI as a flag, never as a
 *    thread interrupt: `Run()` inside ORT cannot be interrupted, only observed
 *    between steps.
 *  - Every call returns a [Report]; nothing is "assumed to have worked".
 */
object SvEngine {

    init {
        System.loadLibrary("shadowvision_jni")
    }

    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "sv-inference").apply { priority = Thread.NORM_PRIORITY - 1 }
    }
    val dispatcher = worker.asCoroutineDispatcher()

    private val cancelFlag = AtomicInteger(0)

    /** Result of one native call, mapped 1:1 from SvReport. */
    data class Stage(val name: String, val ms: Double, val peakRssKb: Long)

    data class Report(
        val status: Int,
        val totalMs: Double,
        val peakRssMb: Double,
        val trackedPeakMb: Double,
        val budgetMb: Double,
        val npuPartitionRatio: Float,
        val thermalStatus: Int,
        val stages: List<Stage>,
    ) {
        val ok: Boolean get() = status == SV_OK
        val summary: String
            get() = buildString {
                append(if (ok) "OK" else "ERR($status)")
                append(" in ").append("%.1f".format(totalMs)).append(" ms")
                append(" · rss ").append("%.0f".format(peakRssMb)).append(" MB")
                append(" · npu ").append((npuPartitionRatio * 100).toInt()).append("%")
            }
    }

    private const val SV_OK = 0
    private const val SV_ERR_BUDGET_EXCEEDED = 3
    private const val SV_ERR_CANCELLED = 6

    private external fun nativeInit(budgetKb: Long): Int
    private external fun nativeLoadModel(path: String?, useNpu: Boolean, threads: Int): Int
    private external fun nativeInpaint(
        rgba: ByteArray,
        width: Int,
        height: Int,
        mask: ByteArray,
        maskWidth: Int,
        maskHeight: Int,
        workResolution: Int,
        cancel: IntArray,
        report: LongArray,
    ): Int
    private external fun nativeRelease()
    private external fun nativeVersion(): String
    private external fun nativeLogDump(): String

    fun version(): String = nativeVersion()

    /** Must be called once, after the process starts. Budget is a hard ceiling. */
    fun init(budgetMb: Int): Boolean = nativeInit(budgetMb.toLong() * 1024L) == SV_OK

    fun loadModel(path: String?, useNpu: Boolean, threads: Int = 2): Int =
        nativeLoadModel(path, useNpu, threads)

    fun release() = nativeRelease()

    fun cancel() {
        cancelFlag.set(1)
    }

    fun logDump(): String = nativeLogDump()

    /**
     * Inpaint on the worker thread. `bitmap` must be ARGB_8888 and is mutated;
     * callers should pass a copy. The mask is 8-bit, one byte per pixel, where
     * 255 = remove. Returns a [Report] that the dev console renders verbatim.
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
        val px = w * h
        val rgba = ByteArray(px * 4)
        val buffer = java.nio.ByteBuffer.wrap(rgba)
        bitmap.copyPixelsToBuffer(buffer)

        // report layout (LongArray, fixed order — keep in sync with JNI):
        // [0]=status [1]=totalMs*1000 [2]=peakRssKb [3]=trackedPeakKb
        // [4]=budgetKb [5]=npuRatio*1000 [6]=stageCount
        // [7..7+2*n)=stage ms*1000, name hash
        val report = LongArray(64)
        val flag = intArrayOf(0)
        val status = nativeInpaint(rgba, w, h, mask, maskWidth, maskHeight, workResolution,
                                   flag, report)
        if (status == SV_OK || status == SV_ERR_BUDGET_EXCEEDED) {
            buffer.rewind()
            bitmap.copyPixelsFromBuffer(buffer)
        }
        Report(
            status = status,
            totalMs = report[1] / 1000.0,
            peakRssMb = report[2] / 1024.0,
            trackedPeakMb = report[3] / 1024.0,
            budgetMb = report[4] / 1024.0,
            npuPartitionRatio = report[5] / 1000f,
            thermalStatus = thermal,
            stages = parseStages(report),
        )
    }

    private fun parseStages(r: LongArray): List<Stage> {
        val count = r[6].toInt().coerceIn(0, 8)
        return (0 until count).map { i ->
            Stage(name = "stage$i", ms = r[7 + i * 2] / 1000.0, peakRssKb = r[8 + i * 2])
        }
    }

    /** Thermal state, read before and after every run — never hidden from the user. */
    fun thermalStatus(pm: PowerManager): Int = pm.currentThermalStatus
}
