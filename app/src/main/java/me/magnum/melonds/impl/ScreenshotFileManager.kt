package me.magnum.melonds.impl

import android.content.Context
import android.graphics.Bitmap
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import me.magnum.melonds.common.uridelegates.UriHandler
import me.magnum.melonds.domain.model.rom.Rom
import me.magnum.melonds.domain.repositories.SettingsRepository
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import javax.inject.Inject

class ScreenshotFileManager @Inject constructor(
    @param:ApplicationContext private val context: Context,
    private val settingsRepository: SettingsRepository,
    private val uriHandler: UriHandler,
) {
    fun save(rom: Rom, screenshot: Bitmap): String {
        val directoryUri = settingsRepository.getScreenshotDirectory()
        val directory = if (directoryUri != null) {
            uriHandler.getUriTreeDocument(directoryUri)
        } else {
            val folder = File(context.getExternalFilesDir(null) ?: context.filesDir, "screenshots")
            if (!folder.isDirectory && !folder.mkdirs()) {
                throw IOException("Cannot create screenshot directory")
            }
            DocumentFile.fromFile(folder)
        } ?: throw IOException("Screenshot directory unavailable")
        if (!directory.isDirectory || !directory.canWrite()) {
            throw IOException("Screenshot directory is not writable")
        }
        val romName = rom.fileName.substringBeforeLast('.').replace(Regex("[\\\\/:*?\"<>|\\p{Cntrl}]"), "_")
            .trim().trim('.').take(120).ifEmpty { "ROM" }
        val timestamp = SimpleDateFormat("yyyy-MM-dd_HH-mm-ss-SSS", Locale.ROOT).format(Date())
        val name = "$romName - $timestamp.png"
        if (directory.findFile(name) != null) {
            throw IOException("Screenshot already exists")
        }
        val documentName = if (directory.uri.scheme == "file") name.removeSuffix(".png") else name
        val document = directory.createFile("image/png", documentName)
            ?: throw IOException("Cannot create screenshot")
        val stream = context.contentResolver.openOutputStream(document.uri, "w")
            ?: throw IOException("Cannot open screenshot output")
        stream.use {
            if (!screenshot.compress(Bitmap.CompressFormat.PNG, 100, it)) {
                throw IOException("Cannot encode screenshot")
            }
        }
        return document.name ?: name
    }
}
