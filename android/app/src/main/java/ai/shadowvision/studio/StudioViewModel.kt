// SPDX-License-Identifier: MIT
package ai.shadowvision.studio

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import android.os.PowerManager
import androidx.exifinterface.media.ExifInterface
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import ai.shadowvision.core.ModelCatalog
import ai.shadowvision.core.SvEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Studio state machine. All heavy work runs on SvEngine's single worker; this
 * class only sequences stages and exposes a single immutable UI state, so the
 * canvas can never observe a half-applied operation.
 *
 * The pipeline is exactly the four stages from the spec:
 *   import -> mask (auto via segmentation, or brush) -> inpaint -> blend (native)
 */
class StudioViewModel(app: Application) : AndroidViewModel(app) {

    enum class Tool { BRUSH, SMART_SELECT, ERASER }

    data class UiState(
        val image: Bitmap? = null,
        val tool: Tool = Tool.BRUSH,
        val hasMask: Boolean = false,
        val maskCoverage: Float = 0f,
        val busy: Boolean = false,
        val stage: String = "",
        val progress: Float = 0f,
        val showBefore: Boolean = false,
        val message: String? = null,
        val canUndo: Boolean = false,
        val canRedo: Boolean = false,
        val brushRadius: Float = 24f,
        val zoom: Float = 1f,
        val engineMode: String = "built-in",
        val segmenterMode: String = "built-in (no model installed)",
        val segmenterLatencyMs: String = "—",
        val lastReport: SvEngine.Report? = null,
        val segmentationReady: Boolean = false,
        /** Bumped every time a native selection is produced, so the canvas can
         *  consume it exactly once even if hasMask was already true. */
        val selectionVersion: Int = 0,
        val resultAvailable: Boolean = false,
        val originalForCompare: Bitmap? = null,
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var catalogEntry: ModelCatalog.Entry? = null

    init {
        SvEngine.init(budgetMb = 1200)
        // The built-in segmenter keeps the studio usable with no models at all;
        // a verified catalog model replaces it (D10). Never the other way round:
        // a model that fails verification must not be loaded "just to try".
        SvEngine.loadSegmenter(null, null)
        _state.value = _state.value.copy(engineMode = "built-in (no model installed)")
        loadCatalog()
    }

    private fun loadCatalog() {
        viewModelScope.launch(Dispatchers.IO) {
            val ctx = getApplication<Application>()
            val result = ModelCatalog.load(ctx)

            // --- segmenter: encoder + decoder, both verified, both present ----
            val encEntry = result.entries.firstOrNull { it.id.startsWith("mobilesam.encoder") }
            val decEntry = result.entries.firstOrNull { it.id.startsWith("mobilesam.decoder") }
            if (result.verified && encEntry != null && decEntry != null) {
                val encPath = ModelCatalog.pathFor(ctx, encEntry)
                val decPath = ModelCatalog.pathFor(ctx, decEntry)
                if (encPath != null && decPath != null) {
                    val segRc = SvEngine.loadSegmenter(encPath, decPath)
                    _state.value = _state.value.copy(
                        segmenterMode = if (segRc == SvEngine.OK)
                            "MobileSAM (1024 px, split encoder/decoder)"
                        else "built-in (load failed: ${SvEngine.describe(segRc)})",
                    )
                }
            }

            val entry = result.entries.firstOrNull { it.id.startsWith("lama") }
            if (result.verified && entry != null) {
                val path = ModelCatalog.pathFor(ctx, entry)
                val rc = if (path != null) {
                    SvEngine.loadInpainter(path, useNpu = false)
                } else {
                    SvEngine.loadInpainter(null, useNpu = false)
                }
                catalogEntry = if (path != null) entry else null
                _state.value = _state.value.copy(
                    engineMode = if (path != null) "LaMa (${entry.id})"
                    else "built-in (model not verified: ${result.reason})",
                )
                if (rc != SvEngine.OK && rc != SvEngine.ERR_MODEL_NOT_LOADED) {
                    _state.value = _state.value.copy(message = SvEngine.describe(rc))
                }
            } else {
                SvEngine.loadInpainter(null, useNpu = false)
                _state.value = _state.value.copy(
                    engineMode = "built-in (catalog: ${result.reason})",
                )
            }
        }
    }

    /**
     * Decodes with the display-safe bounds: a 12 MP JPEG is ~48 MB in ARGB_8888,
     * and decoding it twice (preview + full) is how Android apps die. We decode
     * once at a bounded size (long side <= 3072) and keep one copy.
     */
    fun importImage(uri: Uri) {
        viewModelScope.launch {
            _state.value = _state.value.copy(busy = true, stage = "decoding", progress = 0.08f)
            val bmp = withContext(Dispatchers.IO) {
                val ctx = getApplication<Application>()
                val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                ctx.contentResolver.openInputStream(uri)?.use {
                    BitmapFactory.decodeStream(it, null, opts)
                }
                var sample = 1
                val longSide = maxOf(opts.outWidth, opts.outHeight)
                while (longSide / sample > 3072) sample *= 2

                val decodeOpts = BitmapFactory.Options().apply {
                    inSampleSize = sample
                    inPreferredConfig = Bitmap.Config.ARGB_8888
                }
                val decoded = ctx.contentResolver.openInputStream(uri)?.use {
                    BitmapFactory.decodeStream(it, null, decodeOpts)
                } ?: return@withContext null

                // Honour EXIF rotation: a photo shown sideways is the single most
                // common "why does it look wrong" report in editing apps.
                val rotation = ctx.contentResolver.openInputStream(uri)?.use { stream ->
                    ExifInterface(stream).rotationDegrees
                } ?: 0
                if (rotation != 0) {
                    val m = Matrix().apply { postRotate(rotation.toFloat()) }
                    val rotated = Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, m, true)
                    if (rotated !== decoded) decoded.recycle()
                    rotated
                } else {
                    decoded
                }
            }
            if (bmp == null) {
                _state.value = _state.value.copy(busy = false, message = "Could not open that image")
                return@launch
            }
            _state.value = _state.value.copy(
                image = bmp,
                hasMask = false,
                maskCoverage = 0f,
                busy = true,
                stage = "analysing",
                progress = 0.2f,
                resultAvailable = false,
                originalForCompare = null,
                segmentationReady = false,
            )
            // Precompute the segmentation embedding in the background so the
            // first tap is instant (D5).
            val rc = SvEngine.prepareSegmentation(bmp)
            _state.value = _state.value.copy(
                busy = false,
                stage = "",
                progress = 0f,
                segmentationReady = rc == SvEngine.OK,
                message = if (rc == SvEngine.OK) null else "Smart select unavailable: ${SvEngine.describe(rc)}",
            )
        }
    }

    fun setTool(tool: Tool) {
        _state.value = _state.value.copy(tool = tool)
    }

    fun setBrushRadius(r: Float) {
        _state.value = _state.value.copy(brushRadius = r)
    }

    fun setZoom(z: Float) {
        _state.value = _state.value.copy(zoom = z)
    }

    fun onMaskChanged(hasMask: Boolean, coverage: Float) {
        _state.value = _state.value.copy(hasMask = hasMask, maskCoverage = coverage)
    }

    fun setCompare(show: Boolean) {
        _state.value = _state.value.copy(showBefore = show)
    }

    fun onUndoState(canUndo: Boolean, canRedo: Boolean) {
        _state.value = _state.value.copy(canUndo = canUndo, canRedo = canRedo)
    }

    fun consumeMessage() {
        _state.value = _state.value.copy(message = null)
    }

    /** Smart select: one tap -> native decoder -> mask. 30-80 ms on the worker. */
    fun smartSelect(x: Int, y: Int) {
        // The tap arrives in image pixels of `state.image`, which is exactly the
        // bitmap that prepareSegmentation() embedded, so no rescaling is needed
        // here. Passing a hard-coded 0 from the caller (as the old signature
        // invited) would have produced an empty mask buffer.
        val w = _state.value.image?.width ?: 0
        val h = _state.value.image?.height ?: 0
        if (w <= 0 || h <= 0) return
        if (!_state.value.segmentationReady) {
            _state.value = _state.value.copy(
                message = "Smart select needs the analysis step — import the photo again",
            )
            return
        }
        viewModelScope.launch {
            val t0 = System.currentTimeMillis()
            val rc = SvEngine.tapToMask(listOf(SvEngine.Tap(x, y, true)), w, h)
            val tookMs = System.currentTimeMillis() - t0
            if (rc.status == SvEngine.OK && rc.mask != null) {
                pendingSegMask = rc.mask
                _state.value = _state.value.copy(
                    message = "Region selected — refine with the brush, then remove",
                    hasMask = true,
                    selectionVersion = _state.value.selectionVersion + 1,
                    segmenterLatencyMs = "$tookMs ms",
                )
            } else {
                _state.value = _state.value.copy(
                    message = "Selection failed: ${SvEngine.describe(rc.status)}",
                )
            }
        }
    }

    /** Handed to the view so it can draw the mask the segmenter produced. */
    var pendingSegMask: ByteArray? = null

    /**
     * The actual removal. Runs the native pipeline (crop -> LaMa -> blend) and
     * keeps the previous bitmap so the user can compare before/after.
     */
    fun remove(bitmap: Bitmap, mask: ByteArray) {
        if (_state.value.busy) return
        viewModelScope.launch {
            val pm = getApplication<Application>().getSystemService(PowerManager::class.java)
            val thermal = SvEngine.thermalStatus(pm)
            if (thermal >= PowerManager.THERMAL_STATUS_SEVERE) {
                _state.value = _state.value.copy(
                    message = "Device is too hot — let it cool before a large edit",
                )
                return@launch
            }
            // The snapshot for "Before/After" is taken AND RELEASED only after the
            // native pipeline has consumed the working bitmap. The old code
            // assigned a fresh copy into `originalForCompare` on every removal and
            // never recycled the previous one: two removals on a 12 MP photo is
            // ~100 MB of orphaned bitmaps, which is an OOM on a 4 GB phone.
            _state.value.originalForCompare?.let { old ->
                if (old !== bitmap && !old.isRecycled) old.recycle()
            }
            _state.value = _state.value.copy(
                busy = true,
                stage = "removing",
                progress = 0.3f,
                originalForCompare = bitmap.copy(Bitmap.Config.ARGB_8888, false),
            )
            val report = SvEngine.inpaint(
                bitmap = bitmap,
                mask = mask,
                maskWidth = bitmap.width,
                maskHeight = bitmap.height,
                workResolution = 512,
                thermal = thermal,
            )
            _state.value = _state.value.copy(
                busy = false,
                stage = "",
                progress = 0f,
                lastReport = report,
                resultAvailable = report.ok,
                showBefore = false,
                message = if (report.ok) null else SvEngine.describe(report.status),
            )
        }
    }

    fun revertToOriginal() {
        val current = _state.value
        val original = current.originalForCompare ?: return
        val fresh = original.copy(Bitmap.Config.ARGB_8888, false)
        // The edited bitmap is dead once we revert; without this, every revert
        // leaked a full-resolution copy.
        current.image?.let { if (it !== fresh && !it.isRecycled) it.recycle() }
        _state.value = current.copy(
            image = fresh,
            resultAvailable = false,
            hasMask = false,
            maskCoverage = 0f,
            selectionVersion = current.selectionVersion + 1,
        )
    }

    /** Saves to app-external Pictures (no permission needed on API 29+). */
    fun export(bitmap: Bitmap, onDone: (File?) -> Unit) {
        viewModelScope.launch {
            val file = withContext(Dispatchers.IO) {
                try {
                    val dir = File(
                        getApplication<Application>().getExternalFilesDir(null),
                        "exports",
                    ).apply { mkdirs() }
                    val out = File(dir, "shadowvision_${System.currentTimeMillis()}.png")
                    out.outputStream().use { stream ->
                        bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)
                    }
                    out
                } catch (e: Exception) {
                    null
                }
            }
            onDone(file)
            _state.value = _state.value.copy(
                message = if (file != null) "Saved to ${file.parentFile?.name}/${file.name}"
                else "Export failed",
            )
        }
    }

    override fun onCleared() {
        SvEngine.releaseSegmenter()
        SvEngine.release()
        super.onCleared()
    }
}
