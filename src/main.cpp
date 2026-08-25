#include <SDL.h>
#include <curl/curl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <json-glib/json-glib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <fcntl.h>
#include <iomanip>
#include <linux/input.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <ctime>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static constexpr const char *VERSION = "1.0.1";
static constexpr const char *PROVIDER = "https://www.romsgames.net";

static std::string envOr(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}
static std::string appHome() { return envOr("ROM_LIBRARY_HOME", "/mnt/data/rom-library"); }
static std::string romRoot() { return envOr("ROM_LIBRARY_ROMS_ROOT", "/mnt/mmc/Roms"); }
static std::string helperPath() { return envOr("ROM_LIBRARY_HELPER", "/mnt/data/rom-library/bin/romlib_helper.py"); }
static std::string downloadRoot() { return envOr("ROM_LIBRARY_DOWNLOAD_ROOT", "/mnt/mmc/.rom-library-downloads"); }
static std::string trashRoot() { return envOr("ROM_LIBRARY_TRASH_ROOT", "/mnt/mmc/.rom-library-trash"); }

static std::string trim(std::string value) {
    auto space = [](unsigned char c) { return std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !space(c); }).base(), value.end());
    return value;
}
static std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}
static bool endsWith(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
static std::string formatBytes(uint64_t bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double amount = double(bytes); int unit = 0;
    while (amount >= 1024.0 && unit < 4) { amount /= 1024.0; ++unit; }
    std::ostringstream out;
    if (unit == 0) out << uint64_t(amount); else out << std::fixed << std::setprecision(amount >= 100 ? 0 : amount >= 10 ? 1 : 2) << amount;
    return out.str() + " " + units[unit];
}
static uint64_t freeBytes(const fs::path &path) {
    std::error_code ec;
    auto info = fs::space(path, ec);
    return ec ? 0 : info.available;
}
static std::string jstr(JsonObject *object, const char *key, const char *fallback = "") {
    if (!object || !json_object_has_member(object, key)) return fallback;
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return fallback;
    const char *value = json_node_get_string(node);
    return value ? value : fallback;
}
static int64_t jint(JsonObject *object, const char *key, int64_t fallback = 0) {
    if (!object || !json_object_has_member(object, key)) return fallback;
    return json_object_get_int_member(object, key);
}
static bool jbool(JsonObject *object, const char *key, bool fallback = false) {
    if (!object || !json_object_has_member(object, key)) return fallback;
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN) return fallback;
    return json_node_get_boolean(node);
}
static JsonObject *jobj(JsonObject *object, const char *key) {
    return object && json_object_has_member(object, key) ? json_object_get_object_member(object, key) : nullptr;
}
static std::string jsonError(const std::string &data) {
    JsonParser *parser = json_parser_new(); std::string error = "Operation failed";
    if (json_parser_load_from_data(parser, data.c_str(), data.size(), nullptr)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) error = jstr(json_node_get_object(root), "error", error.c_str());
    }
    g_object_unref(parser); return error;
}

struct ExecResult { int status = -1; std::string output; };
static ExecResult execCapture(const std::vector<std::string> &arguments) {
    ExecResult result; if (arguments.empty()) return result;
    int pipefd[2]; if (pipe(pipefd) != 0) return result;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        std::vector<char *> argv; argv.reserve(arguments.size() + 1);
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr); execvp(argv[0], argv.data()); _exit(127);
    }
    close(pipefd[1]); char buffer[4096]; ssize_t count;
    while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, size_t(count));
    close(pipefd[0]); int status = 0; waitpid(pid, &status, 0);
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.output = trim(result.output); return result;
}

struct SearchResult {
    std::string title, path, console, thumbnail, mediaId;
};
struct Ticket {
    std::string title, console, region, mediaId, downloadUrl, downloadName;
    uint64_t bytes = 0;
};
struct GameFile {
    std::string title, system, path, extension;
    uint64_t bytes = 0;
};
struct TrashItem {
    std::string title, primary, manifest;
    uint64_t bytes = 0; int files = 0; bool available = false;
};

enum class Operation { None, Search, Detail, Download, ScanLibrary, ListTrash, TrashGame, RestoreGame, PurgeGame };
struct TaskState {
    std::atomic<bool> running{false}, done{false}, cancel{false};
    std::atomic<long long> current{0}, total{0};
    Operation operation = Operation::None;
    std::mutex mutex;
    std::string stage, error, message;
    std::vector<SearchResult> results;
    std::vector<GameFile> games;
    std::vector<TrashItem> trash;
    Ticket ticket;
};
static void taskStage(TaskState &task, const std::string &stage) {
    std::lock_guard<std::mutex> lock(task.mutex); task.stage = stage;
}
static void taskError(TaskState &task, const std::string &error) {
    std::lock_guard<std::mutex> lock(task.mutex); task.error = error;
}

struct MemoryBuffer { std::string data; size_t limit = 4 * 1024 * 1024; };
static size_t memoryWrite(char *data, size_t size, size_t count, void *opaque) {
    auto *buffer = static_cast<MemoryBuffer *>(opaque); size_t bytes = size * count;
    if (buffer->data.size() + bytes > buffer->limit) return 0;
    buffer->data.append(data, bytes); return bytes;
}
static size_t fileWrite(char *data, size_t size, size_t count, void *opaque) {
    return std::fwrite(data, size, count, static_cast<FILE *>(opaque));
}
struct ProgressContext { TaskState *task = nullptr; };
static int transferProgress(void *opaque, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto *context = static_cast<ProgressContext *>(opaque);
    if (!context || !context->task) return 0;
    context->task->total = static_cast<long long>(total);
    context->task->current = static_cast<long long>(now);
    return context->task->cancel ? 1 : 0;
}
struct HttpResponse { long status = 0; std::string body, contentType, error; };

class HttpClient {
public:
    static std::string encode(const std::string &value) {
        CURL *handle = curl_easy_init(); if (!handle) return {};
        char *encoded = curl_easy_escape(handle, value.c_str(), int(value.size()));
        std::string result = encoded ? encoded : ""; curl_free(encoded); curl_easy_cleanup(handle); return result;
    }
    static std::string decode(const std::string &value) {
        CURL *handle = curl_easy_init(); if (!handle) return value; int length = 0;
        char *decoded = curl_easy_unescape(handle, value.c_str(), int(value.size()), &length);
        std::string result = decoded ? std::string(decoded, size_t(length)) : value;
        curl_free(decoded); curl_easy_cleanup(handle); return result;
    }
    static bool approvedProviderUrl(const std::string &url) {
        std::smatch match; std::regex pattern(R"(^https://([^/:?#]+)(?::443)?(?:/|$))", std::regex::icase);
        if (!std::regex_search(url, match, pattern)) return false;
        std::string host = lower(match[1].str());
        return host == "romsgames.net" || endsWith(host, ".romsgames.net");
    }
    HttpResponse request(const std::string &url, const std::string &method = "GET", const std::string &form = {},
                         const std::vector<std::string> &headers = {}, const std::string &destination = {},
                         TaskState *task = nullptr, size_t memoryLimit = 4 * 1024 * 1024) {
        HttpResponse response;
        if (!approvedProviderUrl(url)) { response.error = "Refused non-provider URL"; return response; }
        CURL *handle = curl_easy_init(); if (!handle) { response.error = "Unable to initialize HTTPS"; return response; }
        char errorBuffer[CURL_ERROR_SIZE]{}; struct curl_slist *headerList = nullptr;
        for (const auto &header : headers) headerList = curl_slist_append(headerList, header.c_str());
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "RG35XXPro-ROM-Library/1.0");
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, destination.empty() ? 45L : 0L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
        if (headerList) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
        if (method == "POST") {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, form.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, long(form.size()));
        }
        MemoryBuffer memory; memory.limit = memoryLimit; FILE *file = nullptr;
        if (destination.empty()) {
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, memoryWrite);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, &memory);
        } else {
            file = std::fopen(destination.c_str(), "wb");
            if (!file) { response.error = "Unable to create download file"; curl_slist_free_all(headerList); curl_easy_cleanup(handle); return response; }
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, fileWrite);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, file);
        }
        ProgressContext progress{task};
        if (task) {
            curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, transferProgress);
            curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
        }
        CURLcode code = curl_easy_perform(handle);
        if (file) { std::fflush(file); fsync(fileno(file)); std::fclose(file); }
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
        char *type = nullptr; curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &type);
        if (type) response.contentType = type;
        if (code != CURLE_OK) response.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
        else response.body = std::move(memory.data);
        curl_slist_free_all(headerList); curl_easy_cleanup(handle); return response;
    }
};

static std::string platformFolder(const std::string &input) {
    std::string key = lower(trim(input));
    static const std::map<std::string, std::string> mapping = {
        {"gba", "GBA"}, {"gameboy advance", "GBA"}, {"game boy advance", "GBA"},
        {"nds", "NDS"}, {"nintendo ds", "NDS"}, {"psp", "PSP"}, {"playstation portable", "PSP"},
        {"gb", "GB"}, {"gameboy", "GB"}, {"game boy", "GB"}, {"gbc", "GBC"}, {"gameboy color", "GBC"},
        {"n64", "N64"}, {"nintendo 64", "N64"}, {"nes", "FC"}, {"nintendo", "FC"},
        {"snes", "SFC"}, {"super nintendo", "SFC"}, {"sfc", "SFC"}, {"genesis", "MD"}, {"mega drive", "MD"}, {"md", "MD"},
        {"dreamcast", "DREAMCAST"}, {"dc", "DREAMCAST"}, {"ps", "PS"}, {"ps1", "PS"}, {"psx", "PS"}, {"playstation", "PS"},
        {"saturn", "SATURN"}, {"game gear", "GG"}, {"gg", "GG"}, {"master system", "SMS"}, {"sms", "SMS"},
        {"sega 32x", "SEGA32X"}, {"32x", "SEGA32X"}, {"pc engine", "PCE"}, {"turbografx-16", "PCE"},
        {"neo geo pocket", "NGP"}, {"ngp", "NGP"}, {"atari 2600", "A2600"}, {"a2600", "A2600"},
        {"atari 5200", "A5200"}, {"atari 7800", "A7800"}, {"lynx", "LYNX"}, {"wonder swan", "WS"}, {"wonderswan", "WS"}
    };
    auto found = mapping.find(key); return found == mapping.end() ? "" : found->second;
}

class ProviderClient {
    HttpClient http_;
public:
    std::vector<SearchResult> search(const std::string &query, TaskState *task = nullptr) {
        if (trim(query).empty()) throw std::runtime_error("Enter a search term");
        if (task) taskStage(*task, "Searching provider");
        std::string url = std::string(PROVIDER) + "/search/hint/?q=" + HttpClient::encode(query);
        HttpResponse response = http_.request(url, "GET", {}, {"Accept: application/json"}, {}, task);
        if (!response.error.empty()) throw std::runtime_error("Search failed: " + response.error);
        JsonParser *parser = json_parser_new(); std::vector<SearchResult> results;
        if (!json_parser_load_from_data(parser, response.body.c_str(), response.body.size(), nullptr)) { g_object_unref(parser); throw std::runtime_error("Provider returned invalid search data"); }
        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_ARRAY(root)) { g_object_unref(parser); throw std::runtime_error("Provider search format changed"); }
        JsonArray *array = json_node_get_array(root); std::set<std::string> paths;
        for (guint index = 0; index < json_array_get_length(array); ++index) {
            JsonObject *item = json_array_get_object_element(array, index); if (!item || jstr(item, "type") != "rom") continue;
            SearchResult result; result.title = jstr(item, "name"); result.path = jstr(item, "url");
            result.console = jstr(item, "consoleShortTitle"); result.thumbnail = jstr(item, "thumbnailImageURL");
            if (result.title.empty() || result.path.empty() || result.path[0] != '/' || result.path.rfind("//", 0) == 0 || result.path.find("..") != std::string::npos) continue;
            if (paths.insert(result.path).second) results.push_back(std::move(result));
        }
        g_object_unref(parser); return results;
    }
    SearchResult detail(SearchResult result, TaskState *task = nullptr) {
        if (!result.mediaId.empty()) return result;
        if (task) taskStage(*task, "Loading game details");
        HttpResponse response = http_.request(std::string(PROVIDER) + result.path, "GET", {}, {"Accept: text/html"}, {}, task, 2 * 1024 * 1024);
        if (!response.error.empty()) throw std::runtime_error("Details failed: " + response.error);
        std::smatch match; std::regex media(R"(data-media-id\s*=\s*["']([0-9]+)["'])", std::regex::icase);
        if (!std::regex_search(response.body, match, media)) throw std::runtime_error("No downloadable media was listed for this game");
        result.mediaId = match[1].str(); return result;
    }
    Ticket ticket(const SearchResult &input, TaskState *task = nullptr) {
        SearchResult result = detail(input, task); if (task) taskStage(*task, "Requesting download ticket");
        std::string url = std::string(PROVIDER) + result.path + "?download";
        std::string form = "mediaId=" + HttpClient::encode(result.mediaId);
        HttpResponse response = http_.request(url, "POST", form,
            {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded; charset=UTF-8", "X-Requested-With: XMLHttpRequest", "Referer: " + std::string(PROVIDER) + result.path}, {}, task, 4 * 1024 * 1024);
        if (!response.error.empty()) throw std::runtime_error("Download ticket failed: " + response.error);
        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, response.body.c_str(), response.body.size(), nullptr)) { g_object_unref(parser); throw std::runtime_error("Provider returned an invalid download ticket"); }
        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(parser); throw std::runtime_error("Provider download format changed"); }
        JsonObject *object = json_node_get_object(root), *asset = jobj(object, "asset"), *media = jobj(object, "media");
        JsonObject *console = media ? jobj(media, "Console") : nullptr;
        Ticket ticket; ticket.title = jstr(asset, "title", result.title.c_str()); ticket.region = jstr(asset, "region", "Unknown");
        ticket.console = jstr(console, "shortTitle", result.console.c_str()); ticket.mediaId = result.mediaId;
        ticket.downloadUrl = jstr(object, "downloadUrl"); ticket.downloadName = jstr(object, "downloadName"); ticket.bytes = uint64_t(std::max<int64_t>(0, jint(media, "size")));
        g_object_unref(parser);
        if (!HttpClient::approvedProviderUrl(ticket.downloadUrl)) throw std::runtime_error("Provider returned an unapproved download host");
        if (ticket.downloadName.empty()) ticket.downloadName = HttpClient::encode(ticket.title + ".bin");
        return ticket;
    }
    std::string download(const Ticket &ticket, TaskState &task) {
        fs::create_directories(downloadRoot());
        fs::path partial = fs::path(downloadRoot()) / (ticket.mediaId + ".part");
        std::error_code ec; fs::remove(partial, ec);
        uint64_t available = freeBytes(downloadRoot());
        if (ticket.bytes && ticket.bytes + 64 * 1024 * 1024ULL > available) throw std::runtime_error("Not enough ROM storage for this download");
        task.current = 0; task.total = 0;
        for (int remaining = 5; remaining > 0; --remaining) {
            taskStage(task, "Provider preparing download (" + std::to_string(remaining) + "s)");
            for (int tick = 0; tick < 10; ++tick) {
                if (task.cancel) throw std::runtime_error("Download cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        std::string separator = ticket.downloadUrl.find('?') == std::string::npos ? "?" : "&";
        std::string url = ticket.downloadUrl + separator + "mediaId=" + HttpClient::encode(ticket.mediaId) + "&attach=" + HttpClient::encode(ticket.downloadName);
        task.current = 0; task.total = ticket.bytes; taskStage(task, "Downloading " + ticket.title);
        HttpResponse response = http_.request(url, "GET", {}, {"Accept: application/octet-stream,application/zip,*/*", "Referer: https://www.romsgames.net/"}, partial.string(), &task);
        if (!response.error.empty()) { fs::remove(partial, ec); if (task.cancel) throw std::runtime_error("Download cancelled"); throw std::runtime_error("Download failed: " + response.error); }
        uint64_t actual = fs::file_size(partial, ec); if (ec || actual == 0) { fs::remove(partial, ec); throw std::runtime_error("Provider download was empty"); }
        if (ticket.bytes && actual != ticket.bytes) { fs::remove(partial, ec); throw std::runtime_error("Downloaded size did not match the provider ticket"); }
        fs::path completed = fs::path(downloadRoot()) / (ticket.mediaId + ".payload"); fs::remove(completed, ec); fs::rename(partial, completed);
        return completed.string();
    }
};

static const std::map<std::string, std::set<std::string>> LIBRARY_EXTENSIONS = {
    {"A2600", {".a26", ".bin", ".zip"}}, {"A5200", {".a52", ".bin", ".zip"}}, {"A7800", {".a78", ".bin", ".zip"}},
    {"AMIGA", {".adf", ".adz", ".dms", ".hdf", ".lha", ".zip"}}, {"C64", {".d64", ".d81", ".g64", ".prg", ".t64", ".tap", ".zip"}},
    {"DREAMCAST", {".cdi", ".chd", ".cue", ".gdi"}}, {"FC", {".nes", ".unf", ".unif", ".zip"}}, {"FDS", {".fds", ".zip"}},
    {"GB", {".gb", ".zip"}}, {"GBA", {".gba", ".zip"}}, {"GBC", {".gbc", ".zip"}}, {"GG", {".gg", ".zip"}},
    {"LYNX", {".lnx", ".zip"}}, {"MD", {".bin", ".gen", ".md", ".smd", ".zip"}}, {"MDCD", {".chd", ".cue", ".iso"}},
    {"MSX", {".cas", ".dsk", ".mx1", ".mx2", ".rom", ".zip"}}, {"N64", {".n64", ".v64", ".z64", ".zip"}},
    {"NDS", {".nds", ".zip"}}, {"NEOCD", {".chd", ".cue"}}, {"NEOGEO", {".zip"}}, {"NGP", {".ngc", ".ngp", ".zip"}},
    {"PCE", {".pce", ".zip"}}, {"PCECD", {".chd", ".cue", ".iso"}}, {"PS", {".chd", ".cue", ".m3u", ".pbp"}},
    {"PSP", {".cso", ".iso", ".pbp"}}, {"SATURN", {".chd", ".cue", ".iso"}}, {"SEGA32X", {".32x", ".bin", ".zip"}},
    {"SFC", {".fig", ".sfc", ".smc", ".zip"}}, {"SMS", {".bin", ".sms", ".zip"}}, {"VB", {".vb", ".vboy", ".zip"}}, {"WS", {".ws", ".wsc", ".zip"}}
};

static std::vector<GameFile> scanLibrary(TaskState *task = nullptr) {
    std::vector<GameFile> games; fs::path root = romRoot(); std::error_code ec;
    for (const auto &[system, extensions] : LIBRARY_EXTENSIONS) {
        if (task && task->cancel) break;
        fs::path directory = root / system;
        if (!fs::is_directory(directory, ec)) continue;
        fs::recursive_directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, ec), end;
        while (iterator != end) {
            if (ec) { ec.clear(); iterator.increment(ec); continue; }
            const auto &entry = *iterator; std::string name = entry.path().filename().string();
            if (entry.is_directory(ec) && (name == "Imgs" || (!name.empty() && name[0] == '.'))) iterator.disable_recursion_pending();
            else if (entry.is_regular_file(ec) && (name.empty() || name[0] != '.')) {
                std::string extension = lower(entry.path().extension().string());
                if (extensions.count(extension)) {
                    GameFile game; game.path = entry.path().string(); game.title = entry.path().stem().string(); game.system = system; game.extension = extension;
                    game.bytes = entry.file_size(ec); if (ec) { game.bytes = 0; ec.clear(); } games.push_back(std::move(game));
                }
            }
            iterator.increment(ec);
        }
    }
    std::sort(games.begin(), games.end(), [](const GameFile &a, const GameFile &b) { return a.system == b.system ? lower(a.title) < lower(b.title) : a.system < b.system; });
    return games;
}

static std::vector<TrashItem> parseTrash(const std::string &data) {
    std::vector<TrashItem> items; JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data.c_str(), data.size(), nullptr)) { g_object_unref(parser); throw std::runtime_error("Trash index is invalid"); }
    JsonNode *root = json_parser_get_root(parser); if (!root || !JSON_NODE_HOLDS_OBJECT(root)) { g_object_unref(parser); throw std::runtime_error("Trash index format changed"); }
    JsonObject *object = json_node_get_object(root); if (!jbool(object, "ok", false)) { std::string error = jstr(object, "error", "Trash operation failed"); g_object_unref(parser); throw std::runtime_error(error); }
    JsonArray *array = json_object_get_array_member(object, "items");
    for (guint index = 0; array && index < json_array_get_length(array); ++index) {
        JsonObject *entry = json_array_get_object_element(array, index); TrashItem item;
        item.primary = jstr(entry, "primary"); item.manifest = jstr(entry, "manifest"); item.bytes = uint64_t(std::max<int64_t>(0, jint(entry, "bytes")));
        item.files = int(jint(entry, "files")); item.available = jbool(entry, "available", false);
        item.title = fs::path(item.primary).stem().string(); items.push_back(std::move(item));
    }
    g_object_unref(parser); return items;
}

struct Color { Uint8 r, g, b, a = 255; };
class Painter {
    SDL_Renderer *renderer_; FT_Library library_{}; FT_Face face_{};
    struct Glyph { SDL_Texture *texture = nullptr; int width = 0, height = 0, left = 0, top = 0, advance = 0; };
    std::map<std::pair<uint32_t, int>, Glyph> cache_;
    static uint32_t next(const std::string &text, size_t &offset) {
        unsigned char c = text[offset++]; if (c < 128) return c; int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
        uint32_t value = c & ((1u << (6 - extra)) - 1u); while (extra-- && offset < text.size()) value = (value << 6) | (text[offset++] & 63u); return value;
    }
    Glyph &glyph(uint32_t codepoint, int size) {
        auto key = std::make_pair(codepoint, size); auto existing = cache_.find(key); if (existing != cache_.end()) return existing->second;
        FT_Set_Pixel_Sizes(face_, 0, size); Glyph glyph;
        if (!FT_Load_Char(face_, codepoint, FT_LOAD_RENDER)) {
            auto &bitmap = face_->glyph->bitmap; glyph.width = bitmap.width; glyph.height = bitmap.rows; glyph.left = face_->glyph->bitmap_left; glyph.top = face_->glyph->bitmap_top; glyph.advance = face_->glyph->advance.x >> 6;
            if (glyph.width && glyph.height) {
                std::vector<Uint32> pixels(size_t(glyph.width * glyph.height));
                for (int y = 0; y < glyph.height; ++y) for (int x = 0; x < glyph.width; ++x) { Uint8 alpha = bitmap.buffer[y * bitmap.pitch + x]; pixels[size_t(y * glyph.width + x)] = (Uint32(alpha) << 24) | 0xFFFFFF; }
                SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixels.data(), glyph.width, glyph.height, 32, glyph.width * 4, 0xFF, 0xFF00, 0xFF0000, 0xFF000000);
                if (surface) { glyph.texture = SDL_CreateTextureFromSurface(renderer_, surface); SDL_FreeSurface(surface); }
                if (glyph.texture) SDL_SetTextureBlendMode(glyph.texture, SDL_BLENDMODE_BLEND);
            }
        }
        if (!glyph.advance) glyph.advance = std::max(1, size / 2);
        return cache_.emplace(key, glyph).first->second;
    }
public:
    explicit Painter(SDL_Renderer *renderer) : renderer_(renderer) {
        FT_Init_FreeType(&library_); FT_New_Face(library_, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &face_);
    }
    ~Painter() { for (auto &[_, glyph] : cache_) if (glyph.texture) SDL_DestroyTexture(glyph.texture); if (face_) FT_Done_Face(face_); if (library_) FT_Done_FreeType(library_); }
    void rect(int x, int y, int width, int height, Color color) { SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a); SDL_Rect area{x, y, width, height}; SDL_RenderFillRect(renderer_, &area); }
    void outline(int x, int y, int width, int height, Color color) { SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a); SDL_Rect area{x, y, width, height}; SDL_RenderDrawRect(renderer_, &area); }
    int width(const std::string &text, int size) { int result = 0; size_t offset = 0; while (offset < text.size()) result += glyph(next(text, offset), size).advance; return result; }
    void text(int x, int y, const std::string &text, Color color, int size = 20, int maxWidth = 0) {
        size_t offset = 0; int origin = x; while (offset < text.size()) { uint32_t codepoint = next(text, offset); if (codepoint == '\n') { x = origin; y += size + 5; continue; }
            Glyph &item = glyph(codepoint, size); if (maxWidth && x + item.advance > origin + maxWidth) break;
            if (item.texture) { SDL_SetTextureColorMod(item.texture, color.r, color.g, color.b); SDL_Rect target{x + item.left, y + size - item.top, item.width, item.height}; SDL_RenderCopy(renderer_, item.texture, nullptr, &target); }
            x += item.advance;
        }
    }
    std::vector<std::string> wrap(const std::string &text, int maxWidth, int size) {
        std::vector<std::string> lines; std::istringstream input(text); std::string source;
        while (std::getline(input, source)) { std::istringstream words(source); std::string word, line; while (words >> word) { std::string candidate = line.empty() ? word : line + " " + word; if (!line.empty() && width(candidate, size) > maxWidth) { lines.push_back(line); line = word; } else line = candidate; } lines.push_back(line); }
        if (lines.empty()) lines.push_back("");
        return lines;
    }
};

enum class Screen { Home, Keyboard, Results, Detail, Download, Library, LibraryDetail, ConfirmTrash, Trash, TrashDetail, ConfirmPurge, Settings, About, Calibration };

class App {
    SDL_Window *window_ = nullptr; SDL_Renderer *renderer_ = nullptr; SDL_GameController *controller_ = nullptr; SDL_Joystick *joystick_ = nullptr;
    int inputFd_ = -1, width_ = 640, height_ = 480, selected_ = 0, scroll_ = 0, frames_ = 0; bool quit_ = false, headless_ = false;
    std::unique_ptr<Painter> painter_; Screen screen_ = Screen::Home, returnScreen_ = Screen::Home;
    TaskState task_; std::thread worker_; std::string query_ = "pokemon", keyboardPurpose_, input_, error_, toast_, screenshot_;
    uint32_t toastUntil_ = 0; bool shift_ = false, symbols_ = false, keepDownloads_ = false, confirmYes_ = false, downloadComplete_ = false;
    std::vector<SearchResult> results_; SearchResult activeResult_; Ticket activeTicket_;
    std::vector<GameFile> games_; GameFile activeGame_; std::vector<std::string> libraryFilters_{"All"}; int filterIndex_ = 0;
    std::vector<TrashItem> trash_; TrashItem activeTrash_;
    std::map<std::string, std::string> bindings_; std::map<unsigned, int> axisDirection_; int calibrationStep_ = 0; bool calibrationWait_ = false; std::string waitToken_; uint32_t calibrationReady_ = 0;
    const std::vector<std::string> calibrationActions_ = {"A", "B", "X", "Y", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "L1", "L2", "R1", "R2", "Select", "Start", "Menu", "Left Stick Up", "Left Stick Down", "Left Stick Left", "Left Stick Right", "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right"};
    const Color background_{9, 16, 30}, surface_{18, 29, 48}, raised_{27, 42, 65}, accent_{45, 212, 191}, accent2_{115, 100, 255}, text_{241, 246, 255}, muted_{151, 166, 189}, danger_{255, 101, 113}, warning_{247, 188, 72};

    void setToast(const std::string &message) { toast_ = message; toastUntil_ = SDL_GetTicks() + 3200; }
    std::string taskText(const std::string &field) {
        std::lock_guard<std::mutex> lock(task_.mutex); if (field == "stage") return task_.stage; if (field == "error") return task_.error; return task_.message;
    }
    template <class Work> void startTask(Operation operation, Work work) {
        if (task_.running) return;
        if (worker_.joinable()) worker_.join();
        task_.operation = operation; task_.running = true; task_.done = false; task_.cancel = false; task_.current = 0; task_.total = 0;
        { std::lock_guard<std::mutex> lock(task_.mutex); task_.stage.clear(); task_.error.clear(); task_.message.clear(); task_.results.clear(); task_.games.clear(); task_.trash.clear(); task_.ticket = {}; }
        worker_ = std::thread([this, work]() mutable { try { work(); } catch (const std::exception &exception) { std::fprintf(stderr, "ROM Library operation error: %s\n", exception.what()); std::fflush(stderr); taskError(task_, exception.what()); } task_.running = false; task_.done = true; });
    }
    void startSearch() {
        screen_ = Screen::Results; selected_ = scroll_ = 0; downloadComplete_ = false;
        std::string query = query_;
        startTask(Operation::Search, [this, query] { ProviderClient provider; auto results = provider.search(query, &task_); std::lock_guard<std::mutex> lock(task_.mutex); task_.results = std::move(results); });
    }
    void startDetail(SearchResult result) {
        activeResult_ = std::move(result); screen_ = Screen::Detail; selected_ = scroll_ = 0;
        startTask(Operation::Detail, [this] { ProviderClient provider; SearchResult detailed = provider.detail(activeResult_, &task_); std::lock_guard<std::mutex> lock(task_.mutex); task_.results = {detailed}; });
    }
    void startDownload() {
        screen_ = Screen::Download; downloadComplete_ = false; SearchResult result = activeResult_; bool keep = keepDownloads_;
        startTask(Operation::Download, [this, result, keep] {
            ProviderClient provider; Ticket ticket = provider.ticket(result, &task_); std::string system = platformFolder(ticket.console.empty() ? result.console : ticket.console);
            if (system.empty()) throw std::runtime_error("This console is not mapped to a stock firmware ROM folder");
            { std::lock_guard<std::mutex> lock(task_.mutex); task_.ticket = ticket; }
            for (int second = 5; second > 0; --second) { if (task_.cancel) throw std::runtime_error("Download cancelled"); taskStage(task_, "Provider countdown: " + std::to_string(second) + " seconds"); std::this_thread::sleep_for(std::chrono::seconds(1)); }
            std::string payload = provider.download(ticket, task_); taskStage(task_, "Inspecting and installing safely");
            std::string decodedName = HttpClient::decode(ticket.downloadName);
            ExecResult installed = execCapture({"python3", helperPath(), "install", "--archive", payload, "--download-name", decodedName, "--system", system, "--rom-root", romRoot()});
            if (installed.status != 0) { std::error_code ec; if (!keep) fs::remove(payload, ec); throw std::runtime_error(jsonError(installed.output)); }
            if (!keep) { std::error_code ec; fs::remove(payload, ec); }
            fs::create_directories(appHome()); std::ofstream history(appHome() + "/history.log", std::ios::app);
            history << std::time(nullptr) << '\t' << system << '\t' << ticket.title << '\t' << installed.output << '\n';
            std::lock_guard<std::mutex> lock(task_.mutex); task_.message = ticket.title + " installed in " + system; task_.ticket = ticket;
        });
    }
    void startScan() {
        screen_ = Screen::Library; selected_ = scroll_ = 0; taskStage(task_, "Scanning installed games");
        startTask(Operation::ScanLibrary, [this] { taskStage(task_, "Scanning installed games"); auto games = scanLibrary(&task_); std::lock_guard<std::mutex> lock(task_.mutex); task_.games = std::move(games); });
    }
    void startTrashList() {
        screen_ = Screen::Trash; selected_ = scroll_ = 0;
        startTask(Operation::ListTrash, [this] { taskStage(task_, "Reading recoverable trash"); ExecResult result = execCapture({"python3", helperPath(), "list-trash", "--trash-root", trashRoot()}); if (result.status != 0) throw std::runtime_error(jsonError(result.output)); auto items = parseTrash(result.output); std::lock_guard<std::mutex> lock(task_.mutex); task_.trash = std::move(items); });
    }
    void startTrashGame() {
        std::string path = activeGame_.path;
        startTask(Operation::TrashGame, [this, path] { taskStage(task_, "Moving game to recoverable trash"); ExecResult result = execCapture({"python3", helperPath(), "trash", "--source", path, "--rom-root", romRoot(), "--trash-root", trashRoot()}); if (result.status != 0) throw std::runtime_error(jsonError(result.output)); std::lock_guard<std::mutex> lock(task_.mutex); task_.message = "Game moved to recoverable trash"; });
    }
    void startRestore() {
        std::string manifest = activeTrash_.manifest;
        startTask(Operation::RestoreGame, [this, manifest] { taskStage(task_, "Restoring game"); ExecResult result = execCapture({"python3", helperPath(), "restore", "--manifest", manifest, "--rom-root", romRoot(), "--trash-root", trashRoot()}); if (result.status != 0) throw std::runtime_error(jsonError(result.output)); std::lock_guard<std::mutex> lock(task_.mutex); task_.message = "Game restored"; });
    }
    void startPurge() {
        std::string manifest = activeTrash_.manifest;
        startTask(Operation::PurgeGame, [this, manifest] { taskStage(task_, "Permanently deleting selected trash item"); ExecResult result = execCapture({"python3", helperPath(), "purge", "--manifest", manifest, "--trash-root", trashRoot()}); if (result.status != 0) throw std::runtime_error(jsonError(result.output)); std::lock_guard<std::mutex> lock(task_.mutex); task_.message = "Trash item permanently deleted"; });
    }
    void pollTask() {
        if (!task_.done.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
        std::string failure = taskText("error"); if (!failure.empty()) { error_ = failure; return; }
        Operation operation = task_.operation;
        bool rescan = false, relistTrash = false;
        std::string completionMessage;
        {
            std::lock_guard<std::mutex> lock(task_.mutex);
            if (operation == Operation::Search) { results_ = task_.results; if (results_.empty()) setToast("No games found"); }
            else if (operation == Operation::Detail) { if (!task_.results.empty()) activeResult_ = task_.results.front(); }
            else if (operation == Operation::Download) { activeTicket_ = task_.ticket; downloadComplete_ = true; setToast(task_.message); }
            else if (operation == Operation::ScanLibrary) {
                games_ = task_.games; libraryFilters_ = {"All"}; for (const auto &game : games_) if (std::find(libraryFilters_.begin(), libraryFilters_.end(), game.system) == libraryFilters_.end()) libraryFilters_.push_back(game.system);
                filterIndex_ = 0; setToast(std::to_string(games_.size()) + " installed games indexed");
            } else if (operation == Operation::ListTrash) { trash_ = task_.trash; }
            else if (operation == Operation::TrashGame) { completionMessage = task_.message; screen_ = Screen::Library; rescan = true; }
            else if (operation == Operation::RestoreGame || operation == Operation::PurgeGame) { completionMessage = task_.message; screen_ = Screen::Trash; relistTrash = true; }
        }
        if (rescan) { setToast(completionMessage); startScan(); }
        else if (relistTrash) { setToast(completionMessage); startTrashList(); }
    }

    void header(const std::string &title, const std::string &subtitle = {}) {
        painter_->rect(0, 0, width_, 58, surface_); painter_->rect(0, 56, width_, 2, accent_);
        painter_->text(20, 11, title, text_, 24, width_ - 40); if (!subtitle.empty()) painter_->text(340, 19, subtitle, muted_, 13, width_ - 360);
    }
    void footer(const std::string &text) { painter_->rect(0, height_ - 36, width_, 36, surface_); painter_->text(16, height_ - 27, text, muted_, 14, width_ - 32); }
    void badge(int x, int y, const std::string &label, Color color) {
        int width = painter_->width(label, 13) + 18; painter_->rect(x, y, width, 24, color); painter_->text(x + 9, y + 5, label, background_, 13); }
    void row(int y, const std::string &title, const std::string &subtitle, bool active, const std::string &pill = {}) {
        Color fill = active ? raised_ : surface_; painter_->rect(14, y, width_ - 28, 54, fill); if (active) painter_->rect(14, y, 5, 54, accent_);
        painter_->text(28, y + 8, title, active ? text_ : Color{218, 228, 242}, 18, pill.empty() ? width_ - 58 : width_ - 150);
        painter_->text(28, y + 32, subtitle, muted_, 13, width_ - 58); if (!pill.empty()) badge(width_ - 100, y + 15, pill, active ? accent_ : Color{81, 102, 128});
    }
    void taskPanel() {
        std::string stage = taskText("stage"); painter_->rect(28, 128, width_ - 56, 170, surface_); painter_->outline(28, 128, width_ - 56, 170, raised_);
        painter_->text(48, 152, stage.empty() ? "Working..." : stage, text_, 20, width_ - 96);
        long long current = task_.current, total = task_.total; int barWidth = width_ - 96; painter_->rect(48, 211, barWidth, 18, raised_);
        int filled = total > 0 ? int(double(current) / double(total) * barWidth) : int((SDL_GetTicks() / 18) % std::max(1, barWidth));
        if (total > 0) painter_->rect(48, 211, std::max(0, std::min(barWidth, filled)), 18, accent_); else painter_->rect(48 + std::max(0, filled - 55), 211, 55, 18, accent2_);
        std::string amount = total > 0 ? formatBytes(uint64_t(std::max<long long>(0, current))) + " / " + formatBytes(uint64_t(total)) : "Secure provider request";
        painter_->text(48, 242, amount, muted_, 15, barWidth);
    }
    std::vector<int> filteredGames() const {
        std::vector<int> indices; std::string filter = libraryFilters_.empty() ? "All" : libraryFilters_[size_t(filterIndex_)];
        for (int i = 0; i < int(games_.size()); ++i) if (filter == "All" || games_[size_t(i)].system == filter) indices.push_back(i);
        return indices;
    }
    void home() {
        header("ROM Library", "RG35XX Pro  •  v" + std::string(VERSION));
        painter_->text(20, 72, "Your games, one comfortable place.", text_, 22, width_ - 40);
        painter_->text(20, 101, "Search the approved provider or manage what is already installed.", muted_, 14, width_ - 40);
        std::vector<std::pair<std::string, std::string>> items = {{"Find games", "Search, review, download and install"}, {"Installed library", "Browse games by system and remove safely"}, {"Recoverable trash", "Restore games or permanently clear selected items"}, {"Settings", "Downloads, storage and controller mapping"}, {"About & controls", "Provider policy, paths and button guide"}};
        int y = 132; for (int i = 0; i < int(items.size()); ++i, y += 58) row(y, items[size_t(i)].first, items[size_t(i)].second, selected_ == i);
        footer("D-pad Navigate   A Open   Menu Exit");
    }
    std::vector<std::string> keys() const {
        return symbols_ ? std::vector<std::string>{"1","2","3","4","5","6","7","8","9","0","!","@","#","$","%","^","&","*","(",")","-","_","=","+","[","]","{","}","/","\\",":",";","'","\"",",",".","?","<",">","`","~","SPACE","DEL","OK"}
                        : std::vector<std::string>{"q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m","0","1","2","3","4","5","6","7","8","9","-","_","'",".","SPACE","DEL","OK"};
    }
    void keyboard() {
        header("Search provider", shift_ ? "SHIFT" : "Controller keyboard"); painter_->rect(14, 68, width_ - 28, 62, surface_);
        painter_->text(27, 87, input_.empty() ? "Enter a title..." : input_, input_.empty() ? muted_ : text_, 19, width_ - 54);
        auto buttons = keys(); int columns = 10, cell = (width_ - 28) / columns, startY = 145;
        for (int i = 0; i < int(buttons.size()); ++i) { int x = 14 + (i % columns) * cell, y = startY + (i / columns) * 47; bool active = selected_ == i;
            painter_->rect(x + 2, y + 2, cell - 4, 40, active ? accent_ : raised_); std::string label = buttons[size_t(i)]; if (shift_ && label.size() == 1 && std::isalpha(static_cast<unsigned char>(label[0]))) label[0] = char(std::toupper(label[0]));
            int textWidth = painter_->width(label, 14); painter_->text(x + (cell - textWidth) / 2, y + 13, label, active ? background_ : text_, 14);
        }
        footer("D-pad Move   A Key   X Delete   Y Shift   L Symbols   Start Search");
    }
    void results() {
        header("Search results", query_); if (task_.running) { taskPanel(); footer("B Cancel"); return; }
        int visible = 6; if (selected_ < scroll_) scroll_ = selected_; if (selected_ >= scroll_ + visible) scroll_ = selected_ - visible + 1;
        int y = 68; for (int i = scroll_; i < int(results_.size()) && i < scroll_ + visible; ++i, y += 59) { const auto &item = results_[size_t(i)]; std::string folder = platformFolder(item.console); row(y, item.title, folder.empty() ? "Console not mapped" : "Installs to /Roms/" + folder, selected_ == i, item.console); }
        if (results_.empty()) { painter_->text(28, 105, "No matching games were returned.", text_, 20); painter_->text(28, 140, "Press X to try a broader title.", muted_, 16); }
        footer("A Details   X New search   Y Refresh   B Home");
    }
    void detail() {
        header("Game details", activeResult_.console); if (task_.running) { taskPanel(); footer("B Cancel"); return; }
        painter_->rect(22, 78, width_ - 44, 272, surface_); badge(42, 98, activeResult_.console.empty() ? "Unknown" : activeResult_.console, accent2_);
        auto titleLines = painter_->wrap(activeResult_.title, width_ - 84, 25); int y = 141; for (const auto &line : titleLines) { painter_->text(42, y, line, text_, 25, width_ - 84); y += 31; if (y > 205) break; }
        std::string folder = platformFolder(activeResult_.console); painter_->text(42, 231, "Provider", muted_, 14); painter_->text(180, 231, "romsgames.net", text_, 16);
        painter_->text(42, 266, "Destination", muted_, 14); painter_->text(180, 266, folder.empty() ? "Unsupported console" : "/mnt/mmc/Roms/" + folder, folder.empty() ? danger_ : text_, 15, width_ - 215);
        painter_->text(42, 301, "Media", muted_, 14); painter_->text(180, 301, activeResult_.mediaId.empty() ? "Unavailable" : "Ready to request securely", activeResult_.mediaId.empty() ? danger_ : accent_, 15);
        footer(folder.empty() ? "B Results" : "A Download & install   B Results");
    }
    void download() {
        header(downloadComplete_ ? "Installation complete" : "Download & install", activeResult_.console);
        if (task_.running) { taskPanel(); footer("B Cancel download"); return; }
        painter_->rect(26, 92, width_ - 52, 240, surface_); painter_->text(48, 119, downloadComplete_ ? "Ready to play" : "Download stopped", downloadComplete_ ? accent_ : danger_, 25);
        auto lines = painter_->wrap(downloadComplete_ ? taskText("message") : (error_.empty() ? "No installation result." : error_), width_ - 96, 18); int y = 171; for (const auto &line : lines) { painter_->text(48, y, line, text_, 18); y += 24; }
        if (downloadComplete_) { painter_->text(48, 266, "Refresh or reopen the firmware game list to see it.", muted_, 15, width_ - 96); painter_->text(48, 294, keepDownloads_ ? "Downloaded archive was retained." : "Downloaded archive was removed after installation.", muted_, 14, width_ - 96); }
        footer(downloadComplete_ ? "A Installed library   B Results" : "B Results");
    }
    void library() {
        std::string filter = libraryFilters_.empty() ? "All" : libraryFilters_[size_t(filterIndex_)]; header("Installed library", filter + "  •  " + std::to_string(games_.size()) + " total");
        if (task_.running) { taskPanel(); footer("B Cancel"); return; } auto indices = filteredGames(); int visible = 6;
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + visible) scroll_ = selected_ - visible + 1;
        int y = 68; for (int position = scroll_; position < int(indices.size()) && position < scroll_ + visible; ++position, y += 59) { const GameFile &game = games_[size_t(indices[size_t(position)])]; row(y, game.title, formatBytes(game.bytes) + "  •  " + fs::path(game.path).filename().string(), selected_ == position, game.system); }
        if (indices.empty()) { painter_->text(28, 105, "No games in this filter.", text_, 20); painter_->text(28, 140, "Use L/R to change systems or X to rescan.", muted_, 16); }
        footer("A Manage   L/R System   X Rescan   B Home");
    }
    void libraryDetail() {
        header("Manage installed game", activeGame_.system); painter_->rect(22, 78, width_ - 44, 286, surface_); badge(42, 98, activeGame_.system, accent2_);
        auto lines = painter_->wrap(activeGame_.title, width_ - 84, 24); int y = 140; for (const auto &line : lines) { painter_->text(42, y, line, text_, 24); y += 30; if (y > 200) break; }
        painter_->text(42, 225, "Size", muted_, 14); painter_->text(170, 225, formatBytes(activeGame_.bytes), text_, 16);
        painter_->text(42, 258, "File", muted_, 14); painter_->text(170, 258, fs::path(activeGame_.path).filename().string(), text_, 15, width_ - 210);
        painter_->text(42, 302, "Removal is recoverable until you purge it from Trash.", muted_, 14, width_ - 84);
        footer("A Move to trash   B Library");
    }
    void confirm(const std::string &title, const std::string &message, bool permanent) {
        header(title); painter_->rect(40, 112, width_ - 80, 205, surface_); painter_->text(64, 139, permanent ? "Permanent action" : "Recoverable action", permanent ? danger_ : warning_, 22);
        auto lines = painter_->wrap(message, width_ - 128, 16); int y = 183; for (const auto &line : lines) { painter_->text(64, y, line, text_, 16); y += 22; }
        painter_->rect(105, 264, 180, 42, !confirmYes_ ? accent_ : raised_); painter_->text(167, 277, "Cancel", !confirmYes_ ? background_ : text_, 16);
        painter_->rect(355, 264, 180, 42, confirmYes_ ? (permanent ? danger_ : warning_) : raised_); painter_->text(419, 277, permanent ? "Delete" : "Remove", confirmYes_ ? background_ : text_, 16);
        footer("Left/Right Choose   A Confirm   B Cancel");
    }
    void trash() {
        header("Recoverable trash", std::to_string(trash_.size()) + " items"); if (task_.running) { taskPanel(); footer("B Cancel"); return; }
        int visible = 6; if (selected_ < scroll_) scroll_ = selected_; if (selected_ >= scroll_ + visible) scroll_ = selected_ - visible + 1;
        int y = 68; for (int i = scroll_; i < int(trash_.size()) && i < scroll_ + visible; ++i, y += 59) { const auto &item = trash_[size_t(i)]; row(y, item.title, formatBytes(item.bytes) + "  •  " + std::to_string(item.files) + " file(s)", selected_ == i, item.available ? "Ready" : "Issue"); }
        if (trash_.empty()) { painter_->text(28, 105, "Trash is empty.", text_, 20); painter_->text(28, 140, "Removed games will appear here for recovery.", muted_, 16); }
        footer("A Manage   X Refresh   B Home");
    }
    void trashDetail() {
        header("Trash item", activeTrash_.available ? "Recoverable" : "Missing files"); painter_->rect(22, 86, width_ - 44, 260, surface_);
        painter_->text(42, 112, activeTrash_.title, text_, 24, width_ - 84); painter_->text(42, 166, "Original", muted_, 14); painter_->text(160, 166, activeTrash_.primary, text_, 15, width_ - 198);
        painter_->text(42, 211, "Size", muted_, 14); painter_->text(160, 211, formatBytes(activeTrash_.bytes), text_, 16);
        painter_->text(42, 251, "Restore returns every tracked file to its original folder.", muted_, 14, width_ - 84);
        painter_->text(42, 282, "Permanent delete cannot be undone.", danger_, 14, width_ - 84);
        footer(activeTrash_.available ? "A Restore   X Permanently delete   B Trash" : "X Remove broken trash record   B Trash");
    }
    void settings() {
        header("Settings", "Local, credential-free configuration"); std::vector<std::pair<std::string, std::string>> items = {
            {"Keep downloaded archives", keepDownloads_ ? "On • uses additional ROM storage" : "Off • remove after successful install"},
            {"ROM storage", romRoot() + " • " + formatBytes(freeBytes(romRoot())) + " free"},
            {"Provider", "romsgames.net • HTTPS • no cookies"}, {"Controller calibration", "Guided raw-input mapping"}, {"Refresh installed library", "Rebuild the local game index"}};
        int y = 76; for (int i = 0; i < int(items.size()); ++i, y += 61) row(y, items[size_t(i)].first, items[size_t(i)].second, selected_ == i);
        footer("A Change/Run   B Home");
    }
    void about() {
        header("About ROM Library", "v" + std::string(VERSION)); painter_->text(24, 79, "Controller-first library management for RG35XX Pro stock firmware.", text_, 18, width_ - 48);
        std::vector<std::string> lines = {"Search: provider JSON endpoint only", "Downloads: HTTPS from provider-approved hosts", "Install: archive traversal, symlink, size and extension checks", "Removal: recoverable trash with explicit permanent-delete confirmation", "No cookies, account credentials, ads, analytics or background service", "A Select   B Back   X Context action   Y Refresh   L/R Filter"};
        int y = 132; for (const auto &line : lines) { painter_->rect(24, y - 5, 7, 7, accent_); painter_->text(43, y - 11, line, muted_, 15, width_ - 67); y += 46; }
        footer("B Home");
    }
    void calibration() {
        header("Controller calibration", "Step " + std::to_string(calibrationStep_ + 1) + " of " + std::to_string(calibrationActions_.size())); painter_->text(30, 105, "Press and release:", muted_, 20);
        painter_->rect(24, 145, width_ - 48, 100, raised_); std::string request = calibrationStep_ < int(calibrationActions_.size()) ? calibrationActions_[size_t(calibrationStep_)] : "Complete";
        int textWidth = painter_->width(request, 30); painter_->text(std::max(30, (width_ - textWidth) / 2), 178, request, accent_, 30, width_ - 60);
        painter_->text(30, 285, calibrationWait_ ? "Release it before the next prompt..." : "Press once, then release.", text_, 18, width_ - 60);
        painter_->text(30, 330, "Move sticks fully for direction prompts.", muted_, 15, width_ - 60); footer("Mapping saves automatically when complete");
    }
    void draw() {
        SDL_SetRenderDrawColor(renderer_, background_.r, background_.g, background_.b, 255); SDL_RenderClear(renderer_);
        switch (screen_) { case Screen::Home: home(); break; case Screen::Keyboard: keyboard(); break; case Screen::Results: results(); break; case Screen::Detail: detail(); break; case Screen::Download: download(); break; case Screen::Library: library(); break; case Screen::LibraryDetail: libraryDetail(); break; case Screen::ConfirmTrash: confirm("Remove installed game?", "Move this game and its referenced disc files into recoverable trash.", false); break; case Screen::Trash: trash(); break; case Screen::TrashDetail: trashDetail(); break; case Screen::ConfirmPurge: confirm("Permanently delete?", "Delete this selected trash item and all files tracked by it. This cannot be undone.", true); break; case Screen::Settings: settings(); break; case Screen::About: about(); break; case Screen::Calibration: calibration(); break; }
        if (!error_.empty()) { painter_->rect(24, height_ / 2 - 58, width_ - 48, 116, Color{72, 25, 35}); painter_->outline(24, height_ / 2 - 58, width_ - 48, 116, danger_); painter_->text(43, height_ / 2 - 43, "Something needs attention", danger_, 19); auto lines = painter_->wrap(error_, width_ - 86, 14); int y = height_ / 2 - 10; for (const auto &line : lines) { painter_->text(43, y, line, text_, 14, width_ - 86); y += 19; if (y > height_ / 2 + 37) break; } }
        if (!toast_.empty() && !SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) { int boxWidth = std::min(width_ - 40, painter_->width(toast_, 14) + 32); painter_->rect((width_ - boxWidth) / 2, height_ - 78, boxWidth, 31, accent_); painter_->text((width_ - boxWidth) / 2 + 16, height_ - 70, toast_, background_, 14, boxWidth - 32); }
        SDL_RenderPresent(renderer_);
        if (!screenshot_.empty() && frames_++ > 60) { SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width_, height_, 32, SDL_PIXELFORMAT_ARGB8888); if (surface && SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) == 0) SDL_SaveBMP(surface, screenshot_.c_str()); if (surface) SDL_FreeSurface(surface); screenshot_.clear(); }
    }

    void clampSelection(int count) { if (count <= 0) { selected_ = scroll_ = 0; return; } selected_ = std::max(0, std::min(selected_, count - 1)); }
    void move(int dx, int dy) {
        error_.clear(); if (screen_ == Screen::Keyboard) { int columns = 10, count = int(keys().size()), row = selected_ / columns, column = selected_ % columns; row += dy; column += dx; if (column < 0) column = columns - 1; if (column >= columns) column = 0; int rows = (count + columns - 1) / columns; if (row < 0) row = rows - 1; if (row >= rows) row = 0; selected_ = std::min(row * columns + column, count - 1); return; }
        if (screen_ == Screen::ConfirmTrash || screen_ == Screen::ConfirmPurge) { if (dx || dy) confirmYes_ = !confirmYes_; return; }
        if (screen_ == Screen::Library && dx) { if (!libraryFilters_.empty()) { filterIndex_ = (filterIndex_ + (dx > 0 ? 1 : int(libraryFilters_.size()) - 1)) % int(libraryFilters_.size()); selected_ = scroll_ = 0; } return; }
        selected_ += dy ? dy : dx; int count = 1;
        if (screen_ == Screen::Home || screen_ == Screen::Settings) count = 5; else if (screen_ == Screen::Results) count = int(results_.size()); else if (screen_ == Screen::Library) count = int(filteredGames().size()); else if (screen_ == Screen::Trash) count = int(trash_.size());
        clampSelection(count);
    }
    void chooseKey() {
        auto buttons = keys(); if (selected_ < 0 || selected_ >= int(buttons.size())) return; std::string value = buttons[size_t(selected_)];
        if (value == "SPACE") input_ += ' '; else if (value == "DEL") { if (!input_.empty()) input_.pop_back(); } else if (value == "OK") submitKeyboard(); else { if (shift_ && value.size() == 1 && std::isalpha(static_cast<unsigned char>(value[0]))) value[0] = char(std::toupper(value[0])); input_ += value; shift_ = false; }
    }
    void submitKeyboard() { query_ = trim(input_); if (query_.empty()) { error_ = "Enter at least one letter or number"; return; } startSearch(); }
    void activate() {
        error_.clear(); if (task_.running) return;
        if (screen_ == Screen::Home) { if (selected_ == 0) { input_ = query_; keyboardPurpose_ = "search"; screen_ = Screen::Keyboard; selected_ = 0; } else if (selected_ == 1) startScan(); else if (selected_ == 2) startTrashList(); else if (selected_ == 3) { screen_ = Screen::Settings; selected_ = 0; } else { screen_ = Screen::About; selected_ = 0; } }
        else if (screen_ == Screen::Keyboard) chooseKey();
        else if (screen_ == Screen::Results && !results_.empty()) startDetail(results_[size_t(selected_)]);
        else if (screen_ == Screen::Detail && !activeResult_.mediaId.empty() && !platformFolder(activeResult_.console).empty()) startDownload();
        else if (screen_ == Screen::Download && downloadComplete_) startScan();
        else if (screen_ == Screen::Library) { auto indices = filteredGames(); if (!indices.empty()) { activeGame_ = games_[size_t(indices[size_t(selected_)])]; screen_ = Screen::LibraryDetail; selected_ = 0; } }
        else if (screen_ == Screen::LibraryDetail) { confirmYes_ = false; screen_ = Screen::ConfirmTrash; }
        else if (screen_ == Screen::ConfirmTrash) { if (confirmYes_) startTrashGame(); else screen_ = Screen::LibraryDetail; }
        else if (screen_ == Screen::Trash && !trash_.empty()) { activeTrash_ = trash_[size_t(selected_)]; screen_ = Screen::TrashDetail; selected_ = 0; }
        else if (screen_ == Screen::TrashDetail && activeTrash_.available) startRestore();
        else if (screen_ == Screen::ConfirmPurge) { if (confirmYes_) startPurge(); else screen_ = Screen::TrashDetail; }
        else if (screen_ == Screen::Settings) { if (selected_ == 0) { keepDownloads_ = !keepDownloads_; saveSettings(); setToast("Setting saved"); } else if (selected_ == 3) { calibrationStep_ = 0; calibrationWait_ = false; waitToken_.clear(); screen_ = Screen::Calibration; } else if (selected_ == 4) startScan(); }
    }
    void back() {
        error_.clear(); if (task_.running) { task_.cancel = true; setToast("Cancelling..."); return; }
        if (screen_ == Screen::Home) quit_ = true; else if (screen_ == Screen::Keyboard) screen_ = Screen::Home; else if (screen_ == Screen::Results || screen_ == Screen::Library || screen_ == Screen::Trash || screen_ == Screen::Settings || screen_ == Screen::About) screen_ = Screen::Home;
        else if (screen_ == Screen::Detail || screen_ == Screen::Download) screen_ = Screen::Results; else if (screen_ == Screen::LibraryDetail || screen_ == Screen::ConfirmTrash) screen_ = Screen::Library; else if (screen_ == Screen::TrashDetail || screen_ == Screen::ConfirmPurge) screen_ = Screen::Trash;
        selected_ = scroll_ = 0;
    }
    void contextX() {
        if (screen_ == Screen::Keyboard) { if (!input_.empty()) input_.pop_back(); }
        else if (screen_ == Screen::Results) { input_ = query_; screen_ = Screen::Keyboard; selected_ = 0; }
        else if (screen_ == Screen::Library) startScan(); else if (screen_ == Screen::Trash) startTrashList();
        else if (screen_ == Screen::TrashDetail) { confirmYes_ = false; screen_ = Screen::ConfirmPurge; }
    }
    void contextY() { if (screen_ == Screen::Keyboard) shift_ = !shift_; else if (screen_ == Screen::Results) startSearch(); }
    void key(SDL_Keycode key) {
        if (key == SDLK_UP) move(0, -1); else if (key == SDLK_DOWN) move(0, 1); else if (key == SDLK_LEFT) move(-1, 0); else if (key == SDLK_RIGHT) move(1, 0); else if (key == SDLK_RETURN) activate(); else if (key == SDLK_ESCAPE) back(); else if (key == SDLK_BACKSPACE) contextX(); else if (key == SDLK_SPACE) submitKeyboard(); else if (key == SDLK_TAB) contextY();
    }
    void button(Uint8 button) {
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) move(0, -1); else if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) move(0, 1); else if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) move(-1, 0); else if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) move(1, 0);
        else if (button == SDL_CONTROLLER_BUTTON_A) activate(); else if (button == SDL_CONTROLLER_BUTTON_B) back(); else if (button == SDL_CONTROLLER_BUTTON_X) contextX(); else if (button == SDL_CONTROLLER_BUTTON_Y) contextY();
        else if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) { if (screen_ == Screen::Keyboard) { symbols_ = !symbols_; selected_ = 0; } else move(-1, 0); }
        else if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) move(1, 0); else if (button == SDL_CONTROLLER_BUTTON_START && screen_ == Screen::Keyboard) submitKeyboard(); else if (button == SDL_CONTROLLER_BUTTON_BACK) quit_ = true;
    }
    void rawButton(Uint8 button) { switch (button) { case 0: action("A"); break; case 1: action("B"); break; case 2: action("Y"); break; case 3: action("X"); break; case 4: action("L1"); break; case 5: action("R1"); break; case 6: action("Select"); break; case 7: action("Start"); break; case 8: action("Menu"); break; case 10: action("L2"); break; case 11: action("R2"); break; default: break; } }
    void defaults() {
        bindings_ = {{"k:304","A"},{"k:305","B"},{"k:306","Y"},{"k:307","X"},{"k:308","L1"},{"k:309","R1"},{"k:310","Select"},{"k:311","Start"},{"k:312","Menu"},{"k:314","L2"},{"k:315","R2"},{"a:17:-1","D-pad Up"},{"a:17:1","D-pad Down"},{"a:16:-1","D-pad Left"},{"a:16:1","D-pad Right"},{"a:2:-1","Left Stick Left"},{"a:2:1","Left Stick Right"},{"a:3:-1","Left Stick Up"},{"a:3:1","Left Stick Down"},{"a:4:-1","Right Stick Left"},{"a:4:1","Right Stick Right"},{"a:5:-1","Right Stick Up"},{"a:5:1","Right Stick Down"}};
    }
    void loadBindings() { defaults(); std::ifstream file(appHome() + "/input-map.conf"); if (!file) return; bindings_.clear(); std::string line; while (std::getline(file, line)) { auto separator = line.find('='); if (separator != std::string::npos) bindings_[line.substr(separator + 1)] = line.substr(0, separator); } if (bindings_.empty()) defaults(); }
    void saveBindings() { fs::create_directories(appHome()); std::ofstream file(appHome() + "/input-map.conf"); for (const auto &[token, action] : bindings_) file << action << '=' << token << '\n'; }
    void loadSettings() { std::ifstream file(appHome() + "/settings.conf"); std::string line; while (std::getline(file, line)) if (line == "keep_downloads=1") keepDownloads_ = true; }
    void saveSettings() { fs::create_directories(appHome()); std::ofstream file(appHome() + "/settings.conf"); file << "keep_downloads=" << (keepDownloads_ ? 1 : 0) << '\n'; }
    void action(const std::string &action) {
        if (action == "A") activate(); else if (action == "B") back(); else if (action == "X") contextX(); else if (action == "Y") contextY(); else if (action.find("Up") != std::string::npos) move(0, -1); else if (action.find("Down") != std::string::npos) move(0, 1); else if (action.find("Left") != std::string::npos) move(-1, 0); else if (action.find("Right") != std::string::npos) move(1, 0); else if (action == "L1") { if (screen_ == Screen::Keyboard) { symbols_ = !symbols_; selected_ = 0; } else move(-1, 0); } else if (action == "R1") move(1, 0); else if (action == "Start" && screen_ == Screen::Keyboard) submitKeyboard(); else if (action == "Menu") quit_ = true;
    }
    void capture(const std::string &token) {
        if (calibrationWait_ || SDL_TICKS_PASSED(calibrationReady_, SDL_GetTicks()) || calibrationStep_ >= int(calibrationActions_.size())) return;
        for (auto iterator = bindings_.begin(); iterator != bindings_.end();) iterator->second == calibrationActions_[size_t(calibrationStep_)] ? iterator = bindings_.erase(iterator) : ++iterator;
        bindings_[token] = calibrationActions_[size_t(calibrationStep_++)]; calibrationWait_ = true; waitToken_ = token;
        if (calibrationStep_ == int(calibrationActions_.size())) { saveBindings(); setToast("Controller mapping saved"); screen_ = Screen::Settings; selected_ = 3; calibrationWait_ = false; }
    }
    void pumpEvdev() {
        if (inputFd_ < 0) return;
        input_event event{};
        while (read(inputFd_, &event, sizeof(event)) == sizeof(event)) {
            if (event.type == EV_KEY) { std::string token = "k:" + std::to_string(event.code); if (screen_ == Screen::Calibration) { if (event.value == 0 && waitToken_ == token) { calibrationWait_ = false; calibrationReady_ = SDL_GetTicks() + 300; } else if (event.value == 1) capture(token); } else if (event.value == 1) { auto found = bindings_.find(token); if (found != bindings_.end()) action(found->second); } }
            else if (event.type == EV_ABS) { int threshold = (event.code == ABS_HAT0X || event.code == ABS_HAT0Y) ? 0 : 1500; int direction = event.value > threshold ? 1 : event.value < -threshold ? -1 : 0, old = axisDirection_[event.code]; axisDirection_[event.code] = direction; std::string base = "a:" + std::to_string(event.code) + ":"; if (screen_ == Screen::Calibration) { if (direction == 0 && waitToken_.rfind(base, 0) == 0) { calibrationWait_ = false; calibrationReady_ = SDL_GetTicks() + 300; } else if (direction && direction != old) capture(base + std::to_string(direction)); } else if (direction && direction != old) { auto found = bindings_.find(base + std::to_string(direction)); if (found != bindings_.end()) action(found->second); } }
        }
    }
public:
    bool init(bool headless) {
        headless_ = headless; fs::create_directories(appHome()); fs::create_directories(downloadRoot()); loadBindings(); loadSettings(); const char *shot = std::getenv("ROM_LIBRARY_SCREENSHOT"); if (shot) screenshot_ = shot;
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) return false;
        SDL_StartTextInput(); Uint32 flags = headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
        window_ = SDL_CreateWindow("ROM Library", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, flags); if (!window_) return false;
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE); if (!renderer_) return false; SDL_GetRendererOutputSize(renderer_, &width_, &height_); painter_ = std::make_unique<Painter>(renderer_);
        for (int index = 0; index < SDL_NumJoysticks(); ++index) { if (SDL_IsGameController(index)) { controller_ = SDL_GameControllerOpen(index); if (controller_) break; } else if (!joystick_) joystick_ = SDL_JoystickOpen(index); }
        if (!headless) inputFd_ = open("/dev/input/event1", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        return true;
    }
    void seedRegression() {
        results_.clear(); games_.clear(); trash_.clear();
        for (int i = 0; i < 12; ++i) results_.push_back({"Sample Game " + std::to_string(i + 1), "/sample-rom/", i % 2 ? "NDS" : "GBA", "", "100" + std::to_string(i)});
        for (int i = 0; i < 14; ++i) games_.push_back({"Installed Game " + std::to_string(i + 1), i % 3 == 0 ? "PSP" : i % 2 ? "NDS" : "GBA", "/tmp/game" + std::to_string(i), i % 3 == 0 ? ".iso" : i % 2 ? ".nds" : ".gba", uint64_t(8 + i) * 1024 * 1024});
        libraryFilters_ = {"All", "GBA", "NDS", "PSP"}; for (int i = 0; i < 3; ++i) trash_.push_back({"Removed Game " + std::to_string(i + 1), "GBA/Removed.gba", "/tmp/manifest", 8 * 1024 * 1024, 1, true});
        activeResult_ = results_[0]; activeGame_ = games_[0]; activeTrash_ = trash_[0]; downloadComplete_ = true;
    }
    void uiRegression() {
        seedRegression(); std::vector<Screen> screens = {Screen::Home, Screen::Keyboard, Screen::Results, Screen::Detail, Screen::Download, Screen::Library, Screen::LibraryDetail, Screen::ConfirmTrash, Screen::Trash, Screen::TrashDetail, Screen::ConfirmPurge, Screen::Settings, Screen::About, Screen::Calibration};
        for (Screen screen : screens) { screen_ = screen; selected_ = scroll_ = 0; draw(); }
        screen_ = Screen::Home; selected_ = 0;
        std::printf("UITEST screens=home,keyboard,results,detail,download,library,library-detail,remove-confirm,trash,trash-detail,purge-confirm,settings,about,calibration ok\n");
    }
    int loop(bool test, int seconds) {
        auto start = std::chrono::steady_clock::now(); while (!quit_) { pumpEvdev(); SDL_Event event;
            while (SDL_PollEvent(&event)) { if (event.type == SDL_QUIT) quit_ = true; else if (event.type == SDL_KEYDOWN) key(event.key.keysym.sym); else if (event.type == SDL_TEXTINPUT && screen_ == Screen::Keyboard) input_ += event.text.text; else if (inputFd_ < 0 && event.type == SDL_CONTROLLERBUTTONDOWN) button(event.cbutton.button); else if (inputFd_ < 0 && event.type == SDL_JOYBUTTONDOWN && !controller_) rawButton(event.jbutton.button); else if (inputFd_ < 0 && event.type == SDL_JOYHATMOTION) { if (event.jhat.value & SDL_HAT_UP) move(0, -1); else if (event.jhat.value & SDL_HAT_DOWN) move(0, 1); else if (event.jhat.value & SDL_HAT_LEFT) move(-1, 0); else if (event.jhat.value & SDL_HAT_RIGHT) move(1, 0); } }
            pollTask(); draw(); if (test && std::chrono::steady_clock::now() - start > std::chrono::seconds(seconds)) { std::printf("SELFTEST ui=up mappings=%s helper=%s romroot=%s driver=%s size=%dx%d joysticks=%d controller=%s raw=%s evdev=%s\n", platformFolder("GBA") == "GBA" && platformFolder("NDS") == "NDS" && platformFolder("PSP") == "PSP" ? "yes" : "no", fs::is_regular_file(helperPath()) ? "yes" : "no", fs::is_directory(romRoot()) ? "yes" : "no", SDL_GetCurrentVideoDriver(), width_, height_, SDL_NumJoysticks(), controller_ ? "yes" : "no", joystick_ ? "yes" : "no", inputFd_ >= 0 ? "yes" : "no"); return 0; }
            SDL_Delay(16);
        } return 0;
    }
    ~App() {
        task_.cancel = true; if (worker_.joinable()) worker_.join(); saveSettings(); painter_.reset(); if (inputFd_ >= 0) close(inputFd_); if (controller_) SDL_GameControllerClose(controller_); if (joystick_) SDL_JoystickClose(joystick_); if (renderer_) SDL_DestroyRenderer(renderer_); if (window_) SDL_DestroyWindow(window_); SDL_Quit();
    }
};

static int providerTest(const std::string &query) {
    TaskState task; try { ProviderClient provider; auto results = provider.search(query, &task); if (results.empty()) { std::fprintf(stderr, "PROVIDER_TEST search_results=0\n"); return 2; } SearchResult selected = provider.detail(results.front(), &task); Ticket ticket = provider.ticket(selected, &task); std::printf("PROVIDER_TEST search_results=%zu first_title=%s console=%s media_id=%s ticket_size=%llu download_host=approved download_ready=yes\n", results.size(), selected.title.c_str(), ticket.console.c_str(), ticket.mediaId.c_str(), static_cast<unsigned long long>(ticket.bytes)); return 0; } catch (const std::exception &exception) { std::fprintf(stderr, "PROVIDER_TEST error=%s\n", exception.what()); return 2; }
}

int main(int argc, char **argv) {
    bool selfTest = false, uiTest = false, smokeTest = false; std::string providerQuery;
    for (int index = 1; index < argc; ++index) { std::string argument = argv[index]; if (argument == "--self-test") selfTest = true; else if (argument == "--ui-test") uiTest = selfTest = true; else if (argument == "--display-smoke-test") smokeTest = true; else if (argument == "--provider-test" && index + 1 < argc) providerQuery = argv[++index]; else if (argument == "--version") { std::printf("ROM Library %s\n", VERSION); return 0; } }
    curl_global_init(CURL_GLOBAL_DEFAULT); if (!providerQuery.empty()) { int status = providerTest(providerQuery); curl_global_cleanup(); return status; }
    int status = 1;
    {
        if (selfTest) setenv("SDL_VIDEODRIVER", "dummy", 1);
        App app;
        if (!app.init(selfTest)) { std::fprintf(stderr, "ROM Library init failed: %s\n", SDL_GetError()); curl_global_cleanup(); return 1; }
        if (uiTest) app.uiRegression();
        status = app.loop(selfTest || smokeTest, smokeTest ? 8 : 3);
    }
    curl_global_cleanup(); return status;
}
