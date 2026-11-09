// SPDX-License-Identifier: MIT
package ai.shadowvision.studio

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/**
 * Dark, neutral, "pro tool" palette. Deliberately desaturated: the only strong
 * colour in the whole app is the mask red, because the mask is the thing the user
 * is manipulating. Any other accent competes with the image.
 */
object SvColors {
    val Background = Color(0xFF0E0F12)
    val Surface = Color(0xFF15171C)
    val SurfaceHigh = Color(0xFF1D2027)
    val Outline = Color(0xFF2A2E37)
    val Text = Color(0xFFE8EAED)
    val TextDim = Color(0xFF9AA0AA)
    val Accent = Color(0xFF6EA8FE)      // actions
    val Mask = Color(0xFFFF5C5C)        // the mask, and nothing else
    val Success = Color(0xFF6FD08C)
    val Warning = Color(0xFFE8B04B)
    val Danger = Color(0xFFEA5B5B)
}

private val svScheme = darkColorScheme(
    primary = SvColors.Accent,
    onPrimary = Color(0xFF06121F),
    background = SvColors.Background,
    onBackground = SvColors.Text,
    surface = SvColors.Surface,
    onSurface = SvColors.Text,
    surfaceVariant = SvColors.SurfaceHigh,
    onSurfaceVariant = SvColors.TextDim,
    outline = SvColors.Outline,
    error = SvColors.Danger,
)

private val svTypography = Typography(
    headlineSmall = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold),
    titleMedium = TextStyle(fontSize = 16.sp, fontWeight = FontWeight.Medium),
    bodyMedium = TextStyle(fontSize = 14.sp),
    bodySmall = TextStyle(fontSize = 12.sp, color = SvColors.TextDim),
    labelSmall = TextStyle(fontSize = 11.sp, letterSpacing = 0.4.sp, color = SvColors.TextDim),
)

@Composable
fun SvTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = svScheme, typography = svTypography, content = content)
}
