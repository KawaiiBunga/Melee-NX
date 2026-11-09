#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace swdk::journal {
// Close every committed record. This flushes libc buffering during the session;
// a hard process crash needs no graceful-exit callback to preserve earlier records.
inline bool write(const std::string& path, const std::vector<uint8_t>& data, const char* mode) {
    FILE* f=std::fopen(path.c_str(),mode);
    if (!f) return false;
    bool ok=std::fwrite(data.data(),1,data.size(),f)==data.size();
    if (std::fflush(f)!=0) ok=false;
    if (std::fclose(f)!=0) ok=false;
    return ok;
}
// Keep the damaged input for inspection. Never append behind a torn record:
// readers would stop there and could never see subsequent sessions' shaders.
inline bool recover(const std::string& path, const std::vector<uint8_t>& valid,
                    const std::string& suffix) {
    const std::string temp=path+".recover.tmp", backup=path+".damaged-"+suffix;
    if (!write(temp,valid,"wb")) return false;
    if (std::rename(path.c_str(),backup.c_str())!=0) return false;
    if (std::rename(temp.c_str(),path.c_str())==0) return true;
    std::rename(backup.c_str(),path.c_str());
    return false;
}
}
