package me.magnum.melonds.ui.settings.fragments

import android.app.TimePickerDialog
import android.os.Build
import android.os.Bundle
import android.text.format.DateFormat
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.activity.result.contract.ActivityResultContracts
import androidx.lifecycle.lifecycleScope
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.PreferenceManager
import androidx.preference.SwitchPreference
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.R
import me.magnum.melonds.common.DirectoryAccessValidator
import me.magnum.melonds.common.UriPermissionManager
import me.magnum.melonds.domain.repositories.SettingsRepository
import me.magnum.melonds.impl.SettingsBackupManager
import me.magnum.melonds.ui.settings.PreferenceFragmentHelper
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.preferences.StoragePickerPreference
import java.util.Calendar
import java.util.Locale
import javax.inject.Inject
import kotlin.math.abs

@AndroidEntryPoint
class SystemPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    @Inject lateinit var uriPermissionManager: UriPermissionManager
    @Inject lateinit var directoryAccessValidator: DirectoryAccessValidator
    @Inject lateinit var settingsBackupManager: SettingsBackupManager
    @Inject lateinit var settingsRepository: SettingsRepository
    private val helper by lazy { PreferenceFragmentHelper(this, uriPermissionManager, directoryAccessValidator) }
    private var updatingMirrorPreference = false
    private lateinit var clockPreference: Preference

    private val backupInternalLayoutLauncher = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            lifecycleScope.launch(Dispatchers.IO) {
                runCatching { settingsBackupManager.backupInternalLayout(uri) }
                    .onSuccess {
                        withContext(Dispatchers.Main) {
                            AlertDialog.Builder(requireContext())
                                .setMessage(R.string.internal_layout_backup_success)
                                .setPositiveButton(android.R.string.ok, null)
                                .show()
                        }
                    }
                    .onFailure {
                        withContext(Dispatchers.Main) {
                            Toast.makeText(requireContext(), R.string.internal_layout_backup_error, Toast.LENGTH_SHORT).show()
                        }
                    }
            }
        }
    }

    private val backupExternalLayoutLauncher = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            lifecycleScope.launch(Dispatchers.IO) {
                runCatching { settingsBackupManager.backupExternalLayout(uri) }
                    .onSuccess {
                        withContext(Dispatchers.Main) {
                            AlertDialog.Builder(requireContext())
                                .setMessage(R.string.external_layout_backup_success)
                                .setPositiveButton(android.R.string.ok, null)
                                .show()
                        }
                    }
                    .onFailure {
                        withContext(Dispatchers.Main) {
                            Toast.makeText(requireContext(), R.string.external_layout_backup_error, Toast.LENGTH_SHORT).show()
                        }
                    }
            }
        }
    }

    private val restoreInternalLayoutLauncher = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            lifecycleScope.launch(Dispatchers.IO) {
                runCatching { settingsBackupManager.restoreInternalLayout(uri) }
                    .onSuccess {
                        withContext(Dispatchers.Main) {
                            AlertDialog.Builder(requireContext())
                                .setMessage(R.string.internal_layout_restore_success)
                                .setPositiveButton(android.R.string.ok, null)
                                .show()
                        }
                    }
                    .onFailure {
                        withContext(Dispatchers.Main) {
                            Toast.makeText(requireContext(), R.string.internal_layout_restore_error, Toast.LENGTH_SHORT).show()
                        }
                    }
            }
        }
    }

    private val restoreExternalLayoutLauncher = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            lifecycleScope.launch(Dispatchers.IO) {
                runCatching { settingsBackupManager.restoreExternalLayout(uri) }
                    .onSuccess {
                        withContext(Dispatchers.Main) {
                            AlertDialog.Builder(requireContext())
                                .setMessage(R.string.external_layout_restore_success)
                                .setPositiveButton(android.R.string.ok, null)
                                .show()
                        }
                    }
                    .onFailure {
                        withContext(Dispatchers.Main) {
                            Toast.makeText(requireContext(), R.string.external_layout_restore_error, Toast.LENGTH_SHORT).show()
                        }
                    }
            }
        }
    }

    override fun getTitle() = getString(R.string.category_system)

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_system, rootKey)
        clockPreference = findPreference("rtc_offset_minutes")!!
        updateClockPreference()
        clockPreference.setOnPreferenceClickListener {
            showClockPicker()
            true
        }
        clockPreference.sharedPreferences?.registerOnSharedPreferenceChangeListener(sharedPreferenceChangeListener)

        val jitPreference = findPreference<SwitchPreference>("enable_jit")!!
        val mirrorPreference = findPreference<SwitchPreference>("save_internal_config_as_file")!!
        val dldiDirectoryPreference = findPreference<StoragePickerPreference>("system_dldi_sd_card_dir")!!

        if (Build.SUPPORTED_64_BIT_ABIS.isEmpty()) {
            jitPreference.isChecked = false
            jitPreference.isVisible = false
        }

        helper.setupStoragePickerPreference(dldiDirectoryPreference)
        helper.bindPreferenceSummaryToValue(findPreference("system_dldi_sd_card_image_size"))

        val frameskipModePreference = findPreference<ListPreference>("frameskip_mode")!!
        val frameskipManualPreference = findPreference<ListPreference>("frameskip_manual_value")!!
        helper.bindPreferenceSummaryToValue(frameskipManualPreference)
        frameskipManualPreference.isVisible = frameskipModePreference.value == "manual"
        frameskipModePreference.setOnPreferenceChangeListener { _, newValue ->
            frameskipManualPreference.isVisible = newValue == "manual"
            true
        }
        updateFrameskipDrsExclusion()

        mirrorPreference.setOnPreferenceChangeListener { _, newValue ->
            if (updatingMirrorPreference) {
                return@setOnPreferenceChangeListener true
            }
            if (newValue != true) {
                return@setOnPreferenceChangeListener true
            }

            enableSettingsMirror(mirrorPreference)
            false
        }

        findPreference<Preference>("backup_internal_layout")?.setOnPreferenceClickListener {
            backupInternalLayoutLauncher.launch(null)
            true
        }
        findPreference<Preference>("backup_external_layout")?.setOnPreferenceClickListener {
            backupExternalLayoutLauncher.launch(null)
            true
        }
        findPreference<Preference>("restore_internal_layout")?.setOnPreferenceClickListener {
            restoreInternalLayoutLauncher.launch(null)
            true
        }
        findPreference<Preference>("restore_external_layout")?.setOnPreferenceClickListener {
            restoreExternalLayoutLauncher.launch(null)
            true
        }
    }

    override fun onResume() {
        super.onResume()
        updateFrameskipDrsExclusion()
        updateClockPreference()
    }

    override fun onDestroy() {
        clockPreference.sharedPreferences?.unregisterOnSharedPreferenceChangeListener(sharedPreferenceChangeListener)
        super.onDestroy()
    }

    private val sharedPreferenceChangeListener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == "ra_hardcore_enabled" || key == clockPreference.key) {
            updateClockPreference()
        }
    }

    private fun updateFrameskipDrsExclusion() {
        val modePreference = findPreference<ListPreference>("frameskip_mode") ?: return
        val manualPreference = findPreference<ListPreference>("frameskip_manual_value") ?: return
        val drsActive = settingsRepository.isVulkanDrsActive()
        modePreference.isEnabled = !drsActive
        if (drsActive) {
            modePreference.summary = getString(R.string.frameskip_mode_disabled_by_drs)
            manualPreference.isVisible = false
        } else {
            modePreference.setSummary(R.string.frameskip_mode_summary)
            manualPreference.isVisible = modePreference.value == "manual"
        }
    }

    private fun enableSettingsMirror(mirrorPreference: SwitchPreference) {
        val mirrorDirectory = settingsBackupManager.getActiveMirrorDirectory()
        if (mirrorDirectory == null || !settingsBackupManager.hasMirrorAt(mirrorDirectory)) {
            setMirrorEnabled(mirrorPreference)
            settingsBackupManager.requestMirrorWrite()
            return
        }

        AlertDialog.Builder(requireContext())
            .setTitle(R.string.settings_mirror_detected_title)
            .setMessage(R.string.settings_mirror_detected_message)
            .setPositiveButton(R.string.settings_mirror_restore) { _, _ ->
                setMirrorEnabled(mirrorPreference)
                settingsBackupManager.restoreMirrorFrom(mirrorDirectory)
                settingsBackupManager.requestMirrorWrite()
            }
            .setNegativeButton(R.string.settings_mirror_ignore) { _, _ ->
                setMirrorEnabled(mirrorPreference)
                settingsBackupManager.overwriteMirrorAt(mirrorDirectory)
                settingsBackupManager.requestMirrorWrite()
            }
            .show()
    }

    private fun setMirrorEnabled(mirrorPreference: SwitchPreference) {
        updatingMirrorPreference = true
        mirrorPreference.isChecked = true
        updatingMirrorPreference = false
    }

    private fun updateClockPreference() {
        val preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        val hardcoreEnabled = preferences.getBoolean("ra_hardcore_enabled", false)
        val offset = preferences.getInt(clockPreference.key, 0).coerceIn(-1440, 1440)
        clockPreference.isEnabled = !hardcoreEnabled
        clockPreference.summary = when {
            hardcoreEnabled -> getString(R.string.emulated_clock_hardcore)
            offset == 0 -> getString(R.string.emulated_clock_device)
            else -> getString(
                R.string.emulated_clock_offset,
                String.format(Locale.getDefault(), "%s%02d:%02d", if (offset > 0) "+" else "−", abs(offset) / 60, abs(offset) % 60),
            )
        }
    }

    private fun showClockPicker() {
        val preferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
        val clock = Calendar.getInstance().apply {
            add(Calendar.MINUTE, preferences.getInt(clockPreference.key, 0).coerceIn(-1440, 1440))
        }
        val dialog = TimePickerDialog(
            requireContext(),
            { _, hour, minute ->
                val now = Calendar.getInstance()
                val offset = hour * 60 + minute - now.get(Calendar.HOUR_OF_DAY) * 60 - now.get(Calendar.MINUTE)
                preferences.edit().putInt(clockPreference.key, offset).apply()
            },
            clock.get(Calendar.HOUR_OF_DAY), clock.get(Calendar.MINUTE), DateFormat.is24HourFormat(requireContext()),
        )
        dialog.setTitle(R.string.emulated_clock)
        dialog.setButton(TimePickerDialog.BUTTON_NEUTRAL, getString(R.string.emulated_clock_reset)) { _, _ ->
            preferences.edit().putInt(clockPreference.key, 0).apply()
        }
        dialog.show()
    }
}
