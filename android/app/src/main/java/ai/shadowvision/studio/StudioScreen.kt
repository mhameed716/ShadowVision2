// SPDX-License-Identifier: MIT
package ai.shadowvision.studio

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import ai.shadowvision.core.MaskSurface
import kotlin.math.roundToInt

/**
 * The User Studio: import -> mask -> remove -> compare -> export.
 *
 * The screen is organised so that the *image is never touched by Compose*. The
 * canvas is an AndroidView hosting MaskSurface; Compose owns chrome only. That
 * split is what keeps a 12 MP edit at 60 fps of chrome and zero GC churn (D8).
 */
@Composable
fun StudioScreen(vm: StudioViewModel, onOpenConsole: () -> Unit) {
    val state by vm.state.collectAsStateWithLifecycle()
    val context = LocalContext.current

    var surface by remember { mutableStateOf<MaskSurface?>(null) }
    var zoom by remember { mutableStateOf(1f) }
    var radius by remember { mutableStateOf(24f) }
    var showCoverage by remember { mutableStateOf(0f) }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia()
    ) { uri ->
        if (uri != null) vm.importImage(uri)
    }

    // Hand the decoded bitmap to the surface once per import.
    LaunchedEffect(state.image) {
        val s = surface ?: return@LaunchedEffect
        state.image?.let { s.setImage(it) }
    }

    // Segmentation results arrive asynchronously; offer them to the canvas lazily.
    LaunchedEffect(state.selectionVersion) {
        val s = surface ?: return@LaunchedEffect
        val pending = vm.pendingSegMask
        if (pending != null) {
            s.offerSelection(pending, state.image?.width ?: 0, state.image?.height ?: 0)
            vm.pendingSegMask = null
        }
    }

    DisposableEffect(Unit) {
        onDispose { surface?.recycle() }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(SvColors.Background)
    ) {
        // ---- top bar ----
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("ShadowVision", style = MaterialTheme.typography.headlineSmall, color = SvColors.Text)
                Text(
                    "engine: ${state.engineMode}",
                    style = MaterialTheme.typography.labelSmall,
                )
                Text(
                    "select: ${state.segmenterMode} · tap ${state.segmenterLatencyMs}",
                    style = MaterialTheme.typography.labelSmall,
                )
            }
            if (state.image != null) {
                TextButton(onClick = { vm.revertToOriginal() }, enabled = state.resultAvailable) {
                    Text("Revert", color = if (state.resultAvailable) SvColors.TextDim else SvColors.Outline)
                }
            }
            TextButton(onClick = onOpenConsole) {
                Text("Engine ⌁", color = SvColors.Accent)
            }
        }

        // ---- canvas ----
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .padding(horizontal = 12.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(SvColors.Surface)
                .border(1.dp, SvColors.Outline, RoundedCornerShape(14.dp))
                .pointerInput(Unit) {
                    // Pinch = zoom, two-finger drag = pan (handled by the surface's
                    // matrix via setZoom; pan is intentionally a later refinement).
                    detectTransformGestures { _, _, gestureZoom, _ ->
                        zoom = (zoom * gestureZoom).coerceIn(1f, 6f)
                        surface?.zoom = zoom
                    }
                },
        ) {
            val image = state.image
            if (image == null) {
                EmptyState(onPick = {
                    picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                })
            } else {
                val shown = if (state.showBefore) state.originalForCompare ?: image else image
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { ctx ->
                        MaskSurface(ctx).also { s ->
                            s.listener = object : MaskSurface.Listener {
                                override fun onMaskChanged(hasMask: Boolean, coveragePct: Float) {
                                    vm.onMaskChanged(hasMask, coveragePct)
                                    showCoverage = coveragePct
                                    vm.onUndoState(s.canUndo(), s.canRedo())
                                }

                                override fun onTap(x: Int, y: Int) {
                                    // The tap is in image pixels of this bitmap, and
                                    // the ViewModel embeds exactly this bitmap, so
                                    // it needs nothing else. The old call passed
                                    // four arguments (including a null mask) to a
                                    // function that took five — it did not compile.
                                    if (vm.state.value.tool == StudioViewModel.Tool.SMART_SELECT) {
                                        vm.smartSelect(x, y)
                                    }
                                }
                            }
                            s.zoom = zoom
                            s.brushRadius = radius
                            s.setImage(image)
                            surface = s
                        }
                    },
                    update = { s ->
                        s.brushRadius = radius
                        s.erasing = state.tool == StudioViewModel.Tool.ERASER
                        if (s.zoom != zoom) s.zoom = zoom
                    },
                )
                if (!state.segmentationReady && !state.busy) {
                    Box(
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(10.dp)
                            .clip(RoundedCornerShape(8.dp))
                            .background(SvColors.SurfaceHigh)
                            .padding(horizontal = 10.dp, vertical = 6.dp),
                    ) {
                        Text(
                            "brush only — smart select unavailable",
                            style = MaterialTheme.typography.labelSmall,
                            color = SvColors.Warning,
                        )
                    }
                }
                // Comparison latch: press and hold "Before".
                if (state.resultAvailable) {
                    Box(
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(10.dp),
                    ) {
                        TextButton(
                            onClick = { vm.setCompare(!state.showBefore) },
                            colors = ButtonDefaults.textButtonColors(contentColor = SvColors.Accent),
                        ) {
                            Text(if (state.showBefore) "Showing: BEFORE" else "Before / After")
                        }
                    }
                }
            }

            if (state.busy) {
                Column(
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .fillMaxWidth()
                        .padding(16.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .background(SvColors.SurfaceHigh)
                        .padding(14.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(16.dp),
                            strokeWidth = 2.dp,
                            color = SvColors.Accent,
                        )
                        Spacer(Modifier.width(10.dp))
                        Text(
                            when (state.stage) {
                                "decoding" -> "Decoding image…"
                                "analysing" -> "Analysing for smart select…"
                                "removing" -> "Removing object on device…"
                                else -> "Working…"
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = SvColors.Text,
                        )
                    }
                    Spacer(Modifier.height(8.dp))
                    LinearProgressIndicator(
                        progress = { state.progress.coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth(),
                        color = SvColors.Accent,
                        trackColor = SvColors.Outline,
                    )
                }
            }
        }

        // ---- tool bar ----
        if (state.image != null) {
            ToolBar(
                tool = state.tool,
                erasing = state.tool == StudioViewModel.Tool.ERASER,
                onTool = { t ->
                    vm.setTool(t)
                    surface?.erasing = t == StudioViewModel.Tool.ERASER
                },
                canUndo = state.canUndo,
                canRedo = state.canRedo,
                onUndo = { surface?.undo() },
                onRedo = { surface?.redo() },
                onClear = { surface?.clearMask() },
                coverage = showCoverage,
            )

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Brush", style = MaterialTheme.typography.labelSmall)
                Slider(
                    value = radius,
                    onValueChange = {
                        radius = it
                        surface?.brushRadius = it
                        vm.setBrushRadius(it)
                    },
                    valueRange = 6f..120f,
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 12.dp),
                    colors = SliderDefaults.colors(
                        thumbColor = SvColors.Text,
                        activeTrackColor = SvColors.TextDim,
                    ),
                )
                Text("${radius.roundToInt()}px", style = MaterialTheme.typography.labelSmall)
            }

            // ---- primary action ----
            Button(
                onClick = {
                    val img = state.image
                    val m = surface?.exportMask()
                    if (img != null && m != null) vm.remove(img, m)
                },
                enabled = state.hasMask && !state.busy,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 10.dp)
                    .height(52.dp),
                shape = RoundedCornerShape(12.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = SvColors.Accent,
                    contentColor = SvColors.Background,
                    disabledContainerColor = SvColors.SurfaceHigh,
                    disabledContentColor = SvColors.TextDim,
                ),
            ) {
                Text(
                    if (state.hasMask) "Remove selection" else "Paint or tap an object",
                    fontWeight = FontWeight.SemiBold,
                )
            }

            if (state.resultAvailable) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp)
                        .padding(bottom = 12.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    OutlinedAction("Export PNG", Modifier.weight(1f)) {
                        state.image?.let { bmp ->
                            // export() reports through the state message, which the
                            // studio already renders; the callback is a hook for
                            // future sharing and must not be omitted (it is a
                            // required parameter, so the old `vm.export(bmp) {}`
                            // with one argument did not compile).
                            vm.export(bmp) { _ -> }
                        }
                    }
                    OutlinedAction("Before / After", Modifier.weight(1f)) {
                        vm.setCompare(!state.showBefore)
                    }
                }
            }
            // Mask coverage hint: cheap feedback that the mask is what the user thinks.
            if (state.hasMask) {
                Text(
                    "mask covers ${String.format("%.1f", state.maskCoverage)}% of the frame",
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.padding(start = 18.dp, bottom = 10.dp),
                )
            }
        }
    }

    state.message?.let { msg ->
        LaunchedEffect(msg) {
            kotlinx.coroutines.delay(4000)
            vm.consumeMessage()
        }
    }
}

@Composable
private fun OutlinedAction(label: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = modifier.height(44.dp),
        shape = RoundedCornerShape(10.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = SvColors.SurfaceHigh,
            contentColor = SvColors.Text,
        ),
    ) { Text(label, style = MaterialTheme.typography.bodyMedium) }
}

@Composable
private fun ToolBar(
    tool: StudioViewModel.Tool,
    erasing: Boolean,
    onTool: (StudioViewModel.Tool) -> Unit,
    canUndo: Boolean,
    canRedo: Boolean,
    onUndo: () -> Unit,
    onRedo: () -> Unit,
    onClear: () -> Unit,
    coverage: Float,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ToolChip("Brush", tool == StudioViewModel.Tool.BRUSH && !erasing) {
            onTool(StudioViewModel.Tool.BRUSH)
        }
        ToolChip("Smart", tool == StudioViewModel.Tool.SMART_SELECT) {
            onTool(StudioViewModel.Tool.SMART_SELECT)
        }
        ToolChip("Erase", tool == StudioViewModel.Tool.ERASER) {
            onTool(StudioViewModel.Tool.ERASER)
        }
        Spacer(Modifier.weight(1f))
        TextButton(onClick = onUndo, enabled = canUndo) {
            Text("↶", color = if (canUndo) SvColors.Text else SvColors.Outline)
        }
        TextButton(onClick = onRedo, enabled = canRedo) {
            Text("↷", color = if (canRedo) SvColors.Text else SvColors.Outline)
        }
        TextButton(onClick = onClear) {
            Text("Clear", color = SvColors.TextDim, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun ToolChip(label: String, selected: Boolean, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        shape = RoundedCornerShape(9.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (selected) SvColors.Accent else SvColors.SurfaceHigh,
            contentColor = if (selected) SvColors.Background else SvColors.TextDim,
        ),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 14.dp, vertical = 8.dp),
    ) {
        Text(label, style = MaterialTheme.typography.bodySmall, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun EmptyState(onPick: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("Nothing imported yet", style = MaterialTheme.typography.titleMedium, color = SvColors.Text)
        Spacer(Modifier.height(6.dp))
        Text(
            "Photos never leave this device.\nNo account, no upload, no telemetry.",
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(20.dp))
        Button(
            onClick = onPick,
            shape = RoundedCornerShape(12.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = SvColors.Accent,
                contentColor = SvColors.Background,
            ),
        ) { Text("Import a photo", fontWeight = FontWeight.SemiBold) }
    }
}
