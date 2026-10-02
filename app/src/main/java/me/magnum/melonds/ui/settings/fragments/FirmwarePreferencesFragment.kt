package me.magnum.melonds.ui.settings.fragments

import android.content.Context
import android.net.Uri
import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import androidx.core.content.edit
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import androidx.preference.Preference
import androidx.preference.PreferenceManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import androidx.preference.PreferenceFragmentCompat
import dagger.hilt.android.AndroidEntryPoint
import me.magnum.melonds.R
import me.magnum.melonds.common.DirectoryAccessValidator
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.ui.settings.PreferenceFragmentHelper
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.preferences.StoragePickerPreference
import javax.inject.Inject

@AndroidEntryPoint
class FirmwarePreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {
    private val helper by lazy { PreferenceFragmentHelper(this, uriPermissionManager, directoryAccessValidator) }
    @Inject lateinit var uriPermissionManager: UriPermissionManager
    @Inject lateinit var directoryAccessValidator: DirectoryAccessValidator

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_internal_firmware_settings, rootKey)
        helper.bindPreferenceSummaryToValue(findPreference("firmware_settings_birthday"))
        helper.bindPreferenceSummaryToValue(findPreference("internal_mac_address"))

        hideDependentsWhenInactive("internal_randomize_mac_address", "internal_mac_address", showWhenChecked = false)
        val storagePreference = findPreference<StoragePickerPreference>("wfc_settings_dir")!!
        helper.setupStoragePickerPreference(storagePreference) { uri, persistDirectory ->
            val context = requireContext().applicationContext
            lifecycleScope.launch {
                storagePreference.isEnabled = false
                val result = withContext(Dispatchers.IO) { runCatching { prepareWfcFile(context, uri) } }
                if (!isAdded) return@launch
                storagePreference.isEnabled = true
                result.onSuccess { file ->
                    persistDirectory()
                    PreferenceManager.getDefaultSharedPreferences(context).edit {
                        putString("wfc_settings_file", file.toString())
                    }
                }.onFailure {
                    AlertDialog.Builder(requireContext())
                        .setTitle(R.string.error_invalid_directory)
                        .setMessage(R.string.wfc_settings_error)
                        .setPositiveButton(R.string.ok, null)
                        .show()
                }
            }
        }
        findPreference<Preference>("wfc_settings_reset")!!.setOnPreferenceClickListener {
            PreferenceManager.getDefaultSharedPreferences(requireContext()).edit {
                remove("wfc_settings_file")
            }
            storagePreference.updatePersistedDirectories(emptySet())
            true
        }

    }

    private fun prepareWfcFile(context: Context, uri: Uri): Uri {
        val directory = requireNotNull(DocumentFile.fromTreeUri(context, uri))
        require(directory.isDirectory && directory.canWrite())
        val existing = directory.findFile("wfcsettings.bin")
        if (existing != null) {
            require(existing.isFile && existing.canRead() && existing.canWrite())
            require(existing.length() == 0L || existing.length() == 2304L)
            return existing.uri
        }
        val internal = File(context.filesDir, "wfcsettings.bin")
        val initial = if (internal.exists()) {
            require(internal.length() == 2304L)
            internal.inputStream().use { input ->
                ByteArray(2304).also { bytes ->
                    java.io.DataInputStream(input).readFully(bytes)
                    require(input.read() == -1)
                }
            }
        } else byteArrayOf()
        val file = requireNotNull(directory.createFile("application/octet-stream", "wfcsettings.bin"))
        require(file.name == "wfcsettings.bin")
        requireNotNull(context.contentResolver.openOutputStream(file.uri, "wt")).use { it.write(initial) }
        return file.uri
    }

    override fun getTitle() = getString(R.string.internal_firmware_settings)
}