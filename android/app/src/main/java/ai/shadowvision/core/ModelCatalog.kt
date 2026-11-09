// SPDX-License-Identifier: MIT
package ai.shadowvision.core

import android.content.Context
import org.json.JSONArray
import java.io.File

/**
 * Signed model catalog (D10).
 *
 * The app never trusts a file because it is present in app storage: it trusts a
 * file because a catalog that verifies against [DEV_PUBLIC_KEY] says so, and the
 * file's SHA-256 still matches. On a rooted device or after a backup restore,
 * "file is there" is attacker-controlled; "file matches a signed hash" is not.
 *
 * The private key lives offline (tools/sign_catalog.py); only the public key is
 * compiled in here. Rotating the key means shipping an app update — which is the
 * correct cost for a trust anchor.
 */
object ModelCatalog {

    /** Development key (keys/dev.pub). Replace with the release key before shipping. */
    const val DEV_PUBLIC_KEY = "f978de86bdb5491a2c29506facaf67c27d898b2833f61a377c9735c0ef36b31a"

    data class Entry(
        val id: String,
        val file: String,
        val bytes: Long,
        val license: String,
        val commercial: Boolean,
        val inputW: Int,
        val inputH: Int,
        val params: String,
        val steps: List<Int>,
        val cfg: List<Float>,
        val seed: Boolean,
    ) {
        /** True when the model exposes no knobs at all (LaMa): the UI must not render any. */
        val deterministic: Boolean get() = params == "none"
    }

    data class Result(val verified: Boolean, val reason: String, val entries: List<Entry>)

    fun modelsDir(ctx: Context): File = File(ctx.filesDir, "models").apply { mkdirs() }

    /**
     * Verifies and loads the catalog shipped in assets (or one dropped into
     * filesDir for testing). Returns a Result rather than throwing: a bad catalog
     * must degrade to "no models", never to a crash on launch.
     */
    fun load(ctx: Context, publicKeyHex: String = DEV_PUBLIC_KEY): Result {
        val dir = modelsDir(ctx)
        val fromFiles = File(dir, "catalog.signed.txt")
        val manifest = if (fromFiles.exists()) {
            fromFiles
        } else {
            // Copy the asset once so the native verifier sees a real path and the
            // signature covers the same bytes in every install.
            val out = File(dir, "catalog.signed.txt")
            try {
                ctx.assets.open("catalog.signed.txt").use { input ->
                    out.outputStream().use { input.copyTo(it) }
                }
            } catch (e: Exception) {
                return Result(false, "no catalog: ${e.message}", emptyList())
            }
            out
        }

        val json = try {
            SvEngine.catalogLoad(manifest.absolutePath, publicKeyHex)
        } catch (e: Exception) {
            return Result(false, "native verifier failed: ${e.message}", emptyList())
        }

        return try {
            val arr = JSONArray(json)
            val entries = (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                Entry(
                    id = o.getString("id"),
                    file = o.getString("file"),
                    bytes = o.optLong("bytes", 0),
                    license = o.optString("license", "unknown"),
                    commercial = o.optBoolean("commercial", false),
                    inputW = o.optInt("input_w", 512),
                    inputH = o.optInt("input_h", 512),
                    params = o.optString("params", "none"),
                    steps = o.optJSONArray("steps")?.let { a -> (0 until a.length()).map { a.getInt(it) } } ?: emptyList(),
                    cfg = o.optJSONArray("cfg")?.let { a -> (0 until a.length()).map { a.getDouble(it).toFloat() } } ?: emptyList(),
                    seed = o.optBoolean("seed", false),
                )
            }
            Result(true, "ok", entries)
        } catch (e: Exception) {
            // A JSON parse failure here means the native side refused to verify.
            Result(false, SvEngine.catalogError(), emptyList())
        }
    }

    /** Size + SHA-256 check for one entry. Cheap enough to run per launch. */
    fun verifyFile(ctx: Context, entry: Entry): Boolean =
        SvEngine.catalogVerify(modelsDir(ctx).absolutePath, entry.id) == SvEngine.OK

    fun fileFor(ctx: Context, entry: Entry): File = File(modelsDir(ctx), entry.file)

    /** Where a model actually lives: verified files only, never arbitrary paths. */
    fun pathFor(ctx: Context, entry: Entry): String? {
        val f = fileFor(ctx, entry)
        return if (f.exists() && verifyFile(ctx, entry)) f.absolutePath else null
    }
}
