// 3gine native addon SDK. Build your addon as a shared library (.so, arm64-v8a) and export:
//     extern "C" void gine_addon_init(const GineAPI* api);
// Put it in  Documents/3gine/addons/<name>.so  (rename to <name>.so.disable to turn it off).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GineAPI {
    int version;                                              // 1
    void (*log)(const char* msg);
    int  (*spawn)(const char* mesh, float x, float y, float z);  // returns object id
    void (*get_pos)(int id, float* x, float* y, float* z);
    void (*set_pos)(int id, float x, float y, float z);
    int  (*find)(const char* name);                           // 0 if not found
    void (*on_update)(void (*fn)(float dt));                  // called every frame while playing
} GineAPI;
typedef void (*GineAddonInit)(const GineAPI* api);
#ifdef __cplusplus
}
#endif
