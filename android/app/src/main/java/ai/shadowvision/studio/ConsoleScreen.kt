// SPDX-License-Identifier: MIT
package ai.shadowvision.studio

import android.os.PowerManager
import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import ai.shadowvision.core.ModelCatalog
import ai.shadowvision.core.SvEngine
import kotlinx.coroutines.delay
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Engine Control: the second interface. Local, honest, and small.
 *
 * Two rules learned from the design review (§3.5):
 *  - No "VRAM" gauge: it does not exist on Android. We show process RSS, which is
 *    the number that actually gets the app killed.
 *  - No free-form CFG/steps fields: parameters come from the signed catalog's
 *    schema, so a deterministic model (LaMa) shows no knobs at all.
 */
@Composable
fun ConsoleScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var rssMb by remember { mutableStateOf(0.0) }
    var thermal by remember { mutableStateOf(-1) }
    var accelerator by remember { mutableStateOf("CPU") }
    var catalog by remember { mutableStateOf<ModelCatalog.Result?>(null) }
    var verifying by remember { mutableStateOf<String?>(null) }
    val logs = remember { mutableStateListOf<String>() }
    val listState = rememberLazyListState()
    val stamp = remember { SimpleDateFormat("HH:mm:ss.SSS", Locale.US) }

    fun log(msg: String) {
        if (logs.size > 300) logs.removeAt(0)
        logs.add("${stamp.format(Date())}  $msg")
    }

    LaunchedEffect(Unit) {
        log("engine ${SvEngine.version()}")
        log("budget 1200 MB · single worker thread · zero network")
        catalog = ModelCatalog.load(context)
        log(
            if (catalog?.verified == true) "catalog verified: ${catalog?.entries?.size} entries"
            else "catalog rejected: ${catalog?.reason}"
        )
        catalog?.entries?.forEach { e ->
            val ok = ModelCatalog.verifyFile(context, e)
            log("  ${e.id}  sha256 ${if (ok) "OK" else "MISSING/MISMATCH"}  licence=${e.license}" +
                if (!e.commercial) "  (non-commercial: hidden in release)" else "")
        }
    }

    // 1 Hz sampling: fast enough to see a spike, slow enough to cost nothing.
    LaunchedEffect(Unit) {
        val pm = context.getSystemService(PowerManager::class.java)
        while (true) {
            rssMb = SvEngine.rssMb()
            thermal = SvEngine.thermalStatus(pm)
            delay(1000)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(SvColors.Background)
            .padding(horizontal = 16.dp),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = onBack) { Text("← Studio", color = SvColors.Accent) }
            Text(
                "Engine Control",
                style = MaterialTheme.typography.titleMedium,
                color = SvColors.Text,
                modifier = Modifier.padding(start = 8.dp),
            )
        }

        // ---- resources ----
        Section("Resources") {
            MetricRow("Process RSS", "%.0f MB".format(rssMb), rssMb / 1200.0)
            MetricRow(
                "Thermal",
                thermalLabel(thermal),
                if (thermal < 0) 0.0 else (thermal / 6.0),
                warn = thermal >= PowerManager.THERMAL_STATUS_SEVERE,
            )
            MetricRow("Accelerator", accelerator, 0.0)
        }

        Spacer(Modifier.height(12.dp))

        // ---- models ----
        Section("Models (signed catalog)") {
            val result = catalog
            if (result == null || !result.verified) {
                Text(
                    "No verified catalog. Install models into\n" +
                        "files/models/ and sign with tools/sign_catalog.py",
                    style = MaterialTheme.typography.bodySmall,
                    color = SvColors.Warning,
                )
            } else {
                result.entries.forEach { entry ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(modifier = Modifier.weight(1f)) {
                            Text(entry.id, style = MaterialTheme.typography.bodyMedium, color = SvColors.Text)
                            Text(
                                "${entry.file} · ${entry.license}" +
                                    if (entry.deterministic) " · deterministic" else " · ${entry.params}",
                                style = MaterialTheme.typography.labelSmall,
                            )
                        }
                        Button(
                            onClick = { verifying = entry.id },
                            shape = RoundedCornerShape(8.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = SvColors.SurfaceHigh,
                                contentColor = SvColors.TextDim,
                            ),
                            contentPadding = androidx.compose.foundation.layout.PaddingValues(12.dp, 6.dp),
                        ) { Text("Verify", style = MaterialTheme.typography.labelSmall) }
                    }
                }
            }
        }

        Spacer(Modifier.height(12.dp))

        // ---- accelerator ----
        Section("Accelerator") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AccelChip("CPU", accelerator == "CPU") {
                    accelerator = "CPU"
                    log("accelerator -> CPU (baseline, always available)")
                }
                AccelChip("QNN Auto", accelerator == "QNN Auto") {
                    accelerator = "QNN Auto"
                    log(
                        "accelerator -> QNN Auto: uses the NPU only if a context binary " +
                            "for this SoC is installed; otherwise stays on CPU"
                    )
                }
            }
            Text(
                "GPU is intentionally absent: no Vulkan EP in ONNX Runtime on Android, " +
                    "and NNAPI is deprecated since Android 15.",
                style = MaterialTheme.typography.labelSmall,
                modifier = Modifier.padding(top = 8.dp),
            )
        }

        Spacer(Modifier.height(12.dp))

        // ---- live logs ----
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Live logs", style = MaterialTheme.typography.titleMedium, color = SvColors.Text)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = { logs.clear() }) { Text("clear", color = SvColors.TextDim) }
        }
        LazyColumn(
            state = listState,
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(SvColors.Surface)
                .border(1.dp, SvColors.Outline, RoundedCornerShape(10.dp))
                .padding(10.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            items(logs) { line ->
                Text(
                    line,
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
                    color = if (line.contains("rejected") || line.contains("MISMATCH")) SvColors.Danger
                    else SvColors.TextDim,
                )
            }
        }
        LaunchedEffect(logs.size) {
            if (logs.isNotEmpty()) listState.animateScrollToItem(logs.size - 1)
        }
        Spacer(Modifier.height(12.dp))
    }

    // Verification result surfaces in the log, not as a dialog: this is a console.
    LaunchedEffect(verifying) {
        val id = verifying ?: return@LaunchedEffect
        val entry = catalog?.entries?.firstOrNull { it.id == id }
        if (entry != null) {
            val ok = ModelCatalog.verifyFile(context, entry)
            log("verify $id: ${if (ok) "OK (size + sha256 match)" else "FAILED (missing or altered file)"}")
        }
        verifying = null
    }
}

@Composable
private fun Section(title: String, content: @Composable () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(SvColors.Surface)
            .border(1.dp, SvColors.Outline, RoundedCornerShape(12.dp))
            .padding(14.dp),
    ) {
        Text(title, style = MaterialTheme.typography.labelSmall, color = SvColors.TextDim)
        Spacer(Modifier.height(8.dp))
        content()
    }
}

@Composable
private fun MetricRow(label: String, value: String, fraction: Double, warn: Boolean = false) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = MaterialTheme.typography.bodySmall, modifier = Modifier.width(110.dp))
        if (fraction > 0) {
            LinearProgressIndicator(
                progress = { fraction.coerceIn(0.0, 1.0).toFloat() },
                modifier = Modifier
                    .weight(1f)
                    .height(6.dp)
                    .clip(RoundedCornerShape(3.dp)),
                color = if (warn) SvColors.Danger else SvColors.Accent,
                trackColor = SvColors.Outline,
            )
            Spacer(Modifier.width(10.dp))
        } else {
            Spacer(Modifier.weight(1f))
        }
        Text(
            value,
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            color = if (warn) SvColors.Danger else SvColors.Text,
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun AccelChip(label: String, selected: Boolean, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        shape = RoundedCornerShape(9.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = if (selected) SvColors.Accent else SvColors.SurfaceHigh,
            contentColor = if (selected) SvColors.Background else SvColors.TextDim,
        ),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(14.dp, 8.dp),
    ) { Text(label, style = MaterialTheme.typography.bodySmall) }
}

private fun thermalLabel(status: Int): String = when (status) {
    PowerManager.THERMAL_STATUS_NONE -> "nominal"
    PowerManager.THERMAL_STATUS_LIGHT -> "light"
    PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
    PowerManager.THERMAL_STATUS_SEVERE -> "severe"
    PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
    PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
    PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
    else -> "unknown"
}
