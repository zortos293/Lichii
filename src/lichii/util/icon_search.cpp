// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/util/icon_search.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "common/httplib.h"
#include "common/logging.h"
#include "common/net/ca_store.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/file_sys/control_metadata.h"
#include "qt_common/config/uisettings.h"

namespace {

constexpr int IconTargetSize = 256;
constexpr int MinCandidateScore = 300;
constexpr std::size_t MaxIconDownloads = 3;
constexpr std::size_t MaxSgdbChoices = 8;

std::vector<u8> EncodeSquareIcon(const QImage& source) {
    if (source.isNull()) {
        return {};
    }
    QImage img = source.convertToFormat(QImage::Format_RGBA8888);
    if (img.width() != IconTargetSize || img.height() != IconTargetSize) {
        img = img.scaled(IconTargetSize, IconTargetSize, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        if (img.width() > IconTargetSize || img.height() > IconTargetSize) {
            const int x = (img.width() - IconTargetSize) / 2;
            const int y = img.height() > img.width() ? (img.height() - IconTargetSize) / 3
                                                     : (img.height() - IconTargetSize) / 2;
            img = img.copy(x, y, IconTargetSize, IconTargetSize);
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return std::vector<u8>(encoded.begin(), encoded.end());
}

std::string NormalizeGameSlug(std::string title) {
    title = Common::ToLower(title);
    boost::replace_all(title, "é", "e");
    boost::replace_all(title, "è", "e");
    boost::replace_all(title, "ê", "e");
    boost::replace_all(title, "ō", "o");
    std::replace(title.begin(), title.end(), ' ', '-');
    std::replace(title.begin(), title.end(), '_', '-');
    std::erase_if(title, [](unsigned char c) { return !std::isalnum(c) && c != '-'; });
    title.erase(title.begin(),
                std::find_if(title.begin(), title.end(), [](char ch) { return ch != '-'; }));
    title.erase(std::find_if(title.rbegin(), title.rend(), [](char ch) { return ch != '-'; }).base(),
                title.end());
    title.erase(std::unique(title.begin(), title.end(),
                            [](char a, char b) { return a == '-' && b == '-'; }),
                title.end());
    return title;
}

std::string ResolveGameNameAlias(const std::string& raw_name, u64 title_id) {
    static const std::unordered_map<u64, std::string> by_title_id{
        {0x0400C3F00006E000ULL, "Mario Kart World"},
        {0x0100000000010000ULL, "Super Mario Odyssey"},
        {0x0100152000022000ULL, "Mario Kart 8 Deluxe"},
        {0x01007EF00011E000ULL, "The Legend of Zelda: Breath of the Wild"},
        {0x0100F2C0115B6000ULL, "The Legend of Zelda: Tears of the Kingdom"},
        {0x01006A800016E000ULL, "Super Smash Bros. Ultimate"},
        {0x01006F8002326000ULL, "Animal Crossing: New Horizons"},
    };
    static const std::unordered_map<std::string, std::string> by_alias{
        {"mkw", "Mario Kart World"},
        {"mariokartworld", "Mario Kart World"},
        {"mario-kart-world", "Mario Kart World"},
        {"dkb", "Donkey Kong Bananza"},
        {"donkeykongbananza", "Donkey Kong Bananza"},
        {"kar", "Kirby Air Riders"},
        {"kirbyairriders", "Kirby Air Riders"},
        {"mp4", "Metroid Prime 4: Beyond"},
        {"metroidprime4", "Metroid Prime 4: Beyond"},
        {"plza", "Pokemon Legends: Z-A"},
        {"pokemonlegendsza", "Pokemon Legends: Z-A"},
        {"mk8", "Mario Kart 8 Deluxe"},
        {"mk8d", "Mario Kart 8 Deluxe"},
        {"smo", "Super Mario Odyssey"},
        {"botw", "The Legend of Zelda: Breath of the Wild"},
        {"totk", "The Legend of Zelda: Tears of the Kingdom"},
        {"ssbu", "Super Smash Bros. Ultimate"},
        {"acnh", "Animal Crossing: New Horizons"},
        {"smbw", "Super Mario Bros. Wonder"},
        {"splatoon3", "Splatoon 3"},
        {"xc3", "Xenoblade Chronicles 3"},
        {"lm3", "Luigi's Mansion 3"},
        {"pikmin4", "Pikmin 4"},
    };
    const std::string slug = NormalizeGameSlug(raw_name);
    if (const auto it = by_alias.find(slug); it != by_alias.end()) {
        return it->second;
    }
    if (slug.empty()) {
        if (const auto it = by_title_id.find(title_id); it != by_title_id.end()) {
            return it->second;
        }
    }
    return raw_name;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

constexpr std::chrono::seconds SearchDeadline{10};
// Nintendo of Europe serves its 1920x1080 product art as PNG, occasionally just over 4 MiB.
constexpr std::size_t MaxHttpBodySize = 8 * 1024 * 1024;
constexpr std::chrono::seconds PrimarySourceDeadline{5};
// cpp-httplib cannot be interrupted from another thread while it connects or waits for the first
// bytes of a reply, so these waits are kept short: a cancelled lookup ends within them instead of
// at the deadline of the whole search. A download in progress stops on cancel right away.
constexpr std::chrono::milliseconds ConnectTimeout{5000};
constexpr std::chrono::milliseconds ReadTimeout{8000};
constexpr std::string_view IconSiteHost = "https://switchiconshowdown.com";
constexpr std::string_view SgdbApiBase = "https://www.steamgriddb.com/api/v2";

bool IsAllowedIconHost(std::string_view host) {
    return host == "switchiconshowdown.com" || host == "steamgriddb.com" ||
           host.ends_with(".steamgriddb.com") || host == "ec.nintendo.com" ||
           host == "www.nintendo.com" || host == "assets.nintendo.com" ||
           host == "atum-img-lp1.cdn.nintendo.net" || host == "img-eshop.cdn.nintendo.net";
}

enum class HttpStatus { Ok, NetworkError, Timeout, Cancelled, Unauthorized };

struct HttpResult {
    HttpStatus status{HttpStatus::NetworkError};
    std::string body;
    std::string detail;
};

bool IsNetworkFailure(const HttpResult& result) {
    return result.status == HttpStatus::Timeout ||
           (result.status == HttpStatus::NetworkError &&
            !result.detail.starts_with("HTTP "));
}

// Every request shares one deadline and cancel flag, so a whole search can never run past it.
HttpResult HttpGet(const std::string& full_url, std::chrono::steady_clock::time_point deadline,
                   const std::atomic_bool& cancel, const std::string& bearer_token = {}) {
    using namespace std::chrono;
    HttpResult result;
    if (cancel) {
        result.status = HttpStatus::Cancelled;
        return result;
    }
    constexpr std::string_view scheme = "https://";
    if (!full_url.starts_with(scheme)) {
        result.detail = "unsupported URL";
        return result;
    }
    const auto path_start = full_url.find('/', scheme.size());
    const std::string origin =
        path_start == std::string::npos ? full_url : full_url.substr(0, path_start);
    const std::string path =
        path_start == std::string::npos ? std::string{"/"} : full_url.substr(path_start);
    if (!IsAllowedIconHost(std::string_view{origin}.substr(scheme.size()))) {
        result.detail = "unexpected host";
        return result;
    }
    const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now());
    if (remaining.count() <= 0) {
        result.status = HttpStatus::Timeout;
        return result;
    }
    try {
        httplib::Client client(origin);
        client.set_connection_timeout(std::min(remaining, ConnectTimeout));
        client.set_read_timeout(std::min(remaining, ReadTimeout));
        client.set_write_timeout(std::min(remaining, ConnectTimeout));
        client.set_max_timeout(remaining);
        client.set_follow_location(true);
        Common::Net::ConfigureCaStore(client);
        httplib::Headers headers{{"User-Agent", "Lichii"}};
        if (!bearer_token.empty()) {
            headers.emplace("Authorization", "Bearer " + bearer_token);
        }
        // Stops a download in progress as soon as cancel is set, not only at the deadline.
        auto res = client.Get(path, headers, [&cancel](size_t, size_t) { return !cancel.load(); });
        if (!res) {
            result.status = cancel ? HttpStatus::Cancelled
                            : res.error() == httplib::Error::Canceled ||
                                    steady_clock::now() >= deadline
                                ? HttpStatus::Timeout
                                : HttpStatus::NetworkError;
            result.detail = httplib::to_string(res.error());
            return result;
        }
        if (res->status != 200) {
            result.status = res->status == 401 || res->status == 403 ? HttpStatus::Unauthorized
                                                                      : HttpStatus::NetworkError;
            result.detail = fmt::format("HTTP {}", res->status);
            return result;
        }
        if (res->body.empty() || res->body.size() > MaxHttpBodySize) {
            result.detail = "unexpected response size";
            return result;
        }
        result.status = HttpStatus::Ok;
        result.body = std::move(res->body);
    } catch (const std::exception& e) {
        result.detail = e.what();
    } catch (...) {
        result.detail = "unknown error";
    }
    return result;
}

struct IconCandidate {
    int id{};
    std::string title;
};

std::string DecodeHtmlEntities(std::string text) {
    static constexpr std::pair<std::string_view, std::string_view> entities[]{
        {"&amp;", "&"},  {"&lt;", "<"},     {"&gt;", ">"},     {"&quot;", "\""},
        {"&#039;", "'"}, {"&#39;", "'"},    {"&apos;", "'"},   {"&eacute;", "\xC3\xA9"},
    };
    for (const auto& [from, to] : entities) {
        boost::replace_all(text, from, to);
    }
    return text;
}

// Rows look like: <a href='./detail.php?id=N'><img ...></a></td><td><center>Title</center>
std::vector<IconCandidate> ParseSearchResults(const std::string& html) {
    constexpr std::string_view id_marker = "detail.php?id=";
    constexpr std::string_view title_marker = "<td><center>";
    std::vector<IconCandidate> out;
    std::size_t pos = 0;
    while (out.size() < 16) {
        const auto id_pos = html.find(id_marker, pos);
        if (id_pos == std::string::npos) {
            break;
        }
        pos = id_pos + id_marker.size();
        const auto id_end = html.find_first_not_of("0123456789", pos);
        const auto digits = html.substr(pos, id_end == std::string::npos ? id_end : id_end - pos);
        if (digits.empty() || digits.size() > 9) {
            continue;
        }
        const auto title_pos = html.find(title_marker, pos);
        if (title_pos == std::string::npos) {
            break;
        }
        const auto title_begin = title_pos + title_marker.size();
        const auto title_end = html.find("</center>", title_begin);
        if (title_end == std::string::npos) {
            break;
        }
        auto title = DecodeHtmlEntities(html.substr(title_begin, title_end - title_begin));
        if (title.find('<') == std::string::npos && !title.empty()) {
            out.push_back({std::stoi(digits), std::move(title)});
        }
        pos = title_end;
    }
    return out;
}

std::string UrlEncodePathSegment(std::string value) {
    std::ranges::replace(value, '/', ' ');
    std::ranges::replace(value, '\\', ' ');
    auto encoded = UrlEncode(value);
    boost::replace_all(encoded, "+", "%20");
    return encoded;
}

// Icon image URLs from a SteamGridDB response, best size first.
std::vector<std::string> ParseSgdbIconUrls(const std::string& body) {
    std::vector<std::string> urls;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("data") ||
        !json["data"].is_array()) {
        return urls;
    }
    for (const auto& item : json["data"]) {
        if (urls.size() >= MaxSgdbChoices) {
            break;
        }
        if (item.is_object() && item.contains("url") && item["url"].is_string()) {
            urls.push_back(item["url"].get<std::string>());
        }
    }
    return urls;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

std::string NormalizeNintendoImageUrl(std::string url) {
    if (url.empty() || !url.starts_with("https://")) {
        return {};
    }
    // Cloudinary's f_auto may answer with a format Qt cannot decode; ask for JPEG instead.
    boost::replace_all(url, "f_auto", "f_jpg");
    return url;
}

std::string NintendoAssetUrl(const nlohmann::json& asset) {
    if (!asset.is_object()) {
        return {};
    }
    if (asset.contains("url") && asset["url"].is_string()) {
        if (auto url = NormalizeNintendoImageUrl(asset["url"].get<std::string>()); !url.empty()) {
            return url;
        }
    }
    if (asset.contains("publicId") && asset["publicId"].is_string()) {
        const auto public_id = asset["publicId"].get<std::string>();
        if (public_id.starts_with("https://")) {
            return NormalizeNintendoImageUrl(public_id);
        }
        if (!public_id.empty()) {
            return "https://assets.nintendo.com/image/upload/c_fill,w_1024,h_1024/q_auto/f_jpg/" +
                   public_id;
        }
    }
    return {};
}

std::string ExtractNextData(const std::string& html) {
    constexpr std::string_view marker = "__NEXT_DATA__";
    std::size_t script = 0;
    while ((script = html.find("<script", script)) != std::string::npos) {
        const auto tag_end = html.find('>', script);
        if (tag_end == std::string::npos) {
            return {};
        }
        if (std::string_view{html}.substr(script, tag_end - script).find(marker) !=
            std::string_view::npos) {
            const auto body_end = html.find("</script>", tag_end + 1);
            if (body_end == std::string::npos) {
                return {};
            }
            return html.substr(tag_end + 1, body_end - tag_end - 1);
        }
        script = tag_end + 1;
    }
    return {};
}

bool ProductMatchesTitleId(const nlohmann::json& product, std::string_view title_id) {
    if (product.contains("applicationId") && product["applicationId"].is_string() &&
        EqualsIgnoreCase(product["applicationId"].get<std::string>(), title_id)) {
        return true;
    }
    if (product.contains("softwareDetails") && product["softwareDetails"].is_object()) {
        const auto& details = product["softwareDetails"];
        if (details.contains("rights") && details["rights"].is_array()) {
            for (const auto& right : details["rights"]) {
                if (right.is_object() && right.contains("titleId") &&
                    right["titleId"].is_string() &&
                    EqualsIgnoreCase(right["titleId"].get<std::string>(), title_id)) {
                    return true;
                }
            }
        }
    }
    return false;
}

const nlohmann::json* FindPageProps(const nlohmann::json& root) {
    const auto props = root.find("props");
    if (props == root.end() || !props->is_object()) {
        return nullptr;
    }
    const auto page = props->find("pageProps");
    return page != props->end() && page->is_object() ? &*page : nullptr;
}

std::string FindMainSku(const nlohmann::json& root) {
    const auto* page = FindPageProps(root);
    if (page == nullptr) {
        return {};
    }
    const auto sku_of = [](const nlohmann::json& item) -> std::string {
        if (item.is_object() && item.contains("sku") && item["sku"].is_string()) {
            return item["sku"].get<std::string>();
        }
        return {};
    };
    const auto linked = page->find("linkedData");
    if (linked != page->end()) {
        if (linked->is_array()) {
            for (const auto& item : *linked) {
                if (auto sku = sku_of(item); !sku.empty()) {
                    return sku;
                }
            }
        } else if (auto sku = sku_of(*linked); !sku.empty()) {
            return sku;
        }
    }
    // Some store pages leave linkedData out and keep the product only in analytics.
    const auto analytics = page->find("analytics");
    if (analytics != page->end() && analytics->is_object()) {
        const auto product = analytics->find("product");
        if (product != analytics->end()) {
            return sku_of(*product);
        }
    }
    return {};
}

std::string FindLinkedImage(const nlohmann::json& root) {
    const auto* page = FindPageProps(root);
    if (page == nullptr) {
        return {};
    }
    const auto image_of = [](const nlohmann::json& item) -> std::string {
        if (item.is_object() && item.contains("image") && item["image"].is_string()) {
            return NormalizeNintendoImageUrl(item["image"].get<std::string>());
        }
        return {};
    };
    const auto linked = page->find("linkedData");
    if (linked == page->end()) {
        return {};
    }
    if (linked->is_array()) {
        for (const auto& item : *linked) {
            if (auto image = image_of(item); !image.empty()) {
                return image;
            }
        }
    } else if (auto image = image_of(*linked); !image.empty()) {
        return image;
    }
    return {};
}

std::string FindEuMediaImage(const std::string& html) {
    // Nintendo of Europe pages render their product art from www.nintendo.com/eu/media, not
    // from the US Cloudinary store. The 1280w variant is a resized copy of the original.
    constexpr std::string_view marker = "https://www.nintendo.com/eu/media/images/assets/";
    const auto pos = html.find(marker.data());
    if (pos == std::string::npos) {
        return {};
    }
    auto end = html.find_first_of("\"' \\", pos);
    if (end == std::string::npos) {
        end = html.size();
    }
    auto url = NormalizeNintendoImageUrl(html.substr(pos, end - pos));
    if (url.empty()) {
        return {};
    }
    const auto resized = url.find("_image1280w");
    if (resized != std::string::npos) {
        auto original = url;
        original.erase(resized, std::string_view{"_image1280w"}.size());
        if (html.find(original) != std::string::npos) {
            url = std::move(original);
        }
    }
    return url;
}

std::string FindOgImage(const std::string& html) {
    constexpr std::string_view marker = "og:image";
    const auto marker_pos = html.find(marker.data());
    if (marker_pos != std::string::npos) {
        // Stay inside the matching <meta> tag: content may come before or after the property,
        // and a neighbouring tag's content is not this image.
        auto tag_begin = html.rfind("<meta", marker_pos);
        if (tag_begin == std::string::npos) {
            tag_begin = 0;
        }
        auto tag_end = html.find('>', marker_pos);
        if (tag_end == std::string::npos) {
            tag_end = html.size();
        }
        const std::string_view tag(html.data() + tag_begin, tag_end - tag_begin);
        for (const std::string_view attribute : {std::string_view{"content=\""},
                                                 std::string_view{"content='"}}) {
            const auto content = tag.find(attribute);
            if (content == std::string_view::npos) {
                continue;
            }
            const auto begin = content + attribute.size();
            const auto end = tag.find(attribute.back(), begin);
            if (end == std::string_view::npos) {
                continue;
            }
            if (auto url =
                    NormalizeNintendoImageUrl(std::string(tag.substr(begin, end - begin)));
                !url.empty()) {
                return url;
            }
        }
    }
    // The Hong Kong store page also carries the same image without Open Graph metadata.
    constexpr std::string_view eshop = "https://img-eshop.cdn.nintendo.net/i/";
    const auto eshop_pos = html.find(eshop.data());
    if (eshop_pos != std::string::npos) {
        auto end = html.find_first_of("\"' \\", eshop_pos);
        if (end == std::string::npos) {
            end = html.size();
        }
        return NormalizeNintendoImageUrl(html.substr(eshop_pos, end - eshop_pos));
    }
    return {};
}

std::string FindNintendoStoreImageUrl(const std::string& html, std::string_view title_id) {
    const auto next_data = ExtractNextData(html);
    if (!next_data.empty()) {
        const auto root = nlohmann::json::parse(next_data, nullptr, false);
        if (!root.is_discarded() && root.is_object()) {
            if (const auto* page = FindPageProps(root); page != nullptr) {
                const auto state = page->find("initialApolloState");
                if (state == page->end() || !state->is_object()) {
                    if (auto image = FindLinkedImage(root); !image.empty()) {
                        return image;
                    }
                    if (auto image = FindEuMediaImage(html); !image.empty()) {
                        return image;
                    }
                    return FindOgImage(html);
                }
                const std::string square_key = "productImage({\"shape\":\"square\"})";
                const auto main_sku = FindMainSku(root);
                const nlohmann::json* main_product = nullptr;
                const nlohmann::json* main_square = nullptr;
                for (auto it = state->begin(); it != state->end(); ++it) {
                    if (!it.value().is_object()) {
                        continue;
                    }
                    const auto& product = it.value();
                    const auto square = product.find(square_key);
                    if (!main_sku.empty() && product.contains("sku") &&
                        product["sku"].is_string() &&
                        product["sku"].get<std::string>() == main_sku) {
                        main_product = &product;
                        main_square = square == product.end() ? nullptr : &*square;
                    }
                    if (square != product.end() && ProductMatchesTitleId(product, title_id)) {
                        if (auto url = NintendoAssetUrl(*square); !url.empty()) {
                            return url;
                        }
                    }
                }
                if (main_square != nullptr) {
                    if (auto url = NintendoAssetUrl(*main_square); !url.empty()) {
                        return url;
                    }
                }
                if (main_product != nullptr) {
                    const auto image = main_product->find("productImage");
                    if (image != main_product->end()) {
                        if (auto url = NintendoAssetUrl(*image); !url.empty()) {
                            return url;
                        }
                    }
                }
            }
            if (auto image = FindLinkedImage(root); !image.empty()) {
                return image;
            }
        }
    }
    if (auto image = FindEuMediaImage(html); !image.empty()) {
        return image;
    }
    return FindOgImage(html);
}

QImage CenterSquare(const QImage& image) {
    if (image.isNull() || image.width() == image.height()) {
        return image;
    }
    const int side = std::min(image.width(), image.height());
    return image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
}

int ScoreCandidateTitle(const std::string& candidate, const std::string& query) {
    const std::string c_slug = NormalizeGameSlug(candidate);
    const std::string q_slug = NormalizeGameSlug(query);
    if (c_slug.empty() || q_slug.empty()) {
        return -1000;
    }
    if (c_slug == q_slug) {
        return 1000;
    }
    int score = 0;
    if (c_slug.starts_with(q_slug + "-") || q_slug.starts_with(c_slug + "-")) {
        score += 750 - static_cast<int>(std::abs(static_cast<long>(c_slug.size()) -
                                                 static_cast<long>(q_slug.size())));
    } else if (c_slug.find(q_slug) != std::string::npos) {
        score += 600;
    }
    const auto split_words = [](const std::string& s) {
        std::set<std::string> words;
        std::size_t start = 0;
        while (start < s.size()) {
            const auto dash = s.find('-', start);
            const auto word = s.substr(start, dash - start);
            if (!word.empty()) {
                words.insert(word);
            }
            if (dash == std::string::npos) {
                break;
            }
            start = dash + 1;
        }
        return words;
    };
    const auto q_words = split_words(q_slug);
    const auto c_words = split_words(c_slug);
    int overlap = 0;
    for (const auto& w : q_words) {
        if (c_words.contains(w)) {
            ++overlap;
        }
    }
    score += overlap * 120 - static_cast<int>(c_words.size()) * 10;
    const std::string lower_cand = Common::ToLower(candidate);
    if (lower_cand.find("(series)") != std::string::npos ||
        lower_cand.find("(character)") != std::string::npos ||
        lower_cand.find("soundtrack") != std::string::npos) {
        score -= 400;
    }
    return score;
}


IconSearchResult RunIconSearch(const std::string& raw_query, u64 tid, const std::string& api_key,
                               const std::atomic_bool& cancel) {
    using namespace std::chrono;
    IconSearchResult out;
    try {
        const auto start = steady_clock::now();
        const auto deadline = start + SearchDeadline;
        const std::string search_name = ResolveGameNameAlias(raw_query, tid);
        if (search_name != raw_query) {
            out.resolved_title = search_name;
        }
        if (search_name.empty()) {
            out.status = IconSearchStatus::NotFound;
            return out;
        }

        const auto to_status = [](const HttpResult& res) {
            switch (res.status) {
            case HttpStatus::Timeout:
                return IconSearchStatus::Timeout;
            case HttpStatus::Cancelled:
                return IconSearchStatus::Cancelled;
            case HttpStatus::Unauthorized:
                return IconSearchStatus::BadApiKey;
            default:
                return IconSearchStatus::NetworkError;
            }
        };
        const auto decode_icon = [&out](const std::string& body) {
            QImage img;
            if (img.loadFromData(reinterpret_cast<const uchar*>(body.data()),
                                 static_cast<int>(body.size()))) {
                out.icon_bytes = EncodeSquareIcon(img);
            }
            return !out.icon_bytes.empty();
        };

        // Primary: switchiconshowdown.com, limited so the fallback keeps some of the time.
        IconSearchStatus primary = IconSearchStatus::NotFound;
        std::string primary_error;
        const auto primary_deadline = std::min(deadline, start + PrimarySourceDeadline);
        do {
            std::vector<std::string> queries{search_name};
            if (const auto colon = search_name.find(':');
                colon != std::string::npos && colon > 0) {
                queries.push_back(search_name.substr(0, colon));
            }
            std::vector<IconCandidate> candidates;
            for (const auto& query : queries) {
                const auto res =
                    HttpGet(std::string{IconSiteHost} + "/search.php?query=" + UrlEncode(query),
                            primary_deadline, cancel);
                if (res.status != HttpStatus::Ok) {
                    primary = to_status(res);
                    primary_error = res.detail;
                    break;
                }
                candidates = ParseSearchResults(res.body);
                if (!candidates.empty()) {
                    break;
                }
            }
            if (primary != IconSearchStatus::NotFound) {
                break;
            }

            // The site can list one game several times; keep the closest title first.
            std::vector<std::pair<int, IconCandidate>> scored;
            for (auto& cand : candidates) {
                const int score = ScoreCandidateTitle(cand.title, search_name);
                if (score >= MinCandidateScore) {
                    scored.emplace_back(score, std::move(cand));
                }
            }
            std::ranges::stable_sort(
                scored, [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < scored.size() && i < MaxIconDownloads; ++i) {
                const auto res = HttpGet(fmt::format("{}/images/100/{}.png", IconSiteHost,
                                                     scored[i].second.id),
                                         primary_deadline, cancel);
                if (res.status != HttpStatus::Ok) {
                    primary = to_status(res);
                    primary_error = res.detail;
                    break;
                }
                if (decode_icon(res.body)) {
                    out.status = IconSearchStatus::Found;
                    return out;
                }
            }
        } while (false);

        if (primary == IconSearchStatus::Cancelled) {
            out.status = primary;
            return out;
        }

        // Fallback: SteamGridDB, which needs the user's API key.
        if (api_key.empty()) {
            out.status = primary == IconSearchStatus::NotFound ? IconSearchStatus::NeedsApiKey
                                                               : primary;
            out.error = primary_error;
            return out;
        }
        const auto sgdb_get = [&](const std::string& path) {
            return HttpGet(std::string{SgdbApiBase} + path, deadline, cancel, api_key);
        };
        const auto search = sgdb_get("/search/autocomplete/" + UrlEncodePathSegment(search_name));
        if (search.status != HttpStatus::Ok) {
            out.status = to_status(search);
            out.error = search.detail;
            return out;
        }
        const auto parsed = nlohmann::json::parse(search.body, nullptr, false);
        int game_id = 0;
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("data") &&
            parsed["data"].is_array() && !parsed["data"].empty() &&
            parsed["data"][0].is_object() && parsed["data"][0].contains("id") &&
            parsed["data"][0]["id"].is_number_integer()) {
            game_id = parsed["data"][0]["id"].get<int>();
        }
        if (game_id <= 0) {
            out.status = IconSearchStatus::NotFound;
            return out;
        }
        const auto icons =
            sgdb_get(fmt::format("/icons/game/{}?dimensions=256,512&mimes=image/png", game_id));
        if (icons.status != HttpStatus::Ok) {
            out.status = to_status(icons);
            out.error = icons.detail;
            return out;
        }
        // Collect every icon that arrives in time; the user picks one.
        for (const auto& url : ParseSgdbIconUrls(icons.body)) {
            const auto img = HttpGet(url, deadline, cancel);
            if (img.status != HttpStatus::Ok) {
                if (img.status == HttpStatus::Cancelled || img.status == HttpStatus::Timeout) {
                    if (out.choices.empty()) {
                        out.status = to_status(img);
                        out.error = img.detail;
                        return out;
                    }
                    break;
                }
                continue;
            }
            if (decode_icon(img.body)) {
                out.choices.push_back(std::move(out.icon_bytes));
                out.icon_bytes.clear();
            }
        }
        if (!out.choices.empty()) {
            out.status = IconSearchStatus::Choices;
            return out;
        }
        out.status = IconSearchStatus::NotFound;
    } catch (const std::exception& e) {
        out.status = IconSearchStatus::InternalError;
        out.error = e.what();
    } catch (...) {
        out.status = IconSearchStatus::InternalError;
        out.error = "unknown error";
    }
    return out;
}

} // namespace

ArtworkLookupResult LookUpSteamGridDbArtwork(const std::string& title, const std::string& api_key,
                                             const std::atomic_bool& cancel, bool prefer_newest) {
    ArtworkLookupResult out;
    // A trailing "(2026)" is not part of the name SteamGridDB knows; it says which release.
    std::string search_title = title;
    int wanted_year = 0;
    if (const auto open = search_title.rfind('(');
        open != std::string::npos && open > 0 && search_title.ends_with(')')) {
        const std::string inner = search_title.substr(open + 1, search_title.size() - open - 2);
        if (inner.size() == 4 && std::ranges::all_of(inner, [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            wanted_year = std::stoi(inner);
            search_title.erase(open);
            while (!search_title.empty() &&
                   std::isspace(static_cast<unsigned char>(search_title.back()))) {
                search_title.pop_back();
            }
        }
    }
    if (title.empty() || search_title.empty() || api_key.empty()) {
        out.status = ArtworkLookupStatus::NotFound;
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    const auto failed = [&out](const HttpResult& res) {
        out.status = res.status == HttpStatus::Unauthorized ? ArtworkLookupStatus::BadApiKey
                     : res.status == HttpStatus::Cancelled  ? ArtworkLookupStatus::Cancelled
                                                            : ArtworkLookupStatus::NetworkError;
        out.detail = res.detail;
        return out;
    };
    const auto sgdb_get = [&](const std::string& path) {
        return HttpGet(std::string{SgdbApiBase} + path, deadline, cancel, api_key);
    };
    try {
        const auto search =
            sgdb_get("/search/autocomplete/" + UrlEncodePathSegment(search_title));
        if (search.status != HttpStatus::Ok) {
            return failed(search);
        }
        // The first autocomplete entry is SteamGridDB's best match. Entries with the same name
        // are other releases of it (an original and its remake); the year picks between them.
        struct Entry {
            int id{};
            std::string name;
            int year{};
            s64 release{};
        };
        std::vector<Entry> entries;
        const auto parsed = nlohmann::json::parse(search.body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("data") &&
            parsed["data"].is_array()) {
            for (const auto& item : parsed["data"]) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer()) {
                    continue;
                }
                Entry entry{.id = item["id"].get<int>()};
                if (item.contains("name") && item["name"].is_string()) {
                    entry.name = item["name"].get<std::string>();
                }
                if (item.contains("release_date") && item["release_date"].is_number_integer()) {
                    entry.release = item["release_date"].get<s64>();
                    entry.year =
                        QDateTime::fromSecsSinceEpoch(entry.release).toUTC().date().year();
                }
                entries.push_back(std::move(entry));
            }
        }
        const auto simple_name = [](const std::string& name) {
            std::string out_name;
            for (const unsigned char c : name) {
                if (std::isalnum(c)) {
                    out_name += static_cast<char>(std::tolower(c));
                }
            }
            return out_name;
        };
        int game_id = 0;
        if (!entries.empty()) {
            const Entry* chosen = &entries.front();
            const std::string best_name = simple_name(chosen->name);
            const std::string searched_name = simple_name(search_title);
            // The year only picks between releases of the game: an entry of that year with
            // another name is a different game.
            const Entry* same_name_in_year = nullptr;
            const Entry* searched_name_in_year = nullptr;
            if (wanted_year != 0) {
                for (const auto& entry : entries) {
                    const std::string name = simple_name(entry.name);
                    // Names with no ASCII letters or digits (Japanese-only) all match each other.
                    if (entry.year != wanted_year || name.empty()) {
                        continue;
                    }
                    if (!same_name_in_year && name == best_name) {
                        same_name_in_year = &entry;
                    }
                    if (!searched_name_in_year && name == searched_name) {
                        searched_name_in_year = &entry;
                    }
                }
            }
            if (same_name_in_year || searched_name_in_year) {
                chosen = same_name_in_year ? same_name_in_year : searched_name_in_year;
            } else if (prefer_newest && !best_name.empty()) {
                for (const auto& entry : entries) {
                    if (simple_name(entry.name) == best_name && entry.release > chosen->release) {
                        chosen = &entry;
                    }
                }
            }
            game_id = chosen->id;
        }
        if (game_id <= 0) {
            out.status = ArtworkLookupStatus::NotFound;
            return out;
        }
        // Square grids first, then icons.
        for (const auto& list_path :
             {fmt::format("/grids/game/{}?dimensions=512x512,1024x1024", game_id),
              fmt::format("/icons/game/{}", game_id)}) {
            const auto list = sgdb_get(list_path);
            if (list.status != HttpStatus::Ok) {
                return failed(list);
            }
            for (const auto& url : ParseSgdbIconUrls(list.body)) {
                const auto image = HttpGet(url, deadline, cancel);
                if (image.status == HttpStatus::Cancelled || image.status == HttpStatus::Timeout) {
                    return failed(image);
                }
                QImage decoded;
                if (image.status == HttpStatus::Ok &&
                    decoded.loadFromData(reinterpret_cast<const uchar*>(image.body.data()),
                                         static_cast<int>(image.body.size()))) {
                    out.status = ArtworkLookupStatus::Found;
                    out.image = std::move(decoded);
                    return out;
                }
            }
        }
        out.status = ArtworkLookupStatus::NotFound;
    } catch (const std::exception& e) {
        out.status = ArtworkLookupStatus::NetworkError;
        out.detail = e.what();
    }
    return out;
}

static std::vector<std::string> NintendoStoreRegions(int language_index) {
    switch (static_cast<Settings::Language>(language_index)) {
    case Settings::Language::EnglishBritish:
        return {"GB", "US", "HK"};
    case Settings::Language::Japanese:
        return {"JP", "US", "HK"};
    case Settings::Language::French:
        return {"FR", "GB", "US", "HK"};
    case Settings::Language::FrenchCanadian:
        return {"FR", "US", "HK"};
    case Settings::Language::German:
        return {"DE", "GB", "US", "HK"};
    case Settings::Language::Spanish:
        return {"ES", "GB", "US", "HK"};
    case Settings::Language::SpanishLatin:
        return {"ES", "US", "HK"};
    case Settings::Language::Italian:
        return {"IT", "GB", "US", "HK"};
    case Settings::Language::Dutch:
        return {"NL", "GB", "US", "HK"};
    case Settings::Language::Portuguese:
        return {"PT", "GB", "US", "HK"};
    case Settings::Language::PortugueseBrazilian:
        return {"PT", "US", "HK"};
    case Settings::Language::Russian:
        return {"US", "HK"};
    case Settings::Language::Korean:
        return {"US", "HK"};
    case Settings::Language::Chinese:
    case Settings::Language::ChineseSimplified:
    case Settings::Language::ChineseTraditional:
    case Settings::Language::Taiwanese:
        return {"HK", "US"};
    case Settings::Language::Polish:
        return {"GB", "US", "HK"};
    case Settings::Language::Thai:
        return {"HK", "US"};
    default:
        return {"US", "HK"};
    }
}

ArtworkLookupResult LookUpNintendoStoreArtwork(u64 title_id, int language_index,
                                               const std::atomic_bool& cancel) {
    ArtworkLookupResult out;
    if (title_id == 0) {
        out.status = ArtworkLookupStatus::NotFound;
        return out;
    }

    const std::string exact = fmt::format("{:016X}", title_id);
    std::vector<std::string> ids{exact};
    // A Switch 2 Edition is sold from its Switch 1 base game's store page.
    if (exact.starts_with("04") && exact.ends_with("001")) {
        std::string base = exact;
        base[1] = '1';
        base[base.size() - 1] = '0';
        base[base.size() - 2] = '0';
        base[base.size() - 3] = '0';
        ids.push_back(std::move(base));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    bool network_error = false;
    std::string detail;
    // Non-square key art is only a last resort. A localized store often serves a 16:9 banner,
    // while another region has the proper square cover the grid expects.
    QImage fallback;
    std::string fallback_detail;
    const auto regions = NintendoStoreRegions(language_index);
    for (const auto& id : ids) {
        for (const auto& region : regions) {
            if (cancel) {
                out.status = ArtworkLookupStatus::Cancelled;
                return out;
            }

            const auto page = HttpGet(
                fmt::format("https://ec.nintendo.com/apps/{}/{}", id, region), deadline, cancel);
            if (page.status == HttpStatus::Cancelled) {
                out.status = ArtworkLookupStatus::Cancelled;
                return out;
            }
            if (page.status != HttpStatus::Ok) {
                network_error = network_error || IsNetworkFailure(page);
                detail = page.detail;
                continue;
            }

            const auto image_url = FindNintendoStoreImageUrl(page.body, exact);
            if (image_url.empty()) {
                detail = "no store image";
                continue;
            }

            const auto image = HttpGet(image_url, deadline, cancel);
            if (image.status == HttpStatus::Cancelled) {
                out.status = ArtworkLookupStatus::Cancelled;
                return out;
            }
            if (image.status != HttpStatus::Ok) {
                network_error = network_error || IsNetworkFailure(image);
                detail = image.detail;
                continue;
            }

            QImage decoded;
            if (!decoded.loadFromData(reinterpret_cast<const uchar*>(image.body.data()),
                                      static_cast<int>(image.body.size()))) {
                detail = "image could not be decoded";
                continue;
            }

            const auto image_detail = fmt::format("{} {}", id, region);
            if (decoded.width() == decoded.height()) {
                out.image = CenterSquare(decoded);
                if (out.image.isNull()) {
                    detail = "empty image";
                    continue;
                }
                out.status = ArtworkLookupStatus::Found;
                out.detail = image_detail;
                out.from_official_store = true;
                return out;
            }
            if (fallback.isNull()) {
                fallback = CenterSquare(decoded);
                fallback_detail = image_detail;
            }
        }
    }

    if (!fallback.isNull()) {
        out.image = std::move(fallback);
        out.status = ArtworkLookupStatus::Found;
        out.detail = std::move(fallback_detail);
        out.from_official_store = true;
        return out;
    }

    out.status =
        network_error ? ArtworkLookupStatus::NetworkError : ArtworkLookupStatus::NotFound;
    if (detail.empty()) {
        out.detail = "Nintendo store returned no artwork";
    } else {
        out.detail = std::move(detail);
    }
    return out;
}

IconSearch::IconSearch(QWidget* parent) : QObject(parent), parent_widget{parent} {
    connect(&watcher, &QFutureWatcher<IconSearchResult>::finished, this, &IconSearch::OnFinished);
}

IconSearch::~IconSearch() {
    // Never block the UI thread: the detached worker stops on its own once it sees the flag.
    cancel_flag->store(true);
}

void IconSearch::Start(const QString& name, u64 title_id) {
    if (watcher.isRunning()) {
        return;
    }
    const std::string query = name.trimmed().toStdString();
    if (query.empty() && title_id == 0) {
        QMessageBox::information(parent_widget, tr("Find Icon"),
                                 tr("Please enter a game name first so the icon can be found."));
        return;
    }
    last_name = name;
    last_title_id = title_id;
    cancel_flag->store(false);
    emit RunningChanged(true);
    // The worker captures no pointer to the dialog, so it may safely outlive it.
    watcher.setFuture(QtConcurrent::run(
        [query, title_id, api_key = UISettings::values.steamgriddb_api_key.GetValue(),
         cancel = cancel_flag] { return RunIconSearch(query, title_id, api_key, *cancel); }));
}

bool IconSearch::IsRunning() const {
    return watcher.isRunning();
}

int IconSearch::ChooseIcon(const std::vector<std::vector<u8>>& choices) {
    if (choices.size() == 1) {
        return 0;
    }
    QDialog dialog(parent_widget);
    dialog.setWindowTitle(tr("Choose an Icon"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(
        new QLabel(tr("SteamGridDB has several icons for this game. Pick one:"), &dialog));
    auto* list = new QListWidget(&dialog);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(128, 128));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSpacing(8);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        QPixmap map;
        if (map.loadFromData(choices[i].data(), static_cast<uint>(choices[i].size()))) {
            auto* item = new QListWidgetItem(QIcon(map), QString::number(i + 1));
            item->setData(Qt::UserRole, static_cast<int>(i));
            list->addItem(item);
        }
    }
    if (list->count() == 0) {
        return -1;
    }
    list->setCurrentRow(0);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog.resize(520, 420);
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return -1;
    }
    return list->currentItem()->data(Qt::UserRole).toInt();
}

bool IconSearch::AskForApiKey(bool missing) {
    QString text = missing ? tr("No matching icon was found on switchiconshowdown.com. A "
                                "SteamGridDB API key enables a fallback search.")
                           : tr("SteamGridDB rejected the saved API key.");
    if (UISettings::values.steamgriddb_api_key.GetValue().empty()) {
        text += QStringLiteral("<br><br>") +
                tr("Create a free key on the <a href=\"%1\">SteamGridDB API preferences</a> page.")
                    .arg(QStringLiteral("https://www.steamgriddb.com/profile/preferences/api"));
    }
    QDialog dialog(parent_widget);
    dialog.setWindowTitle(tr("SteamGridDB API Key"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text, &dialog);
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);
    auto* key_edit = new QLineEdit(
        QString::fromStdString(UISettings::values.steamgriddb_api_key.GetValue()), &dialog);
    key_edit->setEchoMode(QLineEdit::Password);
    key_edit->setPlaceholderText(tr("API key"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(label);
    layout->addWidget(key_edit);
    layout->addWidget(buttons);
    dialog.setMinimumWidth(420);
    if (dialog.exec() != QDialog::Accepted || key_edit->text().trimmed().isEmpty()) {
        return false;
    }
    UISettings::values.steamgriddb_api_key.SetValue(key_edit->text().trimmed().toStdString());
    return true;
}

void IconSearch::OnFinished() {
    emit RunningChanged(false);

    IconSearchResult result;
    try {
        result = watcher.result();
    } catch (const std::exception& e) {
        result.status = IconSearchStatus::InternalError;
        result.error = e.what();
    } catch (...) {
        result.status = IconSearchStatus::InternalError;
    }

    const auto select = [&](std::vector<u8> png) {
        emit IconSelected(QByteArray(reinterpret_cast<const char*>(png.data()),
                                     static_cast<qsizetype>(png.size())),
                          QString::fromStdString(result.resolved_title));
    };

    QString message;
    switch (result.status) {
    case IconSearchStatus::Found:
        select(std::move(result.icon_bytes));
        return;
    case IconSearchStatus::Choices: {
        const int index = ChooseIcon(result.choices);
        if (index >= 0) {
            select(std::move(result.choices[static_cast<std::size_t>(index)]));
        }
        return;
    }
    case IconSearchStatus::NotFound:
        message = tr("No matching icon was found on switchiconshowdown.com or SteamGridDB. Try a "
                     "different game name or choose an icon file.");
        break;
    case IconSearchStatus::NeedsApiKey:
    case IconSearchStatus::BadApiKey:
        if (AskForApiKey(result.status == IconSearchStatus::NeedsApiKey)) {
            Start(last_name, last_title_id);
        }
        return;
    case IconSearchStatus::Timeout:
        message = tr("Searching for an icon took longer than %1 seconds and was stopped. Check "
                     "your connection and try again.")
                      .arg(SearchDeadline.count());
        break;
    case IconSearchStatus::NetworkError:
        message = tr("Could not reach the icon sources (%1). Check your connection and try again.")
                      .arg(QString::fromStdString(result.error));
        break;
    case IconSearchStatus::Cancelled:
        return;
    case IconSearchStatus::InternalError:
        message = tr("Searching for an icon failed unexpectedly: %1")
                      .arg(QString::fromStdString(result.error));
        break;
    }
    LOG_WARNING(Frontend, "Icon search failed: {}", message.toStdString());
    QMessageBox::warning(parent_widget, tr("Find Icon"), message);
}
