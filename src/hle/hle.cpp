#include "hle/hle.h"

#include <string>
#include <unordered_map>

namespace Hle {

namespace {

std::unordered_map<std::string, void*>& Table() {
    static std::unordered_map<std::string, void*> table;
    return table;
}

std::string Key(std::string_view library, std::string_view nid) {
    std::string key;
    key.reserve(library.size() + 1 + nid.size());
    key.append(library).append(":").append(nid);
    return key;
}

} // namespace

void* Lookup(std::string_view library, std::string_view nid) {
    const auto it = Table().find(Key(library, nid));
    return it == Table().end() ? nullptr : it->second;
}

void Register(std::string_view library, std::string_view nid, void* function) {
    Table()[Key(library, nid)] = function;
}

} // namespace Hle
