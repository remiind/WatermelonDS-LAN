package me.magnum.melonds.ui.settings.fragments

import android.content.Intent
import android.os.Bundle
import androidx.preference.Preference
import androidx.preference.MultiSelectListPreference
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import me.magnum.melonds.domain.repositories.LayoutsRepository
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SeekBarPreference
import androidx.preference.SwitchPreference
import dagger.hilt.android.AndroidEntryPoint
import me.magnum.melonds.R
import me.magnum.melonds.common.vibration.TouchVibrator
import me.magnum.melonds.ui.inputsetup.InputSetupActivity
import me.magnum.melonds.ui.layouts.LayoutListActivity
import me.magnum.melonds.ui.settings.PreferenceFragmentTitleProvider
import me.magnum.melonds.ui.settings.SettingsActivity
import me.magnum.melonds.ui.settings.preferences.InGameLockedPreference
import me.magnum.melonds.ui.settings.preferences.SoftwareInputBehaviourPreference
import javax.inject.Inject

@AndroidEntryPoint
class InputPreferencesFragment : BasePreferenceFragment(), PreferenceFragmentTitleProvider {

    @Inject lateinit var vibrator: TouchVibrator
    @Inject lateinit var layoutsRepository: LayoutsRepository

    private lateinit var softInputBehaviourPreference: SoftwareInputBehaviourPreference

    override fun getTitle() = getString(R.string.input)

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.pref_input, rootKey)
        softInputBehaviourPreference = findPreference("soft_input_behaviour")!!
        val cycleLayoutsPreference = findPreference<MultiSelectListPreference>("input_cycle_layout_ids")!!
        cycleLayoutsPreference.isPersistent = false
        cycleLayoutsPreference.setOnPreferenceChangeListener { _, value ->
            @Suppress("UNCHECKED_CAST")
            val selectedIds = value as Set<String>
            cycleLayoutsPreference.sharedPreferences?.edit()
                ?.putStringSet(cycleLayoutsPreference.key, selectedIds)?.apply()
            true
        }
        val touchVibratePreference = findPreference<SwitchPreference>("input_touch_haptic_feedback_enabled")!!
        val vibrationStrengthPreference = findPreference<SeekBarPreference>("input_touch_haptic_feedback_strength")!!
        val keyMappingPreference = findPreference<InGameLockedPreference>("input_key_mapping")!!
        val layoutsPreference = findPreference<InGameLockedPreference>("input_layouts")!!
        keyMappingPreference.isInGameLocked = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_INPUT_MAPPING, false)
        keyMappingPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings
        layoutsPreference.isInGameLocked = requireActivity().intent.getBooleanExtra(SettingsActivity.KEY_LOCK_INPUT_LAYOUT, false)
        layoutsPreference.inGameLockedMessageRes = R.string.cannot_change_use_rom_settings

        if (!vibrator.supportsVibration()) {
            touchVibratePreference.isVisible = false
        }
        vibrationStrengthPreference.isVisible = false

        vibrationStrengthPreference.setOnPreferenceChangeListener { _, newValue ->
            val strength = newValue as Int
            vibrator.performTouchHapticFeedback(strength)
            true
        }
        keyMappingPreference.setOnPreferenceClickListener {
            val intent = InputSetupActivity.getGlobalIntent(requireContext())
            startActivity(intent)
            true
        }
        layoutsPreference.setOnPreferenceClickListener {
            val intent = Intent(requireContext(), LayoutListActivity::class.java)
            startActivity(intent)
            true
        }
    }

    override fun onResume() {
        super.onResume()
        lifecycleScope.launch {
            val layouts = layoutsRepository.getLayouts().first().filter { it.id != null }
            val preference = findPreference<MultiSelectListPreference>("input_cycle_layout_ids") ?: return@launch
            val ids = layouts.map { it.id.toString() }.toTypedArray()
            preference.entries = layouts.map { it.name ?: getString(R.string.custom_layout_default_name) }.toTypedArray()
            preference.entryValues = ids
            preference.values = preference.sharedPreferences
                ?.getStringSet(preference.key, null)?.intersect(ids.toSet()) ?: ids.toSet()
        }
        // Set proper value for soft input behaviour preference since the value is not updated when returning from the fragment
        softInputBehaviourPreference.value = softInputBehaviourPreference.sharedPreferences?.getString(softInputBehaviourPreference.key, "hide_system_buttons_when_controller_connected")
    }
}
