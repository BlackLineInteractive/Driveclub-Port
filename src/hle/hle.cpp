#include "hle/hle.h"

#include <array>
#include <unordered_map>

#include "common/sha1.h"

namespace Hle {

void RegisterKernel();
namespace VideoOut { void RegisterVideoOut(); }
namespace GnmDriver { void RegisterGnmDriver(); }
namespace PlayGo { void RegisterPlayGo(); }
namespace UserService { void RegisterUserService(); }
namespace SystemService { void RegisterSystemService(); }
namespace SaveData { void RegisterSaveData(); }
namespace Pad { void RegisterPad(); }
namespace Rtc { void RegisterRtc(); }
namespace Sysmodule { void RegisterSysmodule(); }
namespace Net { void RegisterNet(); }
namespace Np { void RegisterNp(); }
namespace Font { void RegisterFont(); }
namespace AvPlayer { void RegisterAvPlayer(); }
namespace MiscServices { void RegisterMiscServices(); }

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

struct NidName {
    const char* nid;
    const char* name;
};

constexpr NidName kNidNames[] = {
#include "hle/nid_db.inc"
};

} // namespace

std::string Nid(std::string_view symbol_name) {
    static constexpr u8 kSalt[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                     0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    std::string input(symbol_name);
    input.append(reinterpret_cast<const char*>(kSalt), sizeof(kSalt));
    const auto digest =
        Common::Sha1(reinterpret_cast<const u8*>(input.data()), input.size());

    // First 8 digest bytes read as a little-endian u64, then base64 of its
    // big-endian bytes, truncated to 11 chars; '/' becomes '-'.
    u64 v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | digest[i];
    }
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string out(11, '\0');
    for (int i = 0; i < 11; ++i) {
        const int shift = 58 - 6 * i; // 64 bits -> 11 sextets, last one padded
        const u64 sextet = shift >= 0 ? (v >> shift) & 0x3F : (v << -shift) & 0x3F;
        out[i] = kAlphabet[sextet];
    }
    return out;
}

std::string_view NameOf(std::string_view nid) {
    static const auto names = [] {
        std::unordered_map<std::string_view, std::string_view> m;
        for (const NidName& n : kNidNames) {
            m.emplace(n.nid, n.name);
        }
        return m;
    }();
    const auto it = names.find(nid);
    return it == names.end() ? nid : it->second;
}

void* Lookup(std::string_view library, std::string_view nid) {
    const auto it = Table().find(Key(library, nid));
    return it == Table().end() ? nullptr : it->second;
}

void Register(std::string_view library, std::string_view nid, void* function) {
    Table()[Key(library, nid)] = function;
}

void RegisterAll() {
    RegisterKernel();
    VideoOut::RegisterVideoOut();
    GnmDriver::RegisterGnmDriver();
    PlayGo::RegisterPlayGo();
    UserService::RegisterUserService();
    SystemService::RegisterSystemService();
    SaveData::RegisterSaveData();
    Pad::RegisterPad();
    Rtc::RegisterRtc();
    Sysmodule::RegisterSysmodule();
    Net::RegisterNet();
    Np::RegisterNp();
    Font::RegisterFont();
    AvPlayer::RegisterAvPlayer();
    MiscServices::RegisterMiscServices();
}

} // namespace Hle
