// Host tests for the launcher's data layer and the game's launch.cfg parser.
// Run: powershell -ExecutionPolicy Bypass -File launcher\tests\run-host-tests.ps1
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "data/dat_identity.h"
#include "data/http.h"
#include "data/lan_signin.h"
#include "data/launch.h"
#include "data/mods.h"
#include "data/settings.h"
#include "data/slippi_account.h"
#include "launch_request.h"

namespace fs = std::filesystem;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) {                                                                 \
            ++g_passed;                                                             \
        } else {                                                                    \
            ++g_failed;                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                           \
    } while (0)

// http::Get stand-in: the tests never touch the network.
namespace mnx::http {
Response Get(const std::string&, size_t) { return {false, 0, "", "offline test"}; }
}  // namespace mnx::http

namespace {

void Put32(std::vector<std::uint8_t>& b, size_t at, std::uint32_t v) {
    b[at] = v >> 24;
    b[at + 1] = v >> 16;
    b[at + 2] = v >> 8;
    b[at + 3] = v;
}

// A minimal HSD archive with the given root names.
std::vector<std::uint8_t> MakeDat(const std::vector<std::string>& roots) {
    const std::uint32_t dataSize = 0x40;
    std::string strings;
    std::vector<std::uint32_t> nameOffsets;
    for (const auto& r : roots) {
        nameOffsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings += r;
        strings.push_back('\0');
    }
    const size_t rootTable = 0x20 + dataSize;
    std::vector<std::uint8_t> b(rootTable + roots.size() * 8 + strings.size(), 0);
    Put32(b, 0, static_cast<std::uint32_t>(b.size()));
    Put32(b, 4, dataSize);
    Put32(b, 8, 0);
    Put32(b, 12, static_cast<std::uint32_t>(roots.size()));
    Put32(b, 16, 0);
    for (size_t i = 0; i < roots.size(); ++i) {
        Put32(b, rootTable + i * 8, 0);
        Put32(b, rootTable + i * 8 + 4, nameOffsets[i]);
    }
    std::memcpy(&b[rootTable + roots.size() * 8], strings.data(), strings.size());
    return b;
}

void WriteBytes(const fs::path& p, const std::vector<std::uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

void WriteText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string Slash(const fs::path& p) { return p.generic_string(); }

void TestDatIdentity() {
    auto id = mnx::dat::IdentifyCostume(MakeDat({"PlyFox5KGr_Share_joint", "PlyFox5KGr_Share_matanim_joint"}));
    CHECK(id.ok);
    CHECK(id.targetFile == "PlFxGr.dat");
    CHECK(id.fighter == "Fox" && id.costume == "Green");
    // Default costumes have no colour suffix in the root but end in Nr on disc.
    id = mnx::dat::IdentifyCostume(MakeDat({"PlyCaptain5K_Share_joint"}));
    CHECK(id.ok && id.targetFile == "PlCaNr.dat");
    // A colour the fighter does not have is not a slot.
    CHECK(!mnx::dat::IdentifyCostume(MakeDat({"PlyFox5KBu_Share_joint"})).ok);
    CHECK(!mnx::dat::IdentifyCostume(MakeDat({"SomethingElse"})).ok);
    // Conflicting roots are refused.
    CHECK(!mnx::dat::IdentifyCostume(MakeDat({"PlyFox5KGr_Share_joint", "PlyFalco5KRe_Share_joint"})).ok);
    // Length field must match the file.
    auto bad = MakeDat({"PlyFox5KGr_Share_joint"});
    bad.push_back(0);
    CHECK(!mnx::dat::IdentifyCostume(bad).ok);
    CHECK(!mnx::dat::IdentifyCostume({1, 2, 3}).ok);
    CHECK(mnx::dat::DescribeCostumeFile("PlFxGr.dat") == "Fox, Green");
    CHECK(mnx::dat::DescribeCostumeFile("PlCaRe.usd") == "Captain Falcon, Red");
    CHECK(mnx::dat::DescribeCostumeFile("GrNBa.dat").empty());
    CHECK(mnx::dat::DescribeCostumeFile("PlFxBu.dat").empty());
}

void TestModCatalog(const fs::path& tmp) {
    const fs::path mods = tmp / "mods", files = tmp / "files";
    WriteText(files / "PlFxGr.dat", "x");
    WriteText(files / "PlCaRe.dat", "x");
    WriteText(files / "PlCaRe.usd", "x");
    WriteText(files / "GrNBa.dat", "x");
    WriteText(mods / "Alpha" / "PlFxGr.dat", "alpha");
    WriteText(mods / "Alpha" / "mod.json", R"({"name":"Alpha Fox","author":"a"})");
    WriteText(mods / "Beta" / "files" / "PlFxGr.dat", "beta");
    WriteText(mods / "Beta" / "files" / "GrNBa.dat", "beta");
    WriteText(mods / "Beta" / "readme.txt", "not a game file");
    WriteText(mods / "Gamma" / "PlCaRe.dat", "gamma");  // .usd twin added from the disc
    WriteText(mods / "Gamma" / "Nope.dat", "missing on disc");
    WriteBytes(mods / "Tom Nook.dat", MakeDat({"PlyFox5KGr_Share_joint"}));
    // A costume-site download: freely named DAT plus portrait/stock PNGs, inside a folder.
    WriteText(files / "PlMrYe.dat", "x");
    WriteBytes(mods / "Sponge" / "PlMrYe sbsp.dat", MakeDat({"PlyMario5KYe_Share_joint"}));
    WriteText(mods / "Sponge" / "csp.png", "png");
    WriteText(mods / "Sponge" / "stc.png", "png");
    WriteText(mods / "broken.dat", "not a dat");

    mnx::ModCatalog cat;
    const auto rep = cat.Scan(Slash(mods), Slash(files), true);
    CHECK(rep.identified == 1);
    CHECK(rep.problems.size() == 1);  // broken.dat
    CHECK(fs::exists(mods / "Tom Nook" / "PlFxGr.dat"));
    CHECK(!fs::exists(mods / "Tom Nook.dat"));
    CHECK(cat.Mods().size() == 5);
    const mnx::Mod* beta = nullptr;
    const mnx::Mod* gamma = nullptr;
    const mnx::Mod* sponge = nullptr;
    for (const auto& m : cat.Mods()) {
        if (m.id == "Beta") beta = &m;
        if (m.id == "Gamma") gamma = &m;
        if (m.id == "Sponge") sponge = &m;
    }
    CHECK(sponge && sponge->files.size() == 1 && sponge->files[0].discPath == "PlMrYe.dat" && sponge->warning.empty());
    CHECK(sponge && !sponge->notes.empty() && sponge->notes[0] == "Mario, Yellow");
    CHECK(beta && beta->files.size() == 2);  // readme.txt skipped, files/ stripped
    CHECK(gamma && gamma->files.size() == 3);
    CHECK(gamma && !gamma->warning.empty());
    CHECK(cat.Mods()[0].name == "Alpha Fox");  // sorted by name

    cat.SetEnabled("Beta", true);
    cat.SetEnabled("Alpha", true);
    CHECK(cat.EnabledCount() == 2);
    CHECK(cat.Mods()[0].id == "Beta" && cat.Mods()[1].id == "Alpha");
    auto conflicts = cat.Conflicts();
    CHECK(conflicts.size() == 1 && conflicts[0].winner == "Beta" && conflicts[0].loser == "Alpha");
    cat.Move("Alpha", -1);
    CHECK(cat.Mods()[0].id == "Alpha");
    cat.Move("Alpha", -1);  // already first: no change
    CHECK(cat.Mods()[0].id == "Alpha");

    const std::string state = Slash(mods / "mods.json"), manifest = Slash(mods / "active.txt");
    CHECK(cat.SaveState(state));
    std::string err;
    CHECK(cat.WriteManifest(manifest, true, &err));
    std::string text;
    CHECK(mnx::ReadFile(manifest, text));
    // Alpha has priority, so its PlFxGr.dat is listed and Beta's is not.
    CHECK(text.find("PlFxGr.dat\t" + Slash(mods / "Alpha" / "PlFxGr.dat")) != std::string::npos);
    CHECK(text.find(Slash(mods / "Beta" / "files" / "PlFxGr.dat")) == std::string::npos);
    CHECK(text.find("GrNBa.dat\t") != std::string::npos);

    // Reload in a fresh catalog: order survives.
    mnx::ModCatalog again;
    again.Scan(Slash(mods), Slash(files), true);
    again.LoadState(state);
    CHECK(again.EnabledCount() == 2 && again.Mods()[0].id == "Alpha" && again.Mods()[1].id == "Beta");
    again.SetEnabled("Alpha", false);
    CHECK(again.Mods()[0].id == "Beta");

    // Inactive: the manifest goes away so the game runs vanilla.
    CHECK(cat.WriteManifest(manifest, false, &err));
    CHECK(!fs::exists(manifest));
}

const char* kUser = R"({"uid":"AbC123xyz","playKey":"secret-key","connectCode":"TEST#123","displayName":"Tester","latestVersion":"3.4.0"})";

void TestSlippiAccount(const fs::path& tmp) {
    mnx::slippi::Account a;
    std::string err;
    CHECK(mnx::slippi::ParseAccount(kUser, a, err));
    CHECK(a.connectCode == "TEST#123" && a.displayName == "Tester");
    CHECK(!mnx::slippi::ParseAccount("{}", a, err));
    CHECK(!mnx::slippi::ParseAccount("not json", a, err));
    CHECK(!mnx::slippi::ParseAccount(R"({"uid":"a","playKey":"k"})", a, err));                       // no code
    CHECK(!mnx::slippi::ParseAccount(R"({"uid":"a/../b","playKey":"k","connectCode":"A#1"})", a, err));  // unsafe uid
    CHECK(!mnx::slippi::ParseAccount(R"({"uid":"a","playKey":"k","connectCode":"NOHASH"})", a, err));
    const std::string path = Slash(tmp / "slippi" / "user.json");
    CHECK(mnx::slippi::LoadAccount(path).state == mnx::slippi::State::None);
    CHECK(mnx::slippi::SaveAccountText(path, kUser, err));
    CHECK(mnx::slippi::LoadAccount(path).state == mnx::slippi::State::Valid);
    CHECK(!mnx::slippi::SaveAccountText(path, "{}", err));
    CHECK(mnx::slippi::LoadAccount(path).state == mnx::slippi::State::Valid);  // bad upload kept the old one
    // Offline refresh fails cleanly and leaves the file alone.
    mnx::slippi::Account loaded = mnx::slippi::LoadAccount(path).account;
    CHECK(!mnx::slippi::RefreshProfile(path, loaded, err));
    CHECK(mnx::slippi::DeleteAccount(path));
    CHECK(mnx::slippi::LoadAccount(path).state == mnx::slippi::State::None);
}

std::string g_saved;
bool SaveStub(const std::string& text, std::string&) {
    g_saved = text;
    return true;
}

void TestSignin() {
    using namespace mnx::signin;
    Request r;
    CHECK(ParseRequest("GET / HTTP/1.1\r\nHost: x\r\n", r, 1024) == Parse::Incomplete);
    CHECK(ParseRequest("GET / HTTP/1.1\r\nHost: x\r\n\r\n", r, 1024) == Parse::Ok);
    CHECK(r.method == "GET" && r.path == "/" && r.headers["host"] == "x");
    const std::string body = kUser;
    const std::string post = "POST /account HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\nX-Pair-Code: 123456\r\n\r\n";
    CHECK(ParseRequest(post + body.substr(0, 10), r, 16384) == Parse::Incomplete);
    CHECK(ParseRequest(post + body, r, 16384) == Parse::Ok);
    CHECK(r.body == body);
    CHECK(ParseRequest("POST /account HTTP/1.1\r\nContent-Length: 99999\r\n\r\n", r, 16384) == Parse::TooLarge);
    CHECK(ParseRequest("POST /account HTTP/1.1\r\nContent-Length: -1\r\n\r\n", r, 16384) == Parse::Bad);
    CHECK(ParseRequest("POST /account HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", r, 16384) == Parse::Bad);
    CHECK(ParseRequest("garbage\r\n\r\n", r, 16384) == Parse::Bad);
    CHECK(ParseRequest(std::string(9000, 'a'), r, 16384) == Parse::TooLarge);

    Request get;
    ParseRequest("GET / HTTP/1.1\r\n\r\n", get, 1024);
    auto o = Handle(get, "123456", SaveStub);
    CHECK(o.response.find("200 OK") != std::string::npos && o.response.find("<html") != std::string::npos);

    Request up;
    ParseRequest(post + body, up, 16384);
    o = Handle(up, "654321", SaveStub);
    CHECK(o.wrongCode && !o.accepted && o.response.find("403") != std::string::npos);
    g_saved.clear();
    o = Handle(up, "123456", SaveStub);
    CHECK(o.accepted && g_saved == body);
    CHECK(o.summary == "Tester (TEST#123)");
    CHECK(o.response.find("secret-key") == std::string::npos);  // never echo the play key

    Request badBody;
    ParseRequest("POST /account HTTP/1.1\r\nContent-Length: 2\r\nX-Pair-Code: 123456\r\n\r\n{}", badBody, 16384);
    o = Handle(badBody, "123456", SaveStub);
    CHECK(!o.accepted && o.response.find("400") != std::string::npos);

    const std::string code = NewPairCode();
    CHECK(code.size() == 6 && code.find_first_not_of("0123456789") == std::string::npos);
}

void TestLaunchHandoff() {
    mnx::launch::Request req{"sdmc:/switch/melee-nx/Melee 1.02.iso", "sdmc:/switch/melee-nx/mods/active.txt",
                             "sdmc:/switch/melee-nx/melee-nx-launcher.nro", "slippi"};
    const auto parsed = melee_nx::ParseLaunchConfig(mnx::launch::FormatConfig(req));
    CHECK(parsed.present);
    CHECK(parsed.disc == req.bootArg && parsed.mods == req.modsManifest && parsed.returnTo == req.returnTo);
    CHECK(parsed.mode == "slippi" && parsed.code.empty());
    req.code = "ABCD#123";
    CHECK(melee_nx::ParseLaunchConfig(mnx::launch::FormatConfig(req)).code == "ABCD#123");
    std::string code = " abcd#12 ";
    CHECK(mnx::launch::NormalizeConnectCode(code) && code == "ABCD#12");
    for (const char* bad : {"", "ABCD", "#123", "ABCDEFGHI#1", "AB#1234", "AB#1A", "A B#C", "AB##1", "AB-C#1"}) {
        std::string b = bad;
        CHECK(!mnx::launch::NormalizeConnectCode(b));
    }
    CHECK(melee_nx::ParseLaunchConfig("disc=x\n").mode.empty());  // older launch.cfg: vanilla
    const auto empty = melee_nx::ParseLaunchConfig("# nothing\nmods=\n");
    CHECK(!empty.present);
    const auto crlf = melee_nx::ParseLaunchConfig("disc=@melee-nx-loose\r\nmods=\r\nreturn=\r\n");
    CHECK(crlf.present && crlf.disc == "@melee-nx-loose" && crlf.mods.empty());
    char a0[] = "melee.nro", a1[] = "--launch";
    char* argv[] = {a0, a1};
    CHECK(melee_nx::HasLaunchFlag(2, argv));
    CHECK(!melee_nx::HasLaunchFlag(1, argv));
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "mnx-launcher-tests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    TestDatIdentity();
    TestModCatalog(tmp);
    TestSlippiAccount(tmp);
    TestSignin();
    TestLaunchHandoff();
    fs::remove_all(tmp);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
