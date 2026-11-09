// SPDX-License-Identifier: MIT
package ai.shadowvision.core

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Rect
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * The brush surface: the one place in this app where a custom View is correct.
 *
 * Why not Compose Canvas (D8): every invalidate() would redraw the whole image
 * layer; at 12 MP that is ~48 MB of work per frame, which produces GC stutter
 * and then OOM. Here the image is drawn once per transform change and the mask
 * lives in a *downscaled* Bitmap that is only repainted over the touched rect.
 *
 * Memory model (deliberate):
 *  - mask is stored at [maskScale] (default 1/2 of the image) as an ALPHA_8-like
 *    ARGB bitmap. Half resolution is invisible for inpainting and costs 4x less.
 *  - strokes are never stored as bitmaps; undo works on saved/restored mask
 *    snapshots taken at stroke *boundaries*, capped at [MAX_UNDO].
 */
class MaskSurface(context: Context) : View(context) {

    interface Listener {
        /** Called when the mask content changed (for enabling the "remove" button). */
        fun onMaskChanged(hasMask: Boolean, coveragePct: Float)
        /** Called on a tap with no drag: a candidate segmentation prompt. */
        fun onTap(x: Int, y: Int)
    }

    var listener: Listener? = null

    /** 0.02..8.0; 1.0 = fit. */
    var zoom: Float = 1f
        set(value) {
            field = value.coerceIn(0.5f, 8f)
            updateMatrix()
            invalidate()
        }

    /**
     * Brush radius in IMAGE pixels. The name the UI uses is [brushRadius]; it was
     * only ever called brushRadiusPx here while every caller wrote
     * `surface.brushRadius = ...`, which does not compile. One name, one setter.
     */
    var brushRadius: Float = 24f
        set(value) {
            field = value.coerceIn(2f, 400f)
            invalidate()
        }

    var erasing: Boolean = false
        set(value) {
            field = value
            invalidate()
        }

    private var image: Bitmap? = null
    private var mask: Bitmap? = null
    private var maskScale = 2                 // mask pixels per image pixel divisor
    private var offsetX = 0f
    private var offsetY = 0f
    private val drawMatrix = Matrix()
    private var lastX = 0f
    private var lastY = 0f
    private var dragging = false
    private var downX = 0f
    private var downY = 0f
    private var downTime = 0L
    private var maskStrokeCount = 0

    private val undoStack = ArrayDeque<Bitmap>()
    private val redoStack = ArrayDeque<Bitmap>()
    private val MAX_UNDO = 6

    private val imagePaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val maskPaint = Paint().apply {
        color = Color.rgb(255, 92, 92)
        alpha = 150
        style = Paint.Style.FILL
    }
    private val maskBlitPaint = Paint()
    private val brushPaint = Paint().apply {
        isAntiAlias = true
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Color.WHITE
    }
    private val eraserPaint = Paint().apply {
        isAntiAlias = true
        style = Paint.Style.STROKE
        xfermode = PorterDuffXfermode(PorterDuff.Mode.CLEAR)
    }

    // ---- content -------------------------------------------------------

    fun setImage(bitmap: Bitmap) {
        image = bitmap
        mask?.recycle()
        mask = null
        undoStack.forEach { it.recycle() }
        undoStack.clear()
        redoStack.clear()
        zoom = 1f
        updateMatrix()
        invalidate()
        listener?.onMaskChanged(false, 0f)
    }

    fun currentImage(): Bitmap? = image

    /** Lazily creates the mask at reduced resolution; returns the raw mask bitmap. */
    fun ensureMask(): Bitmap? {
        val img = image ?: return null
        val m = mask
        if (m != null) return m
        val w = max(1, img.width / maskScale)
        val h = max(1, img.height / maskScale)
        val created = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        mask = created
        return created
    }

    /** Full-resolution 8-bit mask for the native pipeline. */
    fun exportMask(): ByteArray? {
        val m = mask ?: return null
        val img = image ?: return null
        val scaled = if (m.width == img.width && m.height == img.height) {
            m
        } else {
            Bitmap.createScaledBitmap(m, img.width, img.height, true)
        }
        val w = scaled.width
        val h = scaled.height
        val pixels = IntArray(w * h)
        scaled.getPixels(pixels, 0, w, 0, 0, w, h)
        val out = ByteArray(w * h)
        for (i in pixels.indices) {
            // Any drawn pixel has alpha > 0; we keep the alpha as mask strength so
            // feathered brush edges survive into the pipeline.
            out[i] = ((pixels[i] ushr 24) and 0xFF).toByte()
        }
        if (scaled !== m) scaled.recycle()
        return out
    }

    fun hasMask(): Boolean = mask != null && maskStrokeCount > 0

    /**
     * True right after a segmentation tap, when a native mask is waiting for the
     * user to accept it. Exposed so the UI can show "Apply selection".
     */
    var hasPendingSelection: Boolean = false
        private set

    /** Lets the activity hand a fresh segmentation result to the surface lazily. */
    fun offerSelection(bytes: ByteArray?, width: Int, height: Int) {
        if (bytes == null) {
            hasPendingSelection = false
            notifyMask()
            return
        }
        setMaskFromBytes(bytes, width, height)
    }

    fun clearMask() {
        pushUndo()
        mask?.eraseColor(Color.TRANSPARENT)
        maskStrokeCount = 0
        hasPendingSelection = false
        invalidate()
        notifyMask()
    }

    /** Replaces the mask with the result of a segmentation tap. */
    private fun setMaskFromBytes(bytes: ByteArray, width: Int, height: Int) {
        val img = image ?: return
        pushUndo()
        val full = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val pixels = IntArray(width * height)
        for (i in pixels.indices) {
            val a = bytes[i].toInt() and 0xFF
            pixels[i] = (a shl 24) or 0x00FF5C5C
        }
        full.setPixels(pixels, 0, width, 0, 0, width, height)
        val target = if (width == img.width && height == img.height) full else {
            val s = Bitmap.createScaledBitmap(full, max(1, img.width / maskScale),
                max(1, img.height / maskScale), true)
            full.recycle()
            s
        }
        mask?.recycle()
        mask = target
        maskStrokeCount++
        hasPendingSelection = true
        invalidate()
        notifyMask()
    }

    // ---- undo / redo ---------------------------------------------------

    fun canUndo(): Boolean = undoStack.isNotEmpty()
    fun canRedo(): Boolean = redoStack.isNotEmpty()

    fun undo() {
        val prev = undoStack.removeLastOrNull() ?: return
        mask?.let { redoStack.addLast(it.copy(Bitmap.Config.ARGB_8888, false)) }
        if (redoStack.size > MAX_UNDO) redoStack.removeFirst().recycle()
        mask?.recycle()
        mask = prev
        maskStrokeCount = max(0, maskStrokeCount - 1)
        invalidate()
        notifyMask()
    }

    fun redo() {
        val next = redoStack.removeLastOrNull() ?: return
        mask?.let { undoStack.addLast(it.copy(Bitmap.Config.ARGB_8888, false)) }
        mask?.recycle()
        mask = next
        maskStrokeCount++
        invalidate()
        notifyMask()
    }

    private fun pushUndo() {
        val m = mask ?: return
        // Bounded history: each snapshot is mask-sized (1/4 of the image at scale
        // 2), so 6 snapshots of a 12 MP photo is ~18 MB — acceptable, 30 is not.
        undoStack.addLast(m.copy(Bitmap.Config.ARGB_8888, false))
        if (undoStack.size > MAX_UNDO) undoStack.removeFirst().recycle()
        redoStack.forEach { it.recycle() }
        redoStack.clear()
    }

    // ---- transform -----------------------------------------------------

    private fun updateMatrix() {
        val img = image ?: return
        val vw = width.toFloat()
        val vh = height.toFloat()
        if (vw <= 0f || vh <= 0f) return
        val fit = min(vw / img.width, vh / img.height)
        val s = fit * zoom
        offsetX = (vw - img.width * s) / 2f
        offsetY = (vh - img.height * s) / 2f
        drawMatrix.reset()
        drawMatrix.postScale(s, s)
        drawMatrix.postTranslate(offsetX, offsetY)
    }

    /** View -> image pixel coordinates (clamped to the image). */
    fun toImage(x: Float, y: Float): Pair<Int, Int> {
        val img = image ?: return Pair(0, 0)
        val pts = floatArrayOf(x, y)
        val inv = Matrix()
        drawMatrix.invert(inv)
        inv.mapPoints(pts)
        return Pair(
            pts[0].toInt().coerceIn(0, img.width - 1),
            pts[1].toInt().coerceIn(0, img.height - 1),
        )
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        updateMatrix()
    }

    // ---- drawing -------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        val img = image ?: return
        canvas.drawBitmap(img, drawMatrix, imagePaint)
        val m = mask
        if (m != null && maskStrokeCount > 0) {
            canvas.drawBitmap(m, drawMatrix, maskPaint)
        }
        // Brush cursor: shows the real radius in image pixels at current zoom.
        if (lastX > 0 && lastY > 0) {
            val scale = drawMatrix.mapRadius(1f)
            canvas.drawCircle(lastX, lastY, brushRadius * scale, brushPaint)
        }
    }

    // ---- touch ---------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (image == null) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = event.x
                downY = event.y
                downTime = System.currentTimeMillis()
                dragging = false
                lastX = event.x
                lastY = event.y
                // Nothing is painted here. A tap is a segmentation PROMPT, and the
                // old code stamped a brush dot on ACTION_DOWN unconditionally —
                // so "Smart select" both painted a red blob and asked the native
                // decoder for a mask. The stroke is started on the first MOVE that
                // exceeds the drag threshold; undo is pushed at that moment, so a
                // tap never leaves an empty undo step behind either.
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging && hypot(event.x - downX, event.y - downY) > 12f) {
                    dragging = true
                    pushUndo()
                    // Start the stroke where the finger went down, not where it is
                    // now, or the first 12 px of every stroke are missing.
                    strokeAt(downX, downY, event.x, event.y)
                } else if (dragging) {
                    strokeAt(lastX, lastY, event.x, event.y)
                }
                lastX = event.x
                lastY = event.y
                return true
            }
            MotionEvent.ACTION_UP -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                val quick = System.currentTimeMillis() - downTime < 250
                if (!dragging && quick) {
                    // A tap (not a stroke): hand it to segmentation instead.
                    val (ix, iy) = toImage(event.x, event.y)
                    listener?.onTap(ix, iy)
                } else if (dragging) {
                    maskStrokeCount++
                }
                notifyMask()
                invalidate()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    /** Paints a capsule from (x0,y0) to (x1,y1) in the reduced-resolution mask. */
    private fun strokeAt(x0: Float, y0: Float, x1: Float, y1: Float) {
        val m = ensureMask() ?: return
        val canvas = Canvas(m)
        val scale = drawMatrix.mapRadius(1f) / maskScale
        val paint = if (erasing) eraserPaint else maskBlitPaint.apply {
            color = Color.rgb(255, 92, 92)
            alpha = 255
        }
        paint.strokeWidth = max(1f, brushRadius * 2f * scale)
        paint.strokeCap = Paint.Cap.ROUND
        paint.isAntiAlias = true
        val p0 = floatArrayOf(x0, y0)
        val p1 = floatArrayOf(x1, y1)
        val inv = Matrix()
        drawMatrix.invert(inv)
        inv.mapPoints(p0)
        inv.mapPoints(p1)
        p0[0] /= maskScale
        p0[1] /= maskScale
        p1[0] /= maskScale
        p1[1] /= maskScale
        canvas.drawLine(p0[0], p0[1], p1[0], p1[1], paint)
        // Invalidate the dirty rectangle in VIEW coordinates. The previous code
        // replaced the stroke endpoints with the mask-space values and then used
        // them as view coordinates, so at any zoom other than 1 (and with the
        // /2 mask downscale) it repainted the wrong part of the screen.
        val pad = brushRadius * drawMatrix.mapRadius(1f) + 2f
        val rect = Rect(
            (min(x0, x1) - pad).toInt().coerceAtLeast(0),
            (min(y0, y1) - pad).toInt().coerceAtLeast(0),
            (max(x0, x1) + pad).toInt().coerceAtMost(width),
            (max(y0, y1) + pad).toInt().coerceAtMost(height),
        )
        invalidate(rect)
    }

    private fun notifyMask() {
        val has = hasMask()
        listener?.onMaskChanged(has, coveragePct())
    }

    /** Rough coverage estimate from a sparse sample: enough for a UI hint. */
    fun coveragePct(): Float {
        val m = mask ?: return 0f
        val step = max(1, m.width / 64)
        var hit = 0
        var total = 0
        val px = IntArray(1)
        var y = 0
        while (y < m.height) {
            var x = 0
            while (x < m.width) {
                px[0] = 0
                // getPixel is slow; sample sparsely and accept the error in a hint.
                if ((m.getPixel(x, y) ushr 24) > 8) hit++
                total++
                x += step
            }
            y += step
        }
        return if (total == 0) 0f else 100f * hit / total
    }

    fun recycle() {
        mask?.recycle()
        mask = null
        undoStack.forEach { it.recycle() }
        redoStack.forEach { it.recycle() }
        undoStack.clear()
        redoStack.clear()
        image = null
    }
}
