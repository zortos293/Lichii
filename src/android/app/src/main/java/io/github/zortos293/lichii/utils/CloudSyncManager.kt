// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.utils

import android.app.Activity
import android.app.Application
import android.content.Context
import android.content.DialogInterface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Toast
import androidx.annotation.Keep
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.FragmentActivity
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.github.zortos293.lichii.LichiiApplication
import io.github.zortos293.lichii.R
import io.github.zortos293.lichii.databinding.DialogProgressBarBinding
import io.github.zortos293.lichii.features.settings.model.BooleanSetting
import io.github.zortos293.lichii.model.Game
import java.lang.ref.WeakReference
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json

/**
 * Google Drive sync ([NativeCloudSync]) for the app: the pull before a game boots, the push after
 * it stops, "Sync save" and "Sync now". Syncs run on Dispatchers.IO; conflicts are asked in the
 * foreground activity without blocking it. Main thread only, except the listener callbacks.
 */
object CloudSyncManager : NativeCloudSync.ConflictListener {
    // How long the pull before boot may run before it is stopped and the game starts without it.
    private const val PULL_BUDGET_MS = 20_000L

    // The boot's dialog appears only when the boot waits longer than this.
    private const val PROGRESS_DELAY_MS = 300L

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val mainHandler = Handler(Looper.getMainLooper())

    // The syncs started here, the pull before boot included.
    private var running = 0
    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> get() = _busy

    // The save imports between beginSaveImport and endSaveImport, which boots and uploads wait for.
    private val saveImports = MutableStateFlow(0)

    private var foreground = WeakReference<Activity>(null)

    private class Prompt(
        val titleName: String,
        val nickname: String,
        val isProfile: Boolean,
        val localTime: Long,
        val remoteTime: Long
    ) {
        var dialog: AlertDialog? = null
        var shownIn: Activity? = null
    }

    // The open conflict questions by id, shown in the foreground activity.
    private val prompts = linkedMapOf<Long, Prompt>()

    /** The cloud sync part of a game's boot, for [abandonBoot]. */
    class Boot internal constructor() {
        internal var token = 0L
        internal var stopRequested = false
        internal var pullDone = false
        internal var abandoned = false
        internal var dialog: AlertDialog? = null
    }

    fun install(application: Application) {
        application.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityResumed(activity: Activity) {
                foreground = WeakReference(activity)
                // Moved to the activity in front.
                hidePrompts { it !== activity }
                showPrompts()
            }

            override fun onActivityDestroyed(activity: Activity) {
                hidePrompts { it === activity }
                if (foreground.get() === activity) {
                    foreground.clear()
                }
                showPrompts()
            }

            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
            override fun onActivityStarted(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) = Unit
            override fun onActivityStopped(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
        })
    }

    /** Sync is on in the settings and an account was signed in when last seen. Doesn't wait. */
    fun isEnabled(): Boolean =
        BooleanSetting.CLOUD_SYNC_ENABLED.getBoolean() && NativeCloudSync.isSignedIn()

    /** A sync runs and may change the profiles: profile editing waits for it. */
    fun isSyncing(): Boolean = _busy.value || NativeCloudSync.isSyncRunning()

    /**
     * Counts a save import as a sync until [endSaveImport] (callable on any thread), so that
     * syncs started meanwhile are refused by their [isSyncing] check, and boots and uploads wait
     * for it. False when a sync runs.
     */
    fun beginSaveImport(): Boolean {
        if (isSyncing()) {
            return false
        }
        setRunning(1)
        saveImports.value++
        return true
    }

    fun endSaveImport() {
        mainHandler.post {
            saveImports.value--
            setRunning(-1)
        }
    }

    /**
     * Before [game] boots, sync on or off: waits for the save imports, stops the syncs touching it
     * and waits for them, then pulls its saves if sync is on. A dialog appears when this takes a
     * while; its Cancel and PULL_BUDGET_MS stop only the pull. Calls onReady once nothing touches
     * the game (after a notice when the pull failed or took too long), unless [abandonBoot] came
     * first. The native emulation run ends the guest once the game loaded; [abandonBoot] must end
     * it otherwise.
     */
    fun prepareBoot(activity: FragmentActivity, game: Game, onReady: () -> Unit): Boot {
        val boot = Boot()
        val pull = isEnabled() && (game.programId.toLongOrNull() ?: 0L) != 0L
        val binding = DialogProgressBarBinding.inflate(activity.layoutInflater)
        binding.progressBar.isIndeterminate = true
        binding.message.visibility = View.VISIBLE
        binding.message.isSelected = true
        binding.message.setText(R.string.cloud_sync_waiting)
        val progress = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.cloud_sync)
            .setView(binding.root)
            .setCancelable(false)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        val stopPull: () -> Unit = {
            boot.stopRequested = true
            binding.message.setText(R.string.cloud_sync_stopping)
            if (boot.token != 0L) {
                NativeCloudSync.cancelPull(boot.token)
            }
        }
        // Stays open until the pull has stopped.
        progress.setOnShowListener {
            progress.getButton(DialogInterface.BUTTON_NEGATIVE).setOnClickListener { stopPull() }
        }
        boot.dialog = progress

        setRunning(1)
        scope.launch {
            var timedOut = false
            val showLater = launch {
                delay(PROGRESS_DELAY_MS)
                if (!boot.abandoned && !activity.isFinishing && !activity.isDestroyed) {
                    progress.show()
                }
            }
            var budget: Job? = null
            // Not cut short by Cancel: nothing may touch the game once it boots.
            saveImports.first { it == 0 }
            val token = withContext(Dispatchers.IO) {
                NativeCloudSync.beginGuest(game.programId, game.title, CloudSyncManager)
            }
            boot.token = token
            var result: NativeCloudSync.SyncResult? = null
            if (token != 0L && pull && !boot.stopRequested && !boot.abandoned) {
                binding.message.setText(R.string.cloud_sync_pulling)
                budget = launch {
                    delay(PULL_BUDGET_MS)
                    timedOut = true
                    stopPull()
                }
                result = withContext(Dispatchers.IO) {
                    NativeCloudSync.pullTitle(token, CloudSyncManager)
                }
            }
            showLater.cancel()
            budget?.cancel()
            progress.dismiss()
            boot.dialog = null
            boot.pullDone = true
            setRunning(-1)
            if (boot.abandoned) {
                NativeCloudSync.endGuest(token)
                return@launch
            }

            val notice = when {
                timedOut -> activity.getString(R.string.cloud_sync_pull_timed_out)
                result != null && !result.cancelled && result.errors.isNotEmpty() ->
                    activity.getString(
                        R.string.cloud_sync_pull_failed,
                        result.errors.joinToString("\n")
                    )

                else -> null
            }
            if (notice == null || activity.isFinishing || activity.isDestroyed) {
                onReady()
                return@launch
            }
            boot.dialog = MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.cloud_sync)
                .setMessage(notice)
                .setPositiveButton(android.R.string.ok, null)
                .setOnDismissListener {
                    boot.dialog = null
                    if (!boot.abandoned) {
                        onReady()
                    }
                }
                .show()
        }
        return boot
    }

    /** The game of [boot] won't start: ends what [prepareBoot] began. */
    fun abandonBoot(boot: Boot) {
        if (boot.abandoned) {
            return
        }
        boot.abandoned = true
        boot.dialog?.dismiss()
        boot.dialog = null
        if (boot.token != 0L) {
            NativeCloudSync.cancelPull(boot.token)
        }
        // Otherwise prepareBoot ends it once its pull returns.
        if (boot.pullDone) {
            NativeCloudSync.endGuest(boot.token)
        }
    }

    fun onGuestStopped(programId: String, title: String) {
        mainHandler.post {
            if (!isEnabled() || (programId.toLongOrNull() ?: 0L) == 0L) {
                return@post
            }
            runInBackground(
                LichiiApplication.appContext.getString(R.string.cloud_sync_uploading, title),
                false
            ) { NativeCloudSync.pushTitle(programId, title, CloudSyncManager) }
        }
    }

    /** Syncs [game]'s saves both ways, in the background, without the profiles. */
    fun syncTitle(game: Game) {
        runInBackground(
            LichiiApplication.appContext.getString(R.string.cloud_sync_syncing_title, game.title),
            true
        ) {
            NativeCloudSync.syncSaves(arrayOf(game.programId), arrayOf(game.title), CloudSyncManager)
        }
    }

    /** Syncs the profiles (unless a game runs) and every game's saves, in the background. */
    fun syncAll() {
        val games = libraryGames()
        runInBackground(LichiiApplication.appContext.getString(R.string.cloud_sync_syncing_all), true) {
            NativeCloudSync.syncAll(
                games.map { it.programId }.toTypedArray(),
                games.map { it.title }.toTypedArray(),
                CloudSyncManager
            )
        }
    }

    // The games of the last library scan, one per program id.
    private fun libraryGames(): List<Game> {
        val stored = PreferenceManager.getDefaultSharedPreferences(LichiiApplication.appContext)
            .getStringSet(GameHelper.KEY_GAMES, emptySet()) ?: emptySet()
        return stored.mapNotNull {
            try {
                val game: Game = Json.decodeFromString(it)
                game
            } catch (e: Exception) {
                null
            }
        }.filter { !it.isHomebrew && (it.programId.toLongOrNull() ?: 0L) != 0L }
            .distinctBy { it.programId }
    }

    private fun setRunning(change: Int) {
        running += change
        _busy.value = running > 0
    }

    private fun runInBackground(
        what: String,
        reportInDialog: Boolean,
        work: () -> NativeCloudSync.SyncResult
    ) {
        setRunning(1)
        if (!reportInDialog) {
            toast(LichiiApplication.appContext.getString(R.string.cloud_sync_status_running, what))
        }
        scope.launch {
            saveImports.first { it == 0 }
            val result = withContext(Dispatchers.IO) { work() }
            setRunning(-1)
            report(what, result, reportInDialog)
        }
    }

    private fun report(what: String, result: NativeCloudSync.SyncResult, reportInDialog: Boolean) {
        if (result.cancelled) {
            return
        }
        val context = LichiiApplication.appContext
        if (result.errors.isEmpty()) {
            if (reportInDialog) {
                showMessage(
                    context.getString(
                        R.string.cloud_sync_synced,
                        result.pushed,
                        result.pulled,
                        result.skipped
                    )
                )
            } else {
                toast(
                    context.getString(
                        R.string.cloud_sync_status_done,
                        what,
                        result.pushed,
                        result.pulled,
                        result.skipped
                    )
                )
            }
            return
        }
        if (reportInDialog) {
            showMessage(
                context.getString(R.string.cloud_sync_failed, what, result.errors.joinToString("\n"))
            )
        } else {
            toast(context.getString(R.string.cloud_sync_status_failed, what, result.errors.first()))
        }
    }

    private fun toast(message: String) {
        Toast.makeText(LichiiApplication.appContext, message, Toast.LENGTH_LONG).show()
    }

    private fun showMessage(message: String) {
        val activity = foreground.get()
        if (activity == null || activity.isFinishing || activity.isDestroyed) {
            toast(message)
            return
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.cloud_sync)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    @Keep
    override fun onConflict(
        id: Long,
        titleName: String,
        nickname: String,
        isProfile: Boolean,
        localTime: Long,
        remoteTime: Long
    ) {
        mainHandler.post {
            prompts[id] = Prompt(titleName, nickname, isProfile, localTime, remoteTime)
            showPrompts()
        }
    }

    @Keep
    override fun onConflictClosed(id: Long) {
        mainHandler.post {
            prompts.remove(id)?.dialog?.dismiss()
        }
    }

    // Closes the questions shown in the activities that match, to be asked again by showPrompts.
    private fun hidePrompts(matches: (Activity) -> Boolean) {
        prompts.values.forEach { prompt ->
            val shownIn = prompt.shownIn ?: return@forEach
            if (matches(shownIn)) {
                prompt.dialog?.dismiss()
                prompt.dialog = null
                prompt.shownIn = null
            }
        }
    }

    private fun showPrompts() {
        val activity = foreground.get() ?: return
        if (activity.isFinishing || activity.isDestroyed) {
            return
        }
        prompts.forEach { (id, prompt) ->
            if (prompt.dialog == null) {
                showPrompt(id, prompt, activity)
            }
        }
    }

    private fun showPrompt(id: Long, prompt: Prompt, activity: Activity) {
        val what = when {
            prompt.isProfile ->
                activity.getString(R.string.cloud_sync_conflict_profile, prompt.nickname)

            prompt.nickname.isEmpty() ->
                activity.getString(R.string.cloud_sync_conflict_shared_save, prompt.titleName)

            else -> activity.getString(
                R.string.cloud_sync_conflict_user_save,
                prompt.nickname,
                prompt.titleName
            )
        }
        prompt.shownIn = activity
        prompt.dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.cloud_sync_conflict_title)
            .setMessage(
                activity.getString(
                    R.string.cloud_sync_conflict_message,
                    what,
                    formatTime(activity, prompt.localTime),
                    formatTime(activity, prompt.remoteTime)
                )
            )
            .setCancelable(false)
            .setPositiveButton(R.string.cloud_sync_keep_local) { _, _ ->
                answer(id, NativeCloudSync.KEEP_LOCAL)
            }
            .setNegativeButton(R.string.cloud_sync_keep_remote) { _, _ ->
                answer(id, NativeCloudSync.KEEP_REMOTE)
            }
            .setNeutralButton(R.string.cloud_sync_skip) { _, _ ->
                answer(id, NativeCloudSync.SKIP)
            }
            .show()
    }

    private fun answer(id: Long, choice: Int) {
        prompts.remove(id)
        NativeCloudSync.answerConflict(id, choice)
    }

    private fun formatTime(context: Context, time: Long): String =
        if (time == 0L) {
            context.getString(R.string.cloud_sync_time_unknown)
        } else {
            DateFormat.getDateTimeInstance(DateFormat.LONG, DateFormat.SHORT).format(Date(time))
        }
}
