// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.fragments

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.net.toUri
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.transition.MaterialSharedAxis
import io.github.zortos293.lichii.NativeLibrary
import io.github.zortos293.lichii.R
import io.github.zortos293.lichii.adapters.HomeSettingAdapter
import io.github.zortos293.lichii.databinding.FragmentCloudSyncBinding
import io.github.zortos293.lichii.features.fetcher.SpacingItemDecoration
import io.github.zortos293.lichii.features.settings.model.BooleanSetting
import io.github.zortos293.lichii.model.HomeSetting
import io.github.zortos293.lichii.utils.CloudSyncManager
import io.github.zortos293.lichii.utils.NativeCloudSync
import io.github.zortos293.lichii.utils.NativeConfig
import io.github.zortos293.lichii.utils.ViewUtils.updateMargins
import io.github.zortos293.lichii.utils.collect
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class CloudSyncFragment : Fragment() {
    private var _binding: FragmentCloudSyncBinding? = null
    private val binding get() = _binding!!

    private val enabledStatus = MutableStateFlow("")

    // The sign-in state, or the code to enter while signing in.
    private val accountStatus = MutableStateFlow("")

    // Which OAuth client sign-in uses.
    private val clientStatus = MutableStateFlow("")
    private var signInJob: Job? = null
    private var codeDialog: AlertDialog? = null

    // Why the last sign-in failed, shown until the next sign-in or sign-out.
    private var signInError: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        exitTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentCloudSyncBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.toolbarCloudSync.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }

        binding.cloudSyncOptionsList.apply {
            layoutManager =
                GridLayoutManager(requireContext(), resources.getInteger(R.integer.grid_columns))
            addItemDecoration(
                SpacingItemDecoration(resources.getDimensionPixelSize(R.dimen.spacing_small))
            )
        }

        CloudSyncManager.busy.collect(viewLifecycleOwner) { refreshOptions() }

        setInsets()
    }

    override fun onResume() {
        super.onResume()
        refreshOptions()
    }

    private fun refreshOptions() {
        _binding ?: return
        binding.cloudSyncOptionsList.adapter = HomeSettingAdapter(
            requireActivity() as AppCompatActivity,
            viewLifecycleOwner,
            buildOptions()
        )
    }

    private fun buildOptions(): List<HomeSetting> {
        // Doesn't wait for a token refresh in flight.
        val signedIn = NativeCloudSync.isSignedIn()
        val signingIn = signInJob != null
        val enabled = BooleanSetting.CLOUD_SYNC_ENABLED.getBoolean()
        val syncing = CloudSyncManager.isSyncing()
        enabledStatus.value =
            getString(if (enabled) R.string.cloud_sync_on else R.string.cloud_sync_off)
        if (!signingIn) {
            accountStatus.value = signInError ?: getString(
                if (signedIn) R.string.cloud_sync_signed_in else R.string.cloud_sync_signed_out
            )
        }
        val clientId = NativeCloudSync.clientId()
        clientStatus.value = when {
            clientId == null -> getString(R.string.cloud_sync_client_none)
            NativeCloudSync.isClientImported() ->
                getString(R.string.cloud_sync_client_imported_status, clientId)

            else -> getString(R.string.cloud_sync_client_built_in, clientId)
        }
        val unavailableTitle = when {
            !signedIn -> R.string.cloud_sync_not_signed_in
            syncing -> R.string.cloud_sync_busy
            else -> R.string.cloud_sync_disabled
        }
        val unavailableMessage = when {
            !signedIn -> R.string.cloud_sync_not_signed_in_description
            syncing -> R.string.cloud_sync_busy_description
            else -> R.string.cloud_sync_disabled_description
        }
        return listOf(
            HomeSetting(
                R.string.cloud_sync_enabled,
                R.string.cloud_sync_enabled_description,
                R.drawable.ic_refresh,
                { toggleEnabled() },
                details = enabledStatus
            ),
            HomeSetting(
                when {
                    signingIn -> R.string.cloud_sync_cancel_sign_in
                    signedIn -> R.string.cloud_sync_sign_out
                    else -> R.string.cloud_sync_sign_in
                },
                R.string.cloud_sync_account_description,
                R.drawable.ic_account_circle,
                {
                    when {
                        signingIn -> NativeCloudSync.cancelSignIn()
                        signedIn -> signOut()
                        else -> signIn()
                    }
                },
                details = accountStatus
            ),
            HomeSetting(
                R.string.cloud_sync_import_client,
                R.string.cloud_sync_import_client_description,
                R.drawable.ic_key,
                { clientSecretPicker.launch(arrayOf("application/json", "text/plain", "*/*")) },
                { signInJob == null },
                R.string.cloud_sync_signing_in,
                R.string.cloud_sync_signing_in_description,
                details = clientStatus
            ),
            HomeSetting(
                R.string.cloud_sync_link_profile,
                R.string.cloud_sync_link_profile_description,
                R.drawable.ic_two_users,
                { chooseLocalProfile() },
                // Linking waits for the sync state, which a running sync holds.
                { NativeCloudSync.isSignedIn() && !CloudSyncManager.isSyncing() },
                if (signedIn) R.string.cloud_sync_busy else R.string.cloud_sync_not_signed_in,
                if (signedIn) {
                    R.string.cloud_sync_busy_description
                } else {
                    R.string.cloud_sync_not_signed_in_description
                }
            ),
            HomeSetting(
                R.string.cloud_sync_sync_now,
                R.string.cloud_sync_sync_now_description,
                R.drawable.ic_refresh,
                { CloudSyncManager.syncAll() },
                {
                    NativeCloudSync.isSignedIn() &&
                        BooleanSetting.CLOUD_SYNC_ENABLED.getBoolean() &&
                        !CloudSyncManager.isSyncing()
                },
                unavailableTitle,
                unavailableMessage
            )
        )
    }

    private fun toggleEnabled() {
        BooleanSetting.CLOUD_SYNC_ENABLED.setBoolean(!BooleanSetting.CLOUD_SYNC_ENABLED.getBoolean())
        NativeConfig.saveGlobalConfig()
        refreshOptions()
    }

    private fun signIn() {
        signInError = null
        accountStatus.value = getString(R.string.cloud_sync_requesting_code)
        val scope = viewLifecycleOwner.lifecycleScope
        signInJob = scope.launch {
            val result = withContext(Dispatchers.IO) {
                NativeCloudSync.signIn { userCode, verificationUrl ->
                    scope.launch { showCode(userCode, verificationUrl) }
                }
            }
            signInJob = null
            codeDialog?.dismiss()
            codeDialog = null
            signInError = when (result.status) {
                NativeCloudSync.SignInResult.DENIED -> getString(R.string.cloud_sync_denied)
                NativeCloudSync.SignInResult.EXPIRED -> getString(R.string.cloud_sync_code_expired)
                NativeCloudSync.SignInResult.FAILED ->
                    getString(R.string.cloud_sync_sign_in_failed, result.error)

                else -> null
            }
            refreshOptions()
        }
        refreshOptions()
    }

    private fun showCode(userCode: String, verificationUrl: String) {
        val message = getString(R.string.cloud_sync_enter_code, verificationUrl, userCode)
        accountStatus.value = message
        codeDialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.cloud_sync_sign_in)
            .setMessage(message)
            .setPositiveButton(R.string.cloud_sync_open_browser) { _, _ ->
                copyToClipboard(userCode)
                try {
                    startActivity(Intent(Intent.ACTION_VIEW, verificationUrl.toUri()))
                } catch (_: ActivityNotFoundException) {
                    Toast.makeText(requireContext(), verificationUrl, Toast.LENGTH_LONG).show()
                }
            }
            .setNegativeButton(R.string.cloud_sync_cancel_sign_in) { _, _ ->
                NativeCloudSync.cancelSignIn()
            }
            .show()
    }

    private fun copyToClipboard(text: String) {
        val clipBoard =
            requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipBoard.setPrimaryClip(ClipData.newPlainText(getString(R.string.cloud_sync), text))

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(requireContext(), R.string.copied_to_clipboard, Toast.LENGTH_SHORT)
                .show()
        }
    }

    private fun signOut() {
        signInError = null
        viewLifecycleOwner.lifecycleScope.launch {
            val removed = withContext(Dispatchers.IO) { NativeCloudSync.signOut() }
            refreshOptions()
            if (!removed) {
                showMessage(getString(R.string.cloud_sync_sign_out_file_left))
            }
        }
    }

    private val clientSecretPicker =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri == null) {
                return@registerForActivityResult
            }
            val resolver = requireContext().contentResolver
            viewLifecycleOwner.lifecycleScope.launch {
                val imported = withContext(Dispatchers.IO) {
                    val json = try {
                        resolver.openInputStream(uri)?.use { it.bufferedReader().readText() }
                    } catch (e: Exception) {
                        null
                    }
                    json != null && NativeCloudSync.importClientSecret(json)
                }
                refreshOptions()
                showMessage(
                    getString(
                        if (imported) {
                            R.string.cloud_sync_client_imported
                        } else {
                            R.string.cloud_sync_client_import_failed
                        }
                    )
                )
            }
        }

    private fun chooseLocalProfile() {
        // The profiles a sync may be changing are read once it is done.
        if (CloudSyncManager.isSyncing()) {
            showMessage(getString(R.string.cloud_sync_busy_description))
            return
        }
        val uuids = (NativeLibrary.getAllUsers() ?: emptyArray()).filter { it.isNotEmpty() }
        val names = uuids.map { NativeLibrary.getUserUsername(it)?.ifEmpty { null } ?: it }
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.cloud_sync_link_local_title)
            .setItems(names.toTypedArray()) { _, which ->
                chooseRemoteProfile(uuids[which], names[which])
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun chooseRemoteProfile(localUuid: String, localName: String) {
        Toast.makeText(requireContext(), R.string.cloud_sync_loading_profiles, Toast.LENGTH_SHORT)
            .show()
        viewLifecycleOwner.lifecycleScope.launch {
            val (remote, linked) = withContext(Dispatchers.IO) {
                NativeCloudSync.listRemoteProfiles() to NativeCloudSync.linkedProfile(localUuid)
            }
            if (remote == null) {
                showMessage(getString(R.string.cloud_sync_drive_unreachable))
                return@launch
            }
            // Itself first, then the other Drive profiles: its own Drive profile is "itself".
            val remoteUuids = mutableListOf<String?>(null)
            val labels = mutableListOf(getString(R.string.cloud_sync_link_itself))
            for (i in 0 until remote.size / 2) {
                if (remote[i * 2].equals(localUuid, ignoreCase = true)) {
                    continue
                }
                remoteUuids.add(remote[i * 2])
                labels.add(
                    getString(
                        R.string.cloud_sync_remote_profile,
                        remote[i * 2 + 1],
                        remote[i * 2].take(8)
                    )
                )
            }
            var selected = remoteUuids.indexOf(linked).coerceAtLeast(0)
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(getString(R.string.cloud_sync_link_remote_title, localName))
                .setSingleChoiceItems(labels.toTypedArray(), selected) { _, which ->
                    selected = which
                }
                .setPositiveButton(R.string.cloud_sync_link) { _, _ ->
                    link(localUuid, remoteUuids[selected])
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun link(localUuid: String, remoteUuid: String?) {
        if (CloudSyncManager.isSyncing()) {
            showMessage(getString(R.string.cloud_sync_busy_description))
            return
        }
        viewLifecycleOwner.lifecycleScope.launch {
            val linked = withContext(Dispatchers.IO) {
                NativeCloudSync.linkProfile(localUuid, remoteUuid)
            }
            if (linked) {
                Toast.makeText(requireContext(), R.string.cloud_sync_linked, Toast.LENGTH_SHORT)
                    .show()
            } else {
                showMessage(getString(R.string.cloud_sync_link_failed))
            }
        }
    }

    private fun showMessage(message: String) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.cloud_sync)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        // A sign-in nobody can see any more.
        if (signInJob != null) {
            NativeCloudSync.cancelSignIn()
            signInJob = null
        }
        codeDialog?.dismiss()
        codeDialog = null
        _binding = null
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { _, windowInsets ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            binding.appbarCloudSync.updateMargins(
                left = barInsets.left + cutoutInsets.left,
                right = barInsets.right + cutoutInsets.right
            )

            binding.scrollViewCloudSync.updatePadding(bottom = barInsets.bottom)

            binding.cloudSyncOptionsList.updatePadding(
                left = barInsets.left + cutoutInsets.left,
                right = barInsets.right + cutoutInsets.right
            )

            windowInsets
        }
}
