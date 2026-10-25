// Asset depot: <files>/img, <files>/sound, <files>/material  (files = Android/data/<package>/files/)
#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
namespace fs = std::filesystem;

class Depot {
public:
    fs::path root;

    bool init(const fs::path& filesDir) {                     // creates img/ sound/ material/
        root = filesDir;
        std::error_code ec;
        for (const char* k : {"img", "sound", "material"}) fs::create_directories(root / k, ec);
        return !ec;
    }
    static std::string ext(const fs::path& p) {
        std::string e = p.extension().string();
        for (auto& c : e) c = (char)tolower((unsigned char)c);
        return e;
    }
    static bool allowed(const std::string& kind, const std::string& e) {
        if (kind == "img") return e == ".png" || e == ".jpg" || e == ".jpeg";
        if (kind == "sound") return e == ".mp3" || e == ".ogg" || e == ".wav";
        if (kind == "material") return e == ".mat";
        return false;
    }
    fs::path pathOf(const std::string& kind, const std::string& name) const {
        return root / kind / fs::path(name).filename();         // filename(): no "../" tricks
    }
    // Reads the folder every time, so files dropped in with a file manager show up immediately.
    std::vector<std::string> list(const std::string& kind) const {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto& it : fs::directory_iterator(root / kind, ec))
            if (it.is_regular_file() && allowed(kind, ext(it.path()))) out.push_back(it.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    }
    // Upload from inside the engine: copy a file picked in the file browser into the depot.
    bool addFile(const fs::path& src, const std::string& kind, std::string* err = nullptr) {
        if (!allowed(kind, ext(src))) { if (err) *err = "Wrong file type for " + kind; return false; }
        std::error_code ec;
        fs::create_directories(root / kind, ec);
        fs::copy_file(src, pathOf(kind, src.filename().string()), fs::copy_options::overwrite_existing, ec);
        if (ec) { if (err) *err = ec.message(); return false; }
        return true;
    }
    bool remove(const std::string& kind, const std::string& name) {
        std::error_code ec;
        return fs::remove(pathOf(kind, name), ec) && !ec;
    }
    bool read(const std::string& kind, const std::string& name, std::string& out) const {
        std::ifstream f(pathOf(kind, name), std::ios::binary);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
        return true;
    }
    // material file (.mat): "specular=0.8\nshininess=48\nemissive=0\nmetal=1\n"
    bool readMaterial(const std::string& name, float out[4]) const {
        std::string txt;
        if (!read("material", name, txt)) return false;
        out[0] = 0; out[1] = 8; out[2] = 0; out[3] = 0;
        std::istringstream in(txt); std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq); float v = (float)atof(line.c_str() + eq + 1);
            if (k == "specular") out[0] = v; else if (k == "shininess") out[1] = v;
            else if (k == "emissive") out[2] = v; else if (k == "metal") out[3] = v;
        }
        return true;
    }
};

// File browser (the "pick a file" part of the Depot screen)
struct DirEntryInfo { std::string name; bool isDir; };
inline std::vector<DirEntryInfo> browse(const fs::path& dir, const std::string& kind) {
    std::vector<DirEntryInfo> out;
    std::error_code ec;
    for (auto& it : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::string n = it.path().filename().string();
        if (n.empty() || n[0] == '.') continue;
        if (it.is_directory(ec)) out.push_back({n, true});
        else if (Depot::allowed(kind, Depot::ext(it.path()))) out.push_back({n, false});
    }
    std::sort(out.begin(), out.end(), [](const DirEntryInfo& a, const DirEntryInfo& b) { return a.isDir != b.isDir ? a.isDir : a.name < b.name; });
    return out;
}
