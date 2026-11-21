// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <zlib.h>

#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/set/settings_server.h"

namespace FileSys {

const std::array<const char*, size_t(Language::Count)> LANGUAGE_NAMES{{
    "AmericanEnglish",
    "BritishEnglish",
    "Japanese",
    "French",
    "German",
    "LatinAmericanSpanish",
    "Spanish",
    "Italian",
    "Dutch",
    "CanadianFrench",
    "Portuguese",
    "Russian",
    "Korean",
    "TraditionalChinese",
    "SimplifiedChinese",
    "BrazilianPortuguese",
    "Polish",
    "Thai",
}};

Language GetConfiguredLanguage() {
    switch (Settings::values.language_index.GetValue()) {
    case Settings::Language::Chinese:
    case Settings::Language::ChineseSimplified:
        return Language::SimplifiedChinese;
    case Settings::Language::ChineseTraditional:
    case Settings::Language::Taiwanese:
        return Language::TraditionalChinese;
    case Settings::Language::Dutch:
        return Language::Dutch;
    case Settings::Language::EnglishAmerican:
        return Language::AmericanEnglish;
    case Settings::Language::EnglishBritish:
        return Language::BritishEnglish;
    case Settings::Language::French:
        return Language::French;
    case Settings::Language::FrenchCanadian:
        return Language::CanadianFrench;
    case Settings::Language::German:
        return Language::German;
    case Settings::Language::Italian:
        return Language::Italian;
    case Settings::Language::Korean:
        return Language::Korean;
    case Settings::Language::Japanese:
        return Language::Japanese;
    case Settings::Language::Portuguese:
        return Language::Portuguese;
    case Settings::Language::PortugueseBrazilian:
        return Language::BrazilianPortuguese;
    case Settings::Language::Russian:
        return Language::Russian;
    case Settings::Language::Spanish:
        return Language::Spanish;
    case Settings::Language::SpanishLatin:
        return Language::LatinAmericanSpanish;
    case Settings::Language::Polish:
        return Language::Polish;
    case Settings::Language::Thai:
        return Language::Thai;
    default:
        return Language::AmericanEnglish;
    }
}

std::string_view GetLanguageTag(Language language) {
    switch (language) {
    case Language::AmericanEnglish:
        return "en-us";
    case Language::BritishEnglish:
        return "en-gb";
    case Language::Japanese:
        return "ja";
    case Language::French:
        return "fr";
    case Language::German:
        return "de";
    case Language::LatinAmericanSpanish:
        return "es-419";
    case Language::Spanish:
        return "es";
    case Language::Italian:
        return "it";
    case Language::Dutch:
        return "nl";
    case Language::CanadianFrench:
        return "fr-ca";
    case Language::Portuguese:
        return "pt";
    case Language::Russian:
        return "ru";
    case Language::Korean:
        return "ko";
    case Language::TraditionalChinese:
        return "zh-hant";
    case Language::SimplifiedChinese:
        return "zh-hans";
    case Language::BrazilianPortuguese:
        return "pt-br";
    case Language::Polish:
        return "pl";
    case Language::Thai:
        return "th";
    default:
        return "en-us";
    }
}

static std::string_view GetLanguageBaseTag(Language language) {
    switch (language) {
    case Language::BritishEnglish:
        return "en-us";
    case Language::CanadianFrench:
        return "fr";
    case Language::LatinAmericanSpanish:
        return "es";
    case Language::BrazilianPortuguese:
        return "pt";
    default:
        return GetLanguageTag(language);
    }
}

std::string GetConfiguredLanguageTag() {
    return std::string(GetLanguageTag(GetConfiguredLanguage()));
}

std::vector<Language> GetLanguagePriority() {
    const auto language_code = Service::Set::GetLanguageCodeFromIndex(
        static_cast<u32>(Settings::values.language_index.GetValue()));
    const auto application_language =
        Service::NS::ConvertToApplicationLanguage(language_code)
            .value_or(Service::NS::ApplicationLanguage::AmericanEnglish);
    const auto priority = Service::NS::GetApplicationLanguagePriorityList(application_language);

    std::vector<Language> languages;
    languages.reserve(static_cast<std::size_t>(Language::Count));
    if (priority != nullptr) {
        for (const auto language : *priority) {
            const auto index = static_cast<std::size_t>(language);
            if (index < static_cast<std::size_t>(Language::Count)) {
                languages.push_back(static_cast<Language>(index));
            }
        }
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(Language::Count); ++index) {
        const auto language = static_cast<Language>(index);
        if (std::find(languages.begin(), languages.end(), language) == languages.end()) {
            languages.push_back(language);
        }
    }
    return languages;
}

std::vector<std::string> GetLanguagePriorityNames() {
    std::vector<std::string> names;
    for (const auto language : GetLanguagePriority()) {
        names.emplace_back(LANGUAGE_NAMES[static_cast<std::size_t>(language)]);
    }
    return names;
}

std::vector<std::string> GetPreferredLanguageTags() {
    std::vector<std::string> tags;
    const auto add = [&](std::string_view tag) {
        if (!tag.empty() &&
            std::find(tags.begin(), tags.end(), std::string(tag)) == tags.end()) {
            tags.emplace_back(tag);
        }
    };
    const auto language = GetConfiguredLanguage();
    add(GetLanguageTag(language));
    add(GetLanguageBaseTag(language));
    add("en-us");
    return tags;
}

namespace
{
    constexpr std::size_t MAX_EXPANDED_LANG_SIZE = sizeof(LanguageEntry) * 32;


    bool InflateRawDeflate(std::span<const u8> compressed, std::vector<u8>& out)
    {
        if (compressed.empty()) return false;

        z_stream stream{};
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
        stream.avail_in = static_cast<uInt>(compressed.size());
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            return false;
        }

        out.resize(MAX_EXPANDED_LANG_SIZE);
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());

        int ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);

        if (ret != Z_STREAM_END) {
            return false;
        }

        // Shrink to actual decompressed size
        out.resize(stream.total_out);
        return true;
    }
} // namespace

std::string LanguageEntry::GetApplicationName() const {
    return Common::StringFromFixedZeroTerminatedBuffer(application_name.data(), application_name.size());
}

std::string LanguageEntry::GetDeveloperName() const {
    return Common::StringFromFixedZeroTerminatedBuffer(developer_name.data(), developer_name.size());
}

NACP::NACP() = default;

NACP::NACP(VirtualFile file)
{
    file->ReadObject(&raw);
    if (raw.titles_data_format == TitleDataFormat::Compressed) {
        const auto& data = raw.language_entries.compressed_data;
        if (data.buffer_size > sizeof(data.buffer)) {
            return;
        }
        const std::span<const u8> compressed_payload{data.buffer, data.buffer_size};

        std::vector<u8> decompressed;
        if (InflateRawDeflate(compressed_payload, decompressed) && !decompressed.empty() &&
            decompressed.size() % sizeof(LanguageEntry) == 0) {
            const size_t entry_count = decompressed.size() / sizeof(LanguageEntry);
            language_entries.resize(entry_count);
            std::memcpy(language_entries.data(), decompressed.data(), decompressed.size());
        }
    } else {
        language_entries.resize(16);
        std::memcpy(language_entries.data(), raw.language_entries.language_entries.data(),
                    sizeof(raw.language_entries.language_entries));
    }
}

NACP::~NACP() = default;

const LanguageEntry& NACP::GetLanguageEntry() const {
    for (const auto language : GetLanguagePriority()) {
        const auto index = static_cast<std::size_t>(language);
        if (index < language_entries.size() &&
            !language_entries[index].GetApplicationName().empty()) {
            return language_entries[index];
        }
    }

    for (const auto& entry : language_entries) {
        if (!entry.GetApplicationName().empty()) {
            return entry;
        }
    }

    if (!language_entries.empty()) {
        return language_entries.front();
    }

    static const LanguageEntry empty_entry{};
    return empty_entry;
}

std::vector<std::string> NACP::GetApplicationNames() const {
    std::vector<std::string> names;
    names.reserve(language_entries.size());
    for (const auto& entry : language_entries) {
        names.push_back(entry.GetApplicationName());
    }
    return names;
}

std::string NACP::GetApplicationName() const {
    return GetLanguageEntry().GetApplicationName();
}

std::string NACP::GetDeveloperName() const {
    return GetLanguageEntry().GetDeveloperName();
}

u64 NACP::GetTitleId() const {
    return raw.save_data_owner_id;
}

u64 NACP::GetDLCBaseTitleId() const {
    return raw.dlc_base_title_id;
}

std::string NACP::GetVersionString() const {
    return Common::StringFromFixedZeroTerminatedBuffer(raw.version_string.data(),
                                                       raw.version_string.size());
}

u64 NACP::GetDefaultNormalSaveSize() const {
    return raw.user_account_save_data_size;
}

u64 NACP::GetDefaultJournalSaveSize() const {
    return raw.user_account_save_data_journal_size;
}

bool NACP::GetUserAccountSwitchLock() const {
    return raw.user_account_switch_lock != 0;
}

u32 NACP::GetSupportedLanguages() const {
    return u32(raw.supported_languages);
}

u64 NACP::GetDeviceSaveDataSize() const {
    return raw.device_save_data_size;
}

u32 NACP::GetParentalControlFlag() const {
    return raw.parental_control;
}

const std::array<u8, 0x20>& NACP::GetRatingAge() const {
    return raw.rating_age;
}

std::vector<u8> NACP::GetRawBytes() const {
    std::vector<u8> out(sizeof(RawNACP));
    std::memcpy(out.data(), &raw, sizeof(RawNACP));
    return out;
}
} // namespace FileSys
