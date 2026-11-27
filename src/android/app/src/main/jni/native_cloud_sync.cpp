// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "frontend_common/cloud_sync/google_auth.h"
#include "frontend_common/cloud_sync/profile_sync.h"
#include "frontend_common/cloud_sync/sync_engine.h"
#include "native.h"
#include "native_cloud_sync.h"

namespace CloudSync = FrontendCommon::CloudSync;

namespace {

constexpr const char* SyncResultClass = "io/github/zortos293/lichii/utils/NativeCloudSync$SyncResult";
constexpr const char* SignInResultClass =
    "io/github/zortos293/lichii/utils/NativeCloudSync$SignInResult";

// The guest beginGuest began, until it stops or the app gives it up.
struct Guest {
    jlong token{};
    // The game's program id beginGuest got; title follows the program that runs.
    u64 game_id{};
    CloudSync::Title title;
    bool loaded{};
    // Stops the pull before the guest boots, also when cancelPull comes before pullTitle.
    std::stop_source pull_stop;
    // Its BeginGuest's, for EndGuest.
    u64 registration{};
};

std::mutex guest_mutex;
std::optional<Guest> guest;
jlong next_guest_token = 1;
// Held by beginGuest from its first look at the guest to the install, so that a begin can't
// replace the guest of one that began after it.
std::mutex begin_mutex;
// NativeCloudSync, for GuestStopped to call on the emulation thread.
jclass native_cloud_sync_class = nullptr;
// CloudSyncManager, for the syncs of a program change on the emulation thread.
jobject conflict_listener = nullptr;

std::mutex sign_in_mutex;
std::stop_source sign_in_stop;

// The conflict questions the app shows, by id, for answerConflict.
std::mutex prompts_mutex;
std::map<jlong, std::function<void(CloudSync::ConflictChoice)>> prompts;
jlong next_prompt_id = 1;

Service::Account::ProfileManager& Profiles() {
    return EmulationSession::GetInstance().System().GetProfileManager();
}

jlong Milliseconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

// Asks listener (NativeCloudSync.ConflictListener) on the syncing thread, which is the thread of
// the JNI call that runs the sync: env and listener are valid for it.
CloudSync::SyncCallbacks Callbacks(JNIEnv* env, jobject listener) {
    jclass listener_class = env->GetObjectClass(listener);
    const jmethodID on_conflict = env->GetMethodID(listener_class, "onConflict",
                                                   "(JLjava/lang/String;Ljava/lang/String;ZJJ)V");
    const jmethodID on_conflict_closed =
        env->GetMethodID(listener_class, "onConflictClosed", "(J)V");
    env->DeleteLocalRef(listener_class);
    return {
        .resolve_conflict =
            [env, listener, on_conflict, on_conflict_closed](const CloudSync::Conflict& conflict,
                                                             std::stop_token stop) {
                jlong id = 0;
                {
                    std::scoped_lock lock{prompts_mutex};
                    id = next_prompt_id++;
                }
                const auto choice = CloudSync::WaitForAnswer(
                    stop, [&](std::function<void(CloudSync::ConflictChoice)> answer) {
                        {
                            std::scoped_lock lock{prompts_mutex};
                            prompts.emplace(id, std::move(answer));
                        }
                        jstring title = Common::Android::ToJString(env, conflict.title_name);
                        jstring nickname = Common::Android::ToJString(env, conflict.nickname);
                        env->CallVoidMethod(listener, on_conflict, id, title, nickname,
                                            static_cast<jboolean>(conflict.title_id == 0),
                                            Milliseconds(conflict.local_time),
                                            Milliseconds(conflict.remote_time));
                        env->DeleteLocalRef(title);
                        env->DeleteLocalRef(nickname);
                    });
                {
                    std::scoped_lock lock{prompts_mutex};
                    prompts.erase(id);
                }
                // Closes the question when the wait ended without an answer.
                env->CallVoidMethod(listener, on_conflict_closed, id);
                return choice;
            },
        .progress = {},
    };
}

jobject ToJSyncResult(JNIEnv* env, const CloudSync::SyncResult& result) {
    jclass result_class = env->FindClass(SyncResultClass);
    const jmethodID constructor =
        env->GetMethodID(result_class, "<init>", "(JJJZ[Ljava/lang/String;)V");
    jobjectArray errors = Common::Android::ToJStringArray(env, result.errors);
    jobject object = env->NewObject(
        result_class, constructor, static_cast<jlong>(result.pushed),
        static_cast<jlong>(result.pulled), static_cast<jlong>(result.skipped),
        static_cast<jboolean>(result.cancelled), errors);
    env->DeleteLocalRef(errors);
    env->DeleteLocalRef(result_class);
    return object;
}

std::vector<CloudSync::Title> ToTitles(JNIEnv* env, jobjectArray jprogram_ids,
                                       jobjectArray jtitles) {
    const jsize count = env->GetArrayLength(jprogram_ids);
    std::vector<CloudSync::Title> titles;
    titles.reserve(static_cast<std::size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto* const jprogram_id = static_cast<jstring>(env->GetObjectArrayElement(jprogram_ids, i));
        auto* const jtitle = static_cast<jstring>(env->GetObjectArrayElement(jtitles, i));
        titles.push_back({EmulationSession::GetProgramId(env, jprogram_id),
                          Common::Android::GetJString(env, jtitle)});
        env->DeleteLocalRef(jprogram_id);
        env->DeleteLocalRef(jtitle);
    }
    return titles;
}

} // namespace

namespace AndroidCloudSync {

void GuestLoaded(u64 program_id, std::stop_token stop) {
    CloudSync::Title previous;
    CloudSync::Title next;
    jobject listener = nullptr;
    jlong token = 0;
    {
        // As in beginGuest, so that no beginGuest registers between this registration and the
        // look at the guest it is for.
        std::scoped_lock begin_lock{begin_mutex};
        {
            std::scoped_lock lock{guest_mutex};
            if (!guest) {
                return;
            }
            if (!guest->loaded || guest->title.id == program_id) {
                guest->loaded = true;
                return;
            }
            // A program change to a program that saves under its own id.
            previous = guest->title;
            guest->title.id = program_id;
            next = guest->title;
            listener = conflict_listener;
            token = guest->token;
        }
        // Before it runs, one after the other in case both programs use the same save: the
        // previous program's saves go up, and the next one's come down.
        const u64 registration = CloudSync::BeginGuest(next.id);
        std::scoped_lock lock{guest_mutex};
        // Unless the guest stopped meanwhile.
        if (guest && guest->token == token) {
            guest->registration = registration;
        }
    }
    if (!Settings::values.cloud_sync_enabled.GetValue() || !CloudSync::SignedInSnapshot() ||
        listener == nullptr) {
        return;
    }
    JNIEnv* env = Common::Android::GetEnvForThread();
    const auto callbacks = Callbacks(env, listener);
    const auto log_errors = [](const CloudSync::SyncResult& result) {
        for (const auto& error : result.errors) {
            LOG_WARNING(Frontend, "Cloud sync at the program change: {}", error);
        }
    };
    if (previous.id != 0) {
        log_errors(CloudSync::PushTitle(Profiles(), previous, callbacks, stop));
    }
    if (next.id != 0) {
        log_errors(CloudSync::PullTitle(Profiles(), next, callbacks, stop));
    }
}

void GuestStopped() {
    std::optional<Guest> stopped;
    jclass callback_class = nullptr;
    {
        std::scoped_lock lock{guest_mutex};
        stopped = std::exchange(guest, std::nullopt);
        if (!stopped) {
            return;
        }
        CloudSync::EndGuest(stopped->registration);
        callback_class = native_cloud_sync_class;
    }
    if (!stopped->loaded || callback_class == nullptr) {
        return;
    }
    JNIEnv* env = Common::Android::GetEnvForThread();
    const jmethodID on_guest_stopped = env->GetStaticMethodID(
        callback_class, "onGuestStopped", "(Ljava/lang/String;Ljava/lang/String;)V");
    jstring program_id = Common::Android::ToJString(env, std::to_string(stopped->title.id));
    jstring title = Common::Android::ToJString(env, stopped->title.name);
    env->CallStaticVoidMethod(callback_class, on_guest_stopped, program_id, title);
    env->DeleteLocalRef(program_id);
    env->DeleteLocalRef(title);
}

} // namespace AndroidCloudSync

extern "C" {

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_isSignedIn(JNIEnv* env,
                                                                          jobject obj) {
    return static_cast<jboolean>(CloudSync::SignedInSnapshot());
}

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_isSyncRunning(JNIEnv* env,
                                                                             jobject obj) {
    return static_cast<jboolean>(CloudSync::SyncRunning());
}

jobject Java_io_github_zortos293_lichii_utils_NativeCloudSync_signIn(JNIEnv* env, jobject obj,
                                                                     jobject jshow_code) {
    std::stop_token stop;
    {
        std::scoped_lock lock{sign_in_mutex};
        sign_in_stop = std::stop_source{};
        stop = sign_in_stop.get_token();
    }
    jclass show_code_class = env->GetObjectClass(jshow_code);
    const jmethodID invoke = env->GetMethodID(
        show_code_class, "invoke", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(show_code_class);
    const auto result = CloudSync::SignIn(
        [&](const CloudSync::DeviceCode& code) {
            jstring user_code = Common::Android::ToJString(env, code.user_code);
            jstring url = Common::Android::ToJString(env, code.verification_url);
            jobject unit = env->CallObjectMethod(jshow_code, invoke, user_code, url);
            env->DeleteLocalRef(unit);
            env->DeleteLocalRef(user_code);
            env->DeleteLocalRef(url);
        },
        stop);

    jclass result_class = env->FindClass(SignInResultClass);
    const jmethodID constructor =
        env->GetMethodID(result_class, "<init>", "(ILjava/lang/String;)V");
    jstring error = Common::Android::ToJString(env, result.error);
    jobject object =
        env->NewObject(result_class, constructor, static_cast<jint>(result.status), error);
    env->DeleteLocalRef(error);
    env->DeleteLocalRef(result_class);
    return object;
}

void Java_io_github_zortos293_lichii_utils_NativeCloudSync_cancelSignIn(JNIEnv* env, jobject obj) {
    std::scoped_lock lock{sign_in_mutex};
    sign_in_stop.request_stop();
}

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_signOut(JNIEnv* env, jobject obj) {
    return static_cast<jboolean>(CloudSync::SignOut());
}

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_importClientSecret(JNIEnv* env,
                                                                                  jobject obj,
                                                                                  jstring jjson) {
    return static_cast<jboolean>(
        CloudSync::ImportClientSecret(Common::Android::GetJString(env, jjson)));
}

jstring Java_io_github_zortos293_lichii_utils_NativeCloudSync_clientId(JNIEnv* env, jobject obj) {
    const auto client = CloudSync::LoadClientCredentials();
    return client ? Common::Android::ToJString(env, client->client_id) : nullptr;
}

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_isClientImported(JNIEnv* env,
                                                                                jobject obj) {
    return static_cast<jboolean>(CloudSync::ClientImported());
}

jobjectArray Java_io_github_zortos293_lichii_utils_NativeCloudSync_listRemoteProfiles(JNIEnv* env,
                                                                                      jobject obj) {
    const auto profiles = CloudSync::ListRemoteProfiles();
    if (!profiles) {
        return nullptr;
    }
    std::vector<std::string> flattened;
    flattened.reserve(profiles->size() * 2);
    for (const auto& profile : *profiles) {
        flattened.push_back(profile.uuid.FormattedString());
        flattened.push_back(profile.nickname);
    }
    return Common::Android::ToJStringArray(env, flattened);
}

jboolean Java_io_github_zortos293_lichii_utils_NativeCloudSync_linkProfile(JNIEnv* env,
                                                                           jobject obj,
                                                                           jstring jlocal_uuid,
                                                                           jstring jremote_uuid) {
    const Common::UUID local{Common::Android::GetJString(env, jlocal_uuid)};
    // Deleted in the profile manager since the app listed it.
    if (!Profiles().UserExists(local)) {
        return false;
    }
    std::optional<Common::UUID> remote;
    if (jremote_uuid != nullptr) {
        remote = Common::UUID{Common::Android::GetJString(env, jremote_uuid)};
    }
    return static_cast<jboolean>(CloudSync::LinkProfile(Profiles(), local, remote));
}

jstring Java_io_github_zortos293_lichii_utils_NativeCloudSync_linkedProfile(JNIEnv* env,
                                                                            jobject obj,
                                                                            jstring jlocal_uuid) {
    const auto linked =
        CloudSync::LinkedProfile(Common::UUID{Common::Android::GetJString(env, jlocal_uuid)});
    if (!linked) {
        return nullptr;
    }
    return Common::Android::ToJString(env, linked->FormattedString());
}

jlong Java_io_github_zortos293_lichii_utils_NativeCloudSync_beginGuest(JNIEnv* env, jobject obj,
                                                                       jstring jprogram_id,
                                                                       jstring jtitle,
                                                                       jobject listener) {
    const u64 program_id = EmulationSession::GetProgramId(env, jprogram_id);
    std::scoped_lock begin_lock{begin_mutex};
    {
        std::scoped_lock lock{guest_mutex};
        if (native_cloud_sync_class == nullptr) {
            jclass object_class = env->GetObjectClass(obj);
            native_cloud_sync_class = static_cast<jclass>(env->NewGlobalRef(object_class));
            env->DeleteLocalRef(object_class);
        }
        if (conflict_listener == nullptr) {
            conflict_listener = env->NewGlobalRef(listener);
        }
        // Begun for an earlier view of this game, which still runs.
        if (guest && guest->loaded && guest->game_id == program_id) {
            return 0;
        }
    }
    const u64 registration = CloudSync::BeginGuest(program_id);
    std::scoped_lock lock{guest_mutex};
    // The replaced guest's pull can no longer be cancelled by its token.
    if (guest) {
        guest->pull_stop.request_stop();
    }
    guest = Guest{
        .token = next_guest_token++,
        .game_id = program_id,
        .title = {program_id, Common::Android::GetJString(env, jtitle)},
        .loaded = false,
        .pull_stop = {},
        .registration = registration,
    };
    return guest->token;
}

jobject Java_io_github_zortos293_lichii_utils_NativeCloudSync_pullTitle(JNIEnv* env, jobject obj,
                                                                        jlong token,
                                                                        jobject listener) {
    CloudSync::Title title;
    std::stop_token stop;
    {
        std::scoped_lock lock{guest_mutex};
        if (!guest || guest->token != token) {
            return ToJSyncResult(env, {.cancelled = true});
        }
        title = guest->title;
        stop = guest->pull_stop.get_token();
    }
    return ToJSyncResult(env,
                         CloudSync::PullTitle(Profiles(), title, Callbacks(env, listener), stop));
}

void Java_io_github_zortos293_lichii_utils_NativeCloudSync_cancelPull(JNIEnv* env, jobject obj,
                                                                      jlong token) {
    std::scoped_lock lock{guest_mutex};
    if (guest && guest->token == token) {
        guest->pull_stop.request_stop();
    }
}

void Java_io_github_zortos293_lichii_utils_NativeCloudSync_endGuest(JNIEnv* env, jobject obj,
                                                                    jlong token) {
    std::scoped_lock lock{guest_mutex};
    // A guest that loaded ends with its emulation run.
    if (!guest || guest->token != token || guest->loaded) {
        return;
    }
    guest->pull_stop.request_stop();
    CloudSync::EndGuest(guest->registration);
    guest.reset();
}

jobject Java_io_github_zortos293_lichii_utils_NativeCloudSync_pushTitle(JNIEnv* env, jobject obj,
                                                                        jstring jprogram_id,
                                                                        jstring jtitle,
                                                                        jobject listener) {
    const CloudSync::Title title{EmulationSession::GetProgramId(env, jprogram_id),
                                 Common::Android::GetJString(env, jtitle)};
    return ToJSyncResult(env, CloudSync::PushTitle(Profiles(), title, Callbacks(env, listener), {}));
}

jobject Java_io_github_zortos293_lichii_utils_NativeCloudSync_syncSaves(JNIEnv* env, jobject obj,
                                                                        jobjectArray jprogram_ids,
                                                                        jobjectArray jtitles,
                                                                        jobject listener) {
    const auto titles = ToTitles(env, jprogram_ids, jtitles);
    return ToJSyncResult(env,
                         CloudSync::SyncSaves(Profiles(), titles, Callbacks(env, listener), {}));
}

jobject Java_io_github_zortos293_lichii_utils_NativeCloudSync_syncAll(JNIEnv* env, jobject obj,
                                                                      jobjectArray jprogram_ids,
                                                                      jobjectArray jtitles,
                                                                      jobject listener) {
    const auto titles = ToTitles(env, jprogram_ids, jtitles);
    return ToJSyncResult(env, CloudSync::SyncAll(Profiles(), titles, Callbacks(env, listener), {}));
}

void Java_io_github_zortos293_lichii_utils_NativeCloudSync_answerConflict(JNIEnv* env, jobject obj,
                                                                          jlong id, jint choice) {
    std::function<void(CloudSync::ConflictChoice)> answer;
    {
        std::scoped_lock lock{prompts_mutex};
        const auto prompt = prompts.find(id);
        if (prompt == prompts.end()) {
            return;
        }
        answer = prompt->second;
    }
    // In NativeCloudSync.KEEP_LOCAL, KEEP_REMOTE, SKIP order.
    answer(choice == 0   ? CloudSync::ConflictChoice::KeepLocal
           : choice == 1 ? CloudSync::ConflictChoice::KeepRemote
                         : CloudSync::ConflictChoice::Skip);
}

} // extern "C"
