// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.utils

import androidx.annotation.Keep

/**
 * Google Drive sync of profiles and saves (frontend_common/cloud_sync). The calls that reach
 * Google or wait for a sync block: run them off the main thread. [CloudSyncManager] is the app's
 * user of these.
 */
object NativeCloudSync {
    @Keep
    class SyncResult(
        val pushed: Long,
        val pulled: Long,
        val skipped: Long,
        val cancelled: Boolean,
        // What went wrong, for the user. Empty when everything synced.
        val errors: Array<String>
    )

    @Keep
    class SignInResult(val status: Int, val error: String) {
        companion object {
            // In CloudSync::SignInStatus order.
            const val SIGNED_IN = 0
            const val DENIED = 1
            const val EXPIRED = 2
            const val CANCELLED = 3
            const val FAILED = 4
        }
    }

    /**
     * Called on the syncing thread. [onConflict] asks the user, who answers through
     * [answerConflict]; [onConflictClosed] follows once the sync no longer waits for that answer.
     * Times are milliseconds since the epoch, 0 when unknown. A profile conflict has an empty
     * titleName; a device save's has an empty nickname.
     */
    @Keep
    interface ConflictListener {
        fun onConflict(
            id: Long,
            titleName: String,
            nickname: String,
            isProfile: Boolean,
            localTime: Long,
            remoteTime: Long
        )

        fun onConflictClosed(id: Long)
    }

    // The choices for answerConflict.
    const val KEEP_LOCAL = 0
    const val KEEP_REMOTE = 1
    const val SKIP = 2

    /** Signed in when last seen. Doesn't wait. */
    external fun isSignedIn(): Boolean

    /** A sync runs and may change the profiles: leave them alone meanwhile. */
    external fun isSyncRunning(): Boolean

    /**
     * Signs in with Google's device flow: hands the code and its URL to showCode (on the calling
     * thread), then waits until the user answers, the code expires or [cancelSignIn].
     */
    external fun signIn(showCode: (userCode: String, verificationUrl: String) -> Unit): SignInResult

    external fun cancelSignIn()

    /** Blocks on Google. False when the sign-in file could not be deleted (still signed out). */
    external fun signOut(): Boolean

    /** Imports a client_secret.json's contents. False when it has no installed-app client. */
    external fun importClientSecret(json: String): Boolean

    /** The OAuth client id sign-in uses: the imported one, else the build's; null when neither. */
    external fun clientId(): String?

    /** True when an imported client_secret.json is in effect instead of the build's client. */
    external fun isClientImported(): Boolean

    /** The profiles on Drive as uuid, nickname pairs one after the other; null when Drive fails. */
    external fun listRemoteProfiles(): Array<String>?

    /** Makes the local profile sync as the remote one, or as itself when remoteUuid is null. */
    external fun linkProfile(localUuid: String, remoteUuid: String?): Boolean

    /** The remote profile the local one syncs as, if it is linked. */
    external fun linkedProfile(localUuid: String): String?

    /**
     * Before a game boots, sync on or off: stops the syncs touching it and waits for them. Returns
     * the guest's token for [pullTitle], [cancelPull] and [endGuest]; 0 when that game already
     * runs. The native emulation run ends the guest once it loaded, and asks listener about the
     * conflicts of the syncs at a program change.
     */
    external fun beginGuest(programId: String, title: String, listener: ConflictListener): Long

    /** Brings in the guest's saves that changed on Drive. [cancelPull] stops it. */
    external fun pullTitle(token: Long, listener: ConflictListener): SyncResult

    external fun cancelPull(token: Long)

    /** The guest won't boot: ends [beginGuest]. Does nothing once it loaded. */
    external fun endGuest(token: Long)

    /** After the game exits: uploads its saves that changed here. */
    external fun pushTitle(programId: String, title: String, listener: ConflictListener): SyncResult

    /** Both directions for the games' saves, without the profiles. */
    external fun syncSaves(
        programIds: Array<String>,
        titles: Array<String>,
        listener: ConflictListener
    ): SyncResult

    /** "Sync now": the profiles (unless a game runs), then the games' saves. */
    external fun syncAll(
        programIds: Array<String>,
        titles: Array<String>,
        listener: ConflictListener
    ): SyncResult

    /** Answers [ConflictListener.onConflict] with [KEEP_LOCAL], [KEEP_REMOTE] or [SKIP]. */
    external fun answerConflict(id: Long, choice: Int)

    /** Called on the emulation thread when a guest that loaded stopped. */
    @Keep
    @JvmStatic
    fun onGuestStopped(programId: String, title: String) {
        CloudSyncManager.onGuestStopped(programId, title)
    }
}
