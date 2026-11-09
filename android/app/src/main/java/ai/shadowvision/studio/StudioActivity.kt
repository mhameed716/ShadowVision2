// SPDX-License-Identifier: MIT
package ai.shadowvision.studio

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import ai.shadowvision.core.SvEngine

/**
 * Single activity, two interfaces (D-architecture §3):
 *   Studio  — the user's editor
 *   Console — the engine/dev panel, reachable only from the studio header
 *
 * No bottom navigation, no tabs: the console is a tool, not a destination.
 */
class StudioActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            SvTheme {
                Box(Modifier.fillMaxSize().background(SvColors.Background)) {
                    Root()
                }
            }
        }
    }

    @Composable
    private fun Root() {
        val vm: StudioViewModel = viewModel()
        var route by rememberSaveable { mutableStateOf("studio") }
        when (route) {
            "studio" -> StudioScreen(vm = vm, onOpenConsole = { route = "console" })
            else -> ConsoleScreen(onBack = { route = "studio" })
        }
    }

    override fun onDestroy() {
        // Release native sessions when the activity really goes away; the
        // ViewModel also does it, but a hard exit (task swipe) skips onCleared.
        SvEngine.releaseSegmenter()
        SvEngine.release()
        super.onDestroy()
    }
}
