#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include "core.hpp"

using namespace geode::prelude;

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int);

namespace {

constexpr int kCtrl = 0x11, kShift = 0x10, kAlt = 0x12;
constexpr float kFade = .3f;

bool held(int vk) { return GetAsyncKeyState(vk) & 0x8000; }

int readMods() { return held(kCtrl) | (held(kShift) << 1) | (held(kAlt) << 2); }

template <class T>
T opt(char const* key) { return Mod::get()->getSettingValue<T>(key); }

void notify(std::string const& s, NotificationIcon icon = NotificationIcon::Info) { Notification::create(s, icon)->show(); }

// Chaikin smoothing, display only.
std::vector<CCPoint> smooth(std::vector<slop::V> const& raw) {
    std::vector<CCPoint> p;
    for (auto& v : raw) p.push_back(ccp((float)v.x, (float)v.y));
    for (int it = 0; it < 2 && p.size() > 2; it++) {
        std::vector<CCPoint> q{p.front()};
        for (size_t i = 0; i + 1 < p.size(); i++) {
            q.push_back(p[i] * .75f + p[i + 1] * .25f);
            q.push_back(p[i] * .25f + p[i + 1] * .75f);
        }
        q.push_back(p.back());
        p = std::move(q);
    }
    return p;
}

class Trace : public CCDrawNode {
public:
    std::vector<slop::V> raw;
    std::vector<slop::Piece> pieces;
    std::function<void()> tick;
    float fade = -1;
    bool cancelled = false;

    static Trace* create() {
        auto t = new Trace();
        if (t->init()) {
            t->autorelease();
            t->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
            t->scheduleUpdate();
            return t;
        }
        delete t;
        return nullptr;
    }

    void finish(bool cancel) {
        cancelled = cancel;
        pieces.clear();
        tick = nullptr;
        fade = kFade;
        redraw();
    }

    void update(float dt) override {
        if (fade < 0) {
            if (auto t = tick) t();
            return;
        }
        fade -= dt;
        if (fade > 0) return redraw();
        retain();
        removeFromParent();
        autorelease();
    }

    void redraw() {
        clear();
        float s = getParent() ? getParent()->getScale() : 1.f;
        float a = fade < 0 ? 1.f : fade / kFade;
        for (auto& p : pieces) {
            CCPoint v[4];
            for (int i = 0; i < p.n; i++) v[i] = ccp((float)p.poly[i].x, (float)p.poly[i].y);
            drawPolygon(v, p.n, {0, 0, 0, .4f}, .8f / s, {1, 1, 1, .7f});
        }
        auto col = cancelled ? ccColor4F{1, .3f, .3f, 1} : ccc4FFromccc4B(opt<ccColor4B>("trace-color"));
        col.a *= a;
        float r = (float)opt<double>("trace-width") / 2 / s;
        auto pts = smooth(raw);
        for (size_t i = 1; i < pts.size(); i++) drawSegment(pts[i - 1], pts[i], r, col);
        if (pts.size() == 1) drawDot(pts[0], r, col);
    }
};

// Runs a function next frame, outside whatever game code is on the stack now.
class Deferred : public CCNode {
public:
    std::function<void()> fn;

    static void run(std::function<void()> f) {
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;
        auto d = new Deferred();
        d->init();
        d->autorelease();
        d->fn = std::move(f);
        scene->addChild(d);
        d->scheduleUpdate();
    }

    void update(float) override {
        auto f = fn;
        unscheduleUpdate();
        retain();
        removeFromParent();
        if (f) f();
        autorelease();
    }
};

// ---- slope families -------------------------------------------------------------

std::unordered_map<int, std::string> const& frames() {
    static std::unordered_map<int, std::string> f;
    if (f.empty())
        for (auto& [k, v] : ObjectToolbox::sharedState()->m_allKeys) f[k] = v.c_str();
    return f;
}

bool g_probed = false;
CCPoint g_off; // node position of an object saved at (0, 0)

// 0 = not a slope, 1 = 1x1, 2 = 2x1. Uses a real level object: createWithKey alone
// does not set the object type.
int sizeClass(LevelEditorLayer* lel, int id) {
    static std::unordered_map<int, int> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    int c = 0;
    if (id > 0 && frames().count(id)) {
        auto a = lel->createObjectsFromString(("1," + std::to_string(id) + ",2,0,3,0").c_str(), true, true);
        if (a && a->count()) {
            auto o = static_cast<GameObject*>(a->objectAtIndex(0));
            auto s = o->getContentSize();
            if (o->m_objectType == GameObjectType::Slope && s.height > 0) {
                float r = s.width / s.height;
                c = r > 1.5f ? 2 : r > .6f ? 1 : 0;
            }
            if (!g_probed) g_off = o->getPosition(), g_probed = true;
            log::debug("id {} type {} size {}x{} -> {}", id, (int)o->m_objectType, s.width, s.height, c);
            for (auto obj : CCArrayExt<GameObject*>(a)) lel->removeObject(obj, true);
        }
    }
    return cache[id] = c;
}

struct Family {
    int one = 0, two = 0;
};

std::unordered_map<int, Family> g_fam;

// The other size of the same block set: nearby id with the longest shared frame name.
Family family(LevelEditorLayer* lel, int id) {
    if (auto it = g_fam.find(id); it != g_fam.end()) return it->second;
    int c = sizeClass(lel, id);
    if (!c) return g_fam[id] = {};
    auto const& name = frames().at(id);
    int best = 0, bestScore = -1;
    for (int d = -12; d <= 12; d++) {
        if (!d || sizeClass(lel, id + d) != 3 - c) continue;
        auto const& other = frames().at(id + d);
        int pre = 0;
        while (pre < (int)name.size() && pre < (int)other.size() && name[pre] == other[pre]) pre++;
        int score = pre * 64 - std::abs(d) * 2 + ((d > 0) == (c == 1));
        if (score > bestScore) bestScore = score, best = id + d;
    }
    return g_fam[id] = c == 1 ? Family{id, best} : Family{best, id};
}

// Object props copied from a selected slope, minus everything slop sets itself.
std::string stripTemplate(std::string const& save, bool dropLayer) {
    static const std::unordered_set<std::string> drop{"1", "2", "3", "4", "5", "6", "32", "128", "129", "131", "132", "108"};
    std::vector<std::string> tok;
    size_t start = 0;
    for (size_t i = 0; i <= save.size(); i++) {
        if (i == save.size() || save[i] == ',' || save[i] == ';') {
            tok.push_back(save.substr(start, i - start));
            start = i + 1;
        }
    }
    std::string out;
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
        if (drop.count(tok[i]) || (dropLayer && tok[i] == "20")) continue;
        out += ',' + tok[i] + ',' + tok[i + 1];
    }
    return out;
}

struct Template {
    int id = 289;
    std::string save;
};

Template g_tpl;
int g_lastIndex = -1;
void* g_lastSel = nullptr;

bool twoGuess() {
    auto it = g_fam.find(g_tpl.id);
    return it == g_fam.end() || it->second.two;
}

// ---- route recording ------------------------------------------------------------

struct Recorder {
    GJBaseGameLayer* layer = nullptr;
    std::vector<GameObject*> objs; // index = cand
    slop::Route r;
    bool brk[2] = {true, true}, has[2] = {false, false}, mini[2] = {false, false};
    float lx[2] = {0, 0}, ly[2] = {0, 0};
};

Recorder g_rec;
bool g_routeDirty = true;

struct TestRun {
    bool on = false, started = false, failed = false, done = false;
    size_t next = 0;
    std::vector<slop::Input> in;
    float endX = 0, failX = 0;
    double f = 1;
    EditorUI* ui = nullptr;
};

TestRun g_test;

std::filesystem::path routeFile(GJGameLevel* level) {
    std::string n = level ? std::string(level->m_levelName.c_str()) : "";
    for (auto& c : n)
        if (!std::isalnum((unsigned char)c)) c = '_';
    auto dir = Mod::get()->getSaveDir() / "routes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / ((n.empty() ? "unnamed" : n) + ".bin");
}

template <class T>
void putVec(std::ofstream& f, std::vector<T> const& v) {
    uint32_t n = (uint32_t)v.size();
    f.write((char const*)&n, 4);
    if (n) f.write((char const*)v.data(), n * sizeof(T));
}

template <class T>
bool getVec(std::ifstream& f, std::vector<T>& v) {
    uint32_t n = 0;
    if (!f.read((char*)&n, 4) || n > 100000000) return false;
    v.resize(n);
    return !n || (bool)f.read((char*)v.data(), n * sizeof(T));
}

// Route file to read for a level: its own, or for a copy ("Name 2", "Name copy") the original's.
std::filesystem::path routeRead(GJGameLevel* level) {
    auto path = routeFile(level);
    if (!std::filesystem::exists(path)) {
        // a copy ("Name 2", "Name copy") falls back to the original's route
        auto stem = path.stem().string();
        auto cut = stem.find_last_of('_');
        if (cut != std::string::npos && cut > 0) {
            auto tail = stem.substr(cut + 1);
            bool num = !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return std::isdigit((unsigned char)c); });
            if (num || tail == "copy" || tail == "Copy") path.replace_filename(stem.substr(0, cut) + ".bin");
        }
    }
    return path;
}

// Pieces computed offline (tools/slop_tool pieces): <route>.pieces.txt next to the route.
std::filesystem::path externalFile(GJGameLevel* level) {
    auto p = routeRead(level);
    p.replace_extension(".pieces.txt");
    return p;
}

bool readRouteFile(std::filesystem::path const& path, slop::Route& r) {
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    if (!f.read(magic, 4) || std::memcmp(magic, "SLP2", 4)) return false;
    return getVec(f, r.ticks) && getVec(f, r.obs) && getVec(f, r.inputs) && getVec(f, r.cands);
}

bool readRoute(GJGameLevel* level, slop::Route& r) { return readRouteFile(routeRead(level), r); }

// Same level (and same object list, so obstacle -> object indices stay valid).
bool sameLevel(slop::Route const& a, slop::Route const& b) {
    if (a.cands.size() != b.cands.size()) return false;
    if (a.cands.empty()) return true;
    auto eq = [](slop::Cand const& p, slop::Cand const& q) { return p.id == q.id && std::abs(p.x - q.x) < .01f && std::abs(p.y - q.y) < .01f; };
    return eq(a.cands.front(), b.cands.front()) && eq(a.cands.back(), b.cands.back());
}

// The old route outside [a, b] + the new run inside it.
slop::Route mergeRoute(slop::Route const& old, slop::Route const& run, float a, float b) {
    slop::Route o;
    o.cands = run.cands;
    auto add = [&](slop::Route const& src, slop::Tick t, bool brk) {
        uint32_t first = (uint32_t)o.obs.size();
        for (uint32_t q = t.first; q < t.first + t.count; q++) o.obs.push_back(src.obs[q]);
        t.first = first;
        if (brk) t.brk = 1;
        o.ticks.push_back(t);
    };
    bool first[2] = {true, true};
    for (auto const& t : old.ticks)
        if (t.x < a) add(old, t, false);
    for (auto const& t : run.ticks) add(run, t, first[t.p]), first[t.p] = false;
    first[0] = first[1] = true;
    for (auto const& t : old.ticks)
        if (t.x > b) add(old, t, first[t.p]), first[t.p] = false;
    for (auto const& e : old.inputs)
        if (e.x < a) o.inputs.push_back(e);
    for (auto const& e : run.inputs) o.inputs.push_back(e);
    for (auto const& e : old.inputs)
        if (e.x > b) o.inputs.push_back(e);
    return o;
}

// Set by Apply: runs on that level are runs on the slope version, kept apart from its route.
std::filesystem::path appliedMark(GJGameLevel* level) {
    auto p = routeFile(level);
    p.replace_extension(".applied");
    return p;
}

void writeRoute(std::filesystem::path const& path, slop::Route const& r) {
    std::ofstream f(path, std::ios::binary);
    f.write("SLP2", 4);
    putVec(f, r.ticks);
    putVec(f, r.obs);
    putVec(f, r.inputs);
    putVec(f, r.cands);
}

void clearRun() {
    g_rec.r.ticks.clear();
    g_rec.r.obs.clear();
    g_rec.r.inputs.clear();
}

void truncate(float x);

// Never refuses a run: a full run replaces the route (old one kept as .prev.bin), an unfinished
// run of the same level only replaces the stretch it covers.
void saveRoute(GJGameLevel* level, bool complete) {
    auto& r = g_rec.r;
    if (!complete && r.ticks.size() > 48) truncate(r.ticks[r.ticks.size() - 24].x); // may end in the crash
    if (r.ticks.empty()) return;
    float a = 1e9f, b = -1e9f;
    for (auto& t : r.ticks) a = std::min(a, t.x), b = std::max(b, t.x);
    auto path = routeFile(level); // own file only, never the original's a copy falls back to
    bool slopeRun = std::filesystem::exists(appliedMark(level));
    if (slopeRun) path.replace_extension(".slope.bin");
    slop::Route old;
    bool merged = false;
    if (readRouteFile(path, old) && sameLevel(old, r)) {
        float omx = -1e9f;
        for (auto& t : old.ticks) omx = std::max(omx, t.x);
        merged = b < omx - 30;
    }
    std::error_code ec;
    if (std::filesystem::exists(path)) {
        auto prev = path;
        prev.replace_extension(".prev.bin");
        std::filesystem::copy_file(path, prev, std::filesystem::copy_options::overwrite_existing, ec);
    }
    auto const& out = merged ? mergeRoute(old, r, a, b) : r;
    writeRoute(path, out);
    if (slopeRun) notify("slop: run on the slope version saved apart (" + path.filename().string() + ")", NotificationIcon::Info);
    else if (merged)
        notify("slop: re-recorded x " + std::to_string((int)a) + "-" + std::to_string((int)b) + ", rest of the route kept", NotificationIcon::Success);
    else
        notify("slop: route saved, " + std::to_string(slop::split(r).size()) + " wave parts, " + std::to_string(r.inputs.size()) + " clicks",
               NotificationIcon::Success);
    log::info("route saved: {} ticks, {} obstacles, {} inputs, {} objects", r.ticks.size(), r.obs.size(), r.inputs.size(), r.cands.size());
    clearRun();
    g_routeDirty = true;
}

void collect(GJBaseGameLayer* gl) {
    g_rec = {};
    g_rec.layer = gl;
    for (auto o : CCArrayExt<GameObject*>(gl->m_objects)) {
        auto t = o->m_objectType;
        if (t != GameObjectType::Solid && t != GameObjectType::Hazard && t != GameObjectType::Slope) continue;
        g_rec.objs.push_back(o);
        g_rec.r.cands.push_back({o->m_objectID, o->m_startPosition.x, o->m_startPosition.y});
    }
}

// Hitbox shape of an object right now: circle (saws), slope triangle, rotated box or plain rect.
int polyOf(GameObject* o, CCRect const& b, bool baseRight, slop::V* poly) {
    if (float rad = o->getObjectRadius(); rad > 0) {
        for (int i = 0; i < slop::kMaxPoly; i++) {
            double a = i * 6.283185307179586 / slop::kMaxPoly;
            poly[i] = {b.getMidX() + rad * std::cos(a), b.getMidY() + rad * std::sin(a)};
        }
        return slop::kMaxPoly;
    }
    float rot = o->getRotation(), m90 = std::fmod(std::abs(rot), 90.f);
    if (o->m_objectType == GameObjectType::Slope) {
        int r90 = ((int)std::lround(rot / 90) % 4 + 4) % 4;
        slop::slopeTri(b.getMidX(), b.getMidY(), b.size.width, b.size.height, o->m_isFlipX, o->m_isFlipY, r90 * 90, baseRight, poly);
        return 3;
    }
    if (m90 > .5f && m90 < 89.5f && o->m_orientedBox) {
        for (int i = 0; i < 4; i++) poly[i] = {o->m_orientedBox->m_corners[i].x, o->m_orientedBox->m_corners[i].y};
        return 4;
    }
    poly[0] = {b.getMinX(), b.getMinY()};
    poly[1] = {b.getMaxX(), b.getMinY()};
    poly[2] = {b.getMaxX(), b.getMaxY()};
    poly[3] = {b.getMinX(), b.getMaxY()};
    return 4;
}

// respawn: drop everything recorded after the respawn point
void truncate(float x) {
    auto& r = g_rec.r;
    while (!r.ticks.empty() && r.ticks.back().x > x - 1) {
        r.obs.resize(r.ticks.back().first);
        r.ticks.pop_back();
    }
    while (!r.inputs.empty() && r.inputs.back().x > x - 1) r.inputs.pop_back();
    g_rec.brk[0] = g_rec.brk[1] = true;
}

// ground / ceiling line, if shown and consistent with where the player is
void addBound(GJBaseGameLayer* gl, GJGroundLayer* g, float cy, float hh, bool bottom) {
    if (!g || !g->isVisible() || !g->getParent()) return;
    float y = gl->m_objectLayer->convertToNodeSpace(g->convertToWorldSpace({0, 0})).y;
    if (bottom ? cy - hh < y - 2 : cy + hh > y + 2) return;
    g_rec.r.obs.push_back(bottom ? slop::Obstacle{-1e6f, y, 3, -1} : slop::Obstacle{y, 1e6f, 3, -1});
}

void stopTestLater() {
    Deferred::run([] {
        if (g_test.on && g_test.ui) g_test.ui->onStopPlaytest(nullptr);
    });
}

void testFail(float x) {
    if (!g_test.on || g_test.failed || g_test.done) return;
    g_test.failed = true;
    g_test.failX = x;
    stopTestLater();
}

// One physics step of one player: the wave hitbox and every obstacle in its column as it is
// right now (moved, rotated, toggled - whatever the triggers did).
// Editor playtests are not recorded unless asked: playtesting an applied copy would
// overwrite the route of the original.
bool recording(GJBaseGameLayer* gl) {
    return opt<bool>("record") && (!typeinfo_cast<LevelEditorLayer*>(gl) || opt<bool>("record-editor"));
}

void sample(GJBaseGameLayer* gl, PlayerObject* pl, int idx) {
    if (!pl) return;
    bool testing = g_test.on;
    if (!testing && !recording(gl)) return;
    if (g_rec.layer != gl) collect(gl);
    auto& R = g_rec;
    if (!pl->m_isDart) {
        R.brk[idx] = true;
        return;
    }
    CCRect rc = pl->getObjectRect();
    float cx = rc.getMidX(), cy = rc.getMidY(), hh = rc.size.height / 2;
    bool mini = pl->m_vehicleSize < .9f;
    if (R.has[idx] && !testing) {
        if (cx < R.lx[idx] - 5) truncate(cx); // the wave never moves left by more than a tick's step
        else if (std::abs(cx - R.lx[idx]) > 40 || std::abs(cy - R.ly[idx]) > 40 || mini != R.mini[idx]) R.brk[idx] = true;
        else if (std::abs(cx - R.lx[idx]) < .001f && std::abs(cy - R.ly[idx]) < .001f) return;
    }
    double x0 = rc.getMinX(), x1 = rc.getMaxX(), win = opt<double>("window") + 60;
    bool baseRight = opt<bool>("base-rises-right");
    uint32_t first = (uint32_t)R.r.obs.size();
    float hitH = hh * (float)g_test.f * .97f;
    for (size_t c = 0; c < R.objs.size(); c++) {
        auto o = R.objs[c];
        if (o->m_isGroupDisabled || o->m_isDisabled || o->m_isNoTouch || o->m_isPassable) continue;
        auto const& b = o->getObjectRect();
        if (b.getMaxX() < x0 || b.getMinX() > x1 || b.getMinY() > cy + win || b.getMaxY() < cy - win) continue;
        slop::V poly[slop::kMaxPoly];
        int n = polyOf(o, b, baseRight, poly);
        double lo, hi;
        if (!slop::spanOver(poly, n, x0, x1, lo, hi)) continue;
        uint8_t type = o->m_objectType == GameObjectType::Hazard ? 0 : o->m_objectType == GameObjectType::Slope ? 2 : 1;
        if (testing) {
            if (type == 0 && lo < cy + hitH && hi > cy - hitH) testFail(cx);
            continue;
        }
        R.r.obs.push_back({(float)lo, (float)hi, type, (int32_t)c});
    }
    if (testing) return;
    addBound(gl, gl->m_groundLayer, cy, hh, true);
    addBound(gl, gl->m_groundLayer2, cy, hh, false);
    R.r.ticks.push_back({cx, cy, hh, (uint8_t)idx, (uint8_t)mini, (uint8_t)R.brk[idx], first, (uint32_t)R.r.obs.size() - first});
    R.brk[idx] = false;
    R.has[idx] = true;
    R.lx[idx] = cx, R.ly[idx] = cy, R.mini[idx] = mini;
}

void onInput(GJBaseGameLayer* gl, bool down, int button, bool p1) {
    if (g_test.on || !recording(gl)) return;
    if (g_rec.layer != gl) collect(gl);
    auto pl = p1 ? gl->m_player1 : gl->m_player2;
    if (pl) g_rec.r.inputs.push_back({pl->getPositionX(), (uint8_t)down, (uint8_t)button, (uint8_t)!p1});
}

// Plays the recorded clicks back by x (x does not depend on inputs, so this is tick exact).
void replayStep(GJBaseGameLayer* gl) {
    auto& T = g_test;
    float px = gl->m_player1->getPositionX();
    if (!T.started) {
        T.started = true;
        while (T.next < T.in.size() && T.in[T.next].x < px - 1) T.next++;
    }
    while (T.next < T.in.size() && T.in[T.next].x <= px + .01f) {
        auto e = T.in[T.next++];
        gl->handleButton(e.down, e.button, !e.p2);
    }
    if (!T.failed && !T.done && px > T.endX) {
        T.done = true;
        stopTestLater();
    }
}

}

class $modify(SlopUI, EditorUI) {
    struct Fields {
        Trace* trace = nullptr;
        bool active = false;
        int mods = -1;
        CCLayerColor* bar = nullptr;
        CCDrawNode* view = nullptr;
        CCLabelBMFont* status = nullptr;
        float btnW = 0;
        slop::Route route;
        std::vector<slop::Section> secs;
        std::vector<double> margin;
        std::vector<char> applied;
        std::vector<int> risk;
        std::vector<std::string> ext; // label of offline pieces per part, empty = own solve
        int cur = 0;
        bool shown = true;
        std::string test;
    };

    CCPoint toLevel(CCTouch* t) { return m_editorLayer->m_objectLayer->convertToNodeSpace(t->getLocation()); }

    float zoom() { return m_editorLayer->m_objectLayer->getScale(); }

    slop::Opts opts(int mods) {
        slop::Opts o;
        o.eps = opt<double>("smoothing") / zoom();
        auto w = opt<std::string>("wave");
        o.wave = w == "Big" ? slop::Wave::Big : w == "Mini" ? slop::Wave::Mini : slop::Wave::Auto;
        o.corridor = mods & 4;
        o.ceiling = mods & 2;
        o.gap = opt<double>("corridor-gap");
        o.snap = opt<double>("start-snap");
        o.baseRight = opt<bool>("base-rises-right");
        o.rot63 = family(m_editorLayer, g_tpl.id).two != 0;
        o.maxPieces = (size_t)opt<int64_t>("max-pieces");
        return o;
    }

    // Whichever changed since the last use wins: build-tab button or a selected slope.
    void pickTemplate() {
        GameObject* sel = m_selectedObject;
        if (!sel && m_selectedObjects && m_selectedObjects->count() == 1)
            sel = static_cast<GameObject*>(m_selectedObjects->objectAtIndex(0));
        if (sel != g_lastSel) {
            g_lastSel = sel;
            if (sel && sizeClass(m_editorLayer, sel->m_objectID))
                g_tpl = {sel->m_objectID, std::string(sel->getSaveString(m_editorLayer).c_str())};
        }
        if (m_selectedObjectIndex != g_lastIndex) {
            g_lastIndex = m_selectedObjectIndex;
            if (sizeClass(m_editorLayer, g_lastIndex)) g_tpl = {g_lastIndex, ""};
        }
        log::debug("build tab {}, selected {}, template {}", m_selectedObjectIndex, sel ? sel->m_objectID : 0, g_tpl.id);
    }

    void refresh(bool force) {
        auto tr = m_fields->trace;
        if (!tr) return;
        int mods = readMods();
        if (!(mods & 1)) {
            tr->finish(true);
            m_fields->trace = nullptr;
            return;
        }
        if (!force && mods == m_fields->mods) return;
        m_fields->mods = mods;
        tr->pieces = slop::build(tr->raw, opts(mods));
        tr->redraw();
    }

    CCArray* place(std::vector<slop::Piece> const& ps) {
        auto fam = family(m_editorLayer, g_tpl.id);
        if (ps.empty() || (!fam.one && !fam.two)) return nullptr;
        int layer = m_editorLayer->m_currentLayer;
        auto extra = stripTemplate(g_tpl.save, layer >= 0);
        int flat = (int)opt<int64_t>("flat-block-id");
        std::string s;
        char buf[160];
        for (auto& p : ps) {
            int id;
            double sx = p.sx, sy = p.sy;
            if (p.kind == slop::Kind::Flat) id = flat;
            else if (p.base == 2 && fam.two) id = fam.two;
            else if (fam.one) id = fam.one;
            else id = fam.two, sx *= .5;
            std::snprintf(buf, sizeof buf, "1,%d,2,%.3f,3,%.3f", id, p.c.x - g_off.x, p.c.y - g_off.y);
            s += buf;
            if (p.fx) s += ",4,1";
            if (p.fy) s += ",5,1";
            if (p.rot) s += ",6," + std::to_string(p.rot);
            if (std::abs(sx - 1) > 1e-4) std::snprintf(buf, sizeof buf, ",128,%.4f", sx), s += buf;
            if (std::abs(sy - 1) > 1e-4) std::snprintf(buf, sizeof buf, ",129,%.4f", sy), s += buf;
            if (layer >= 0) s += ",20," + std::to_string(layer);
            s += extra;
            s += ';';
        }
        auto arr = m_editorLayer->createObjectsFromString(s.c_str(), true, true);
        log::info("template {} ({}/{}), {} pieces -> {} objects, offset {},{}", g_tpl.id, fam.one, fam.two, ps.size(),
                  arr ? arr->count() : 0, g_off.x, g_off.y);
        if (!arr || !arr->count()) return nullptr;
        m_editorLayer->addToUndoList(UndoObject::createWithArray(arr, UndoCommand::Paste), false);
        updateButtons();
        return arr;
    }

    // ---- route bar -------------------------------------------------------------

    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        g_rec = {};
        auto bar = CCLayerColor::create({0, 0, 0, 170}, 10, 34);
        bar->ignoreAnchorPointForPosition(false);
        bar->setAnchorPoint({.5f, 0});
        bar->setID("route-bar"_spr);
        addChild(bar, 100);
        m_fields->bar = bar;
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        bar->addChild(menu);
        float x = 5;
        auto add = [&](char const* text, SEL_MenuHandler sel) {
            auto l = CCLabelBMFont::create(text, "chatFont.fnt");
            l->setScale(.6f);
            auto b = CCMenuItemSpriteExtra::create(l, this, sel);
            b->setPosition({x + b->getContentSize().width / 2, 9});
            x += b->getContentSize().width + 9;
            menu->addChild(b);
        };
        add("<", menu_selector(SlopUI::onRoutePrev));
        add(">", menu_selector(SlopUI::onRouteNext));
        add("-", menu_selector(SlopUI::onRouteMinus));
        add("+", menu_selector(SlopUI::onRoutePlus));
        add("Apply", menu_selector(SlopUI::onRouteApply));
        add("All", menu_selector(SlopUI::onRouteAll));
        add("Test", menu_selector(SlopUI::onRouteTest));
        add("Hide", menu_selector(SlopUI::onRouteHide));
        add("Load", menu_selector(SlopUI::onRouteLoad));
        add("Del", menu_selector(SlopUI::onRouteDel));
        m_fields->btnW = x;
        auto st = CCLabelBMFont::create("", "chatFont.fnt");
        st->setScale(.6f);
        st->setAnchorPoint({0, .5f});
        st->setPosition({5, 26});
        bar->addChild(st);
        m_fields->status = st;
        bar->setPosition({CCDirector::get()->getWinSize().width / 2, m_toolbarHeight + 4});
        auto view = CCDrawNode::create();
        view->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        lel->m_objectLayer->addChild(view, 9998);
        m_fields->view = view;
        loadSections();
        return true;
    }

    slop::Params params(size_t i, bool haveTwo) {
        slop::Params p;
        p.window = opt<double>("window");
        p.tol = opt<double>("fit-tol");
        p.wallScale = opt<double>("wall-scale");
        p.baseRight = opt<bool>("base-rises-right");
        p.margin = i < m_fields->margin.size() ? m_fields->margin[i] : 0;
        p.haveTwo = haveTwo;
        return p;
    }

    void loadSections() {
        m_fields->route = {};
        readRoute(m_editorLayer->m_level, m_fields->route);
        m_fields->secs = slop::split(m_fields->route);
        size_t n = m_fields->secs.size();
        m_fields->margin.assign(n, 0);
        m_fields->applied.assign(n, 0);
        m_fields->risk.assign(n, 0);
        m_fields->cur = std::clamp(m_fields->cur, 0, std::max(0, (int)n - 1));
        g_routeDirty = false;
        bool two = twoGuess();
        for (size_t i = 0; i < n; i++) slop::solve(m_fields->route, m_fields->secs[i], params(i, two));
        slop::separate(m_fields->route, m_fields->secs);
        m_fields->ext.assign(n, "");
        loadExternal();
        scanRisks();
        redrawRoute();
    }

    // Offline pieces replace the own solve for parts they match (same player, same x range).
    // Format: "section <p> <x0> <x1> <label>" then "piece <kind> <base> <cx> <cy> <fx> <fy> <rot>
    // <sx> <sy> <ceil> <tight> <n> <x y>*n" lines.
    void loadExternal() {
        std::ifstream f(externalFile(m_editorLayer->m_level));
        if (!f) return;
        auto& secs = m_fields->secs;
        std::string line;
        int cur = -1, count = 0;
        while (std::getline(f, line)) {
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "section") {
                int p = 0;
                double x0 = 0, x1 = 0;
                in >> p >> x0 >> x1;
                std::string label;
                std::getline(in, label);
                label.erase(0, label.find_first_not_of(' '));
                cur = -1;
                for (size_t i = 0; i < secs.size(); i++) {
                    if (secs[i].p != p || std::abs(secs[i].x0 - x0) > 5 || std::abs(secs[i].x1 - x1) > 5) continue;
                    cur = (int)i;
                    secs[i].pieces.clear();
                    m_fields->ext[i] = label.empty() ? "ext" : label;
                    count++;
                    break;
                }
            } else if (tag == "piece" && cur >= 0) {
                slop::Piece pc;
                int kind = 0, fx = 0, fy = 0, ceil = 0;
                in >> kind >> pc.base >> pc.c.x >> pc.c.y >> fx >> fy >> pc.rot >> pc.sx >> pc.sy >> ceil >> pc.tight >> pc.n;
                pc.kind = (slop::Kind)kind, pc.fx = fx, pc.fy = fy, pc.ceil = ceil;
                pc.n = std::clamp(pc.n, 0, 4);
                for (int k = 0; k < pc.n; k++) in >> pc.poly[k].x >> pc.poly[k].y;
                if (in) secs[cur].pieces.push_back(pc);
            }
        }
        if (count) notify("slop: " + std::to_string(count) + " parts use offline pieces");
    }

    // Things that can make the timing depend on the path itself: speed / teleport portals,
    // collision blocks, touch triggered triggers.
    void scanRisks() {
        static const std::unordered_set<int> ids{200, 201, 202, 203, 1334, 747, 1816};
        auto& secs = m_fields->secs;
        if (secs.empty()) return;
        for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
            bool risky = ids.count(o->m_objectID) > 0;
            if (!risky)
                if (auto e = typeinfo_cast<EffectGameObject*>(o)) risky = e->m_isTouchTriggered;
            if (!risky) continue;
            float x = o->getPositionX();
            for (size_t i = 0; i < secs.size(); i++)
                if (x >= secs[i].x0 - 30 && x <= secs[i].x1 + 30) m_fields->risk[i]++;
        }
    }

    void redrawRoute() {
        auto v = m_fields->view;
        if (!v) return;
        v->clear();
        auto const& secs = m_fields->secs;
        auto const& r = m_fields->route;
        int cur = m_fields->cur;
        if (m_fields->shown) {
            for (size_t i = 0; i < secs.size(); i++) {
                auto const& s = secs[i];
                bool on = (int)i == cur;
                float a = on ? 1.f : .35f;
                for (size_t j = 3; j < s.ti.size(); j += 3) {
                    auto const& p0 = r.ticks[s.ti[j - 3]];
                    auto const& p1 = r.ticks[s.ti[j]];
                    v->drawSegment(ccp(p0.x, p0.y), ccp(p1.x, p1.y), .5f, {1, 1, 1, .6f * a});
                }
                for (auto const& p : s.pieces) {
                    CCPoint q[4];
                    for (int k = 0; k < p.n; k++) q[k] = ccp((float)p.poly[k].x, (float)p.poly[k].y);
                    float t = (float)p.tight;
                    v->drawPolygon(q, p.n, {1, .25f + .75f * t, .25f + .75f * t, on ? .45f : .15f}, .5f, {1, 1, 1, on ? .6f : .2f});
                }
                for (auto const& b : s.bad) v->drawDot(ccp((float)b.x, (float)b.y), 3.f, {1, 0, 1, .9f * a});
                for (size_t c = 0; c < s.clicks.size(); c++) {
                    auto const& t = r.ticks[s.ti[s.clicks[c]]];
                    int w = s.winLo[c] + s.winHi[c];
                    ccColor4F col = w <= 1 ? ccColor4F{1, .2f, .2f, a} : w <= 3 ? ccColor4F{1, .65f, .1f, a} : ccColor4F{1, 1, 1, .7f * a};
                    v->drawDot(ccp(t.x, t.y), 2.2f, col);
                    if (s.winLo[c] != s.genLo[c] || s.winHi[c] != s.genHi[c]) v->drawDot(ccp(t.x, t.y + 7), 1.6f, {1, 0, 1, a});
                }
            }
        }
        std::string st = "no route - play the level (bot / noclip) to record";
        if (!secs.empty()) {
            auto const& s = secs[cur];
            int diff = 0, tight = 0;
            for (size_t c = 0; c < s.clicks.size(); c++) {
                diff += s.winLo[c] != s.genLo[c] || s.winHi[c] != s.genHi[c];
                tight += s.winLo[c] + s.winHi[c] <= 1;
            }
            char buf[200];
            std::snprintf(buf, sizeof buf, "%d/%d %s%s  match %.1f%%  clicks %d  tight %d", cur + 1, (int)secs.size(), s.mini ? "mini" : "big",
                          s.p ? " p2" : "", s.score, (int)s.clicks.size(), tight);
            st = buf;
            if (diff) st += "  diff " + std::to_string(diff);
            int m = (int)m_fields->margin[cur];
            if (m) st += std::string("  gap ") + (m > 0 ? "+" : "") + std::to_string(m);
            if (s.fHaz < .995 || s.fSolid < .995) {
                std::snprintf(buf, sizeof buf, "  hitbox x%.2f", std::min(s.fHaz, s.fSolid));
                st += buf;
            }
            if (m_fields->risk[cur]) st += "  ~" + std::to_string(m_fields->risk[cur]) + " path-dependent";
            if (s.anomalies) st += "  !" + std::to_string(s.anomalies) + " through objects";
            if (!m_fields->ext[cur].empty()) st += "  offline: " + m_fields->ext[cur];
            if (m_fields->applied[cur]) st += "  applied";
        }
        if (!m_fields->test.empty()) st += "  |  " + m_fields->test;
        m_fields->status->setString(st.c_str());
        float w = std::max(m_fields->btnW, m_fields->status->getScaledContentSize().width + 10);
        m_fields->bar->setContentSize({w, 34});
    }

    void go(int d) {
        if (g_routeDirty) loadSections();
        int n = (int)m_fields->secs.size();
        if (!n) return redrawRoute();
        m_fields->cur = ((m_fields->cur + d) % n + n) % n;
        auto c = m_fields->secs[m_fields->cur].mid;
        auto ol = m_editorLayer->m_objectLayer;
        auto win = CCDirector::get()->getWinSize();
        ol->setPosition({win.width / 2 - (float)c.x * ol->getScale(), win.height / 2 - (float)c.y * ol->getScale()});
        redrawRoute();
    }

    void adjust(double d) {
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        int i = m_fields->cur;
        m_fields->ext[i].clear(); // a manual tweak goes back to the own solve
        m_fields->margin[i] = std::clamp(m_fields->margin[i] + d, -20.0, 60.0);
        slop::solve(m_fields->route, m_fields->secs[i], params(i, twoGuess()));
        slop::separate(m_fields->route, m_fields->secs, i);
        redrawRoute();
    }

    // Replaces one wave part: objects that ever reached the wave's column there (and deco around
    // it) are deleted, the slope version is placed. Two undo steps.
    bool applySection(size_t i) {
        pickTemplate();
        auto fam = family(m_editorLayer, g_tpl.id);
        if (!fam.one && !fam.two) {
            notify("slop: pick a slope in the build tab", NotificationIcon::Warning);
            return false;
        }
        auto& s = m_fields->secs[i];
        auto const& r = m_fields->route;
        if (m_fields->ext[i].empty()) {
            slop::solve(r, s, params(i, fam.two != 0));
            slop::separate(r, m_fields->secs, (int)i);
        }
        std::set<std::tuple<int, long, long>> keys;
        for (auto c : s.cands) {
            auto const& k = r.cands[c];
            keys.insert(std::make_tuple(k.id, std::lround(k.x * 4), std::lround(k.y * 4)));
        }
        double ylo = 1e18, yhi = -1e18;
        for (auto const& c : s.chan) ylo = std::min(ylo, c.lo), yhi = std::max(yhi, c.hi);
        ylo -= 90, yhi += 90;
        bool deco = opt<bool>("clean-deco");
        auto del = CCArray::create();
        int kept = 0, matched = 0;
        for (auto o : CCArrayExt<GameObject*>(m_editorLayer->m_objects)) {
            if (o->m_isTrigger || typeinfo_cast<EffectGameObject*>(o)) continue;
            auto t = o->m_objectType;
            auto p = o->getPosition();
            if (t == GameObjectType::Solid || t == GameObjectType::Hazard || t == GameObjectType::Slope) {
                if (!keys.count(std::make_tuple(o->m_objectID, std::lround(p.x * 4), std::lround(p.y * 4)))) continue;
                matched++;
                auto const& b = o->getObjectRect();
                if (b.getMinX() < s.x0 - 45 || b.getMaxX() > s.x1 + 45) {
                    kept++;
                    continue;
                }
                del->addObject(o);
            } else if (deco && (o->m_isDecoration || t == GameObjectType::Decoration)) {
                if (p.x >= s.x0 && p.x <= s.x1 && p.y >= ylo && p.y <= yhi) del->addObject(o);
            }
        }
        deselectAll();
        if (del->count()) {
            m_editorLayer->addToUndoList(UndoObject::createWithArray(del, UndoCommand::DeleteMulti), false);
            for (auto o : CCArrayExt<GameObject*>(del)) m_editorLayer->removeObject(o, true);
        }
        auto placed = place(s.pieces);
        m_fields->applied[i] = 1;
        std::ofstream(appliedMark(m_editorLayer->m_level)) << "applied\n";
        log::info("apply part {}: {} of {} objects matched, {} removed, {} kept (reach outside), {} placed", i + 1, matched, keys.size(),
                  del->count(), kept, placed ? placed->count() : 0);
        if (keys.size() && !matched) notify("slop: recorded objects not found in this level", NotificationIcon::Warning);
        return true;
    }

    void onRoutePrev(CCObject*) { go(-1); }
    void onRouteNext(CCObject*) { go(1); }
    void onRouteMinus(CCObject*) { adjust(-2); }
    void onRoutePlus(CCObject*) { adjust(2); }

    void onRouteApply(CCObject*) {
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        applySection(m_fields->cur);
        redrawRoute();
    }

    void onRouteAll(CCObject*) {
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        for (size_t i = 0; i < m_fields->secs.size(); i++)
            if (!m_fields->applied[i] && !applySection(i)) break;
        redrawRoute();
    }

    void onRouteTest(CCObject*) {
        if (g_routeDirty) loadSections();
        auto const& r = m_fields->route;
        if (r.inputs.empty() || r.ticks.empty()) return notify("slop: the route has no recorded clicks", NotificationIcon::Warning);
        if (m_editorLayer->m_playbackMode != PlaybackMode::Not) return;
        g_test = {};
        g_test.on = true;
        g_test.in = r.inputs;
        float mx = -1e9f;
        for (auto const& t : r.ticks) mx = std::max(mx, t.x);
        g_test.endX = mx + 30;
        for (auto const& s : m_fields->secs) g_test.f = std::min(g_test.f, s.fHaz);
        g_test.ui = this;
        m_fields->test = "testing";
        redrawRoute();
        onPlaytest(nullptr);
    }

    void onRouteHide(CCObject*) {
        m_fields->shown = !m_fields->shown;
        redrawRoute();
    }

    void onRouteLoad(CCObject*) {
        m_fields->test.clear();
        loadSections();
    }

    void onRouteDel(CCObject*) {
        std::error_code ec;
        std::filesystem::remove(routeFile(m_editorLayer->m_level), ec);
        std::filesystem::remove(appliedMark(m_editorLayer->m_level), ec);
        m_fields->test.clear();
        loadSections();
    }

    void onStopPlaytest(CCObject* sender) {
        bool wasTest = g_test.on;
        EditorUI::onStopPlaytest(sender);
        if (wasTest) {
            g_test.on = false;
            std::string res = g_test.done ? "test passed" : "test stopped";
            if (g_test.failed) {
                res = "test: died at x " + std::to_string((int)g_test.failX);
                for (size_t i = 0; i < m_fields->secs.size(); i++)
                    if (g_test.failX >= m_fields->secs[i].x0 - 30 && g_test.failX <= m_fields->secs[i].x1 + 30)
                        res = "test: died in part " + std::to_string(i + 1);
            }
            m_fields->test = res;
            notify("slop: " + res, g_test.failed ? NotificationIcon::Error : NotificationIcon::Success);
            redrawRoute();
        } else if (g_routeDirty) {
            loadSections();
        }
    }

    // ---- drawing -------------------------------------------------------------

    bool ccTouchBegan(CCTouch* t, CCEvent* e) {
        if (!opt<bool>("enabled") || !held(kCtrl) || m_editorLayer->m_playbackMode != PlaybackMode::Not ||
            t->getLocation().y < m_toolbarHeight + 38)
            return EditorUI::ccTouchBegan(t, e);
        pickTemplate();
        m_fields->active = true;
        if (auto fam = family(m_editorLayer, g_tpl.id); !fam.one && !fam.two) {
            notify("slop: pick a slope in the build tab", NotificationIcon::Warning);
            return true;
        }
        auto tr = Trace::create();
        m_editorLayer->m_objectLayer->addChild(tr, 9999);
        auto p = toLevel(t);
        tr->raw.push_back({p.x, p.y});
        tr->tick = [this] { refresh(false); };
        m_fields->trace = tr;
        m_fields->mods = -1;
        refresh(true);
        return true;
    }

    void ccTouchMoved(CCTouch* t, CCEvent* e) {
        if (!m_fields->active) return EditorUI::ccTouchMoved(t, e);
        auto tr = m_fields->trace;
        if (!tr) return;
        auto p = toLevel(t);
        if (std::hypot(p.x - tr->raw.back().x, p.y - tr->raw.back().y) * zoom() < 2) return;
        tr->raw.push_back({p.x, p.y});
        refresh(true);
    }

    void ccTouchEnded(CCTouch* t, CCEvent* e) {
        if (!m_fields->active) return EditorUI::ccTouchEnded(t, e);
        m_fields->active = false;
        auto tr = m_fields->trace;
        if (!tr) return;
        m_fields->trace = nullptr;
        place(tr->pieces);
        tr->finish(false);
    }

    void ccTouchCancelled(CCTouch* t, CCEvent* e) {
        if (!m_fields->active) return EditorUI::ccTouchCancelled(t, e);
        m_fields->active = false;
        if (auto tr = m_fields->trace) tr->finish(true);
        m_fields->trace = nullptr;
    }
};

class $modify(SlopGame, GJBaseGameLayer) {
    int checkCollisions(PlayerObject* p, float dt, bool ignoreDamage) {
        int r = GJBaseGameLayer::checkCollisions(p, dt, ignoreDamage);
        sample(this, p, p == m_player2 ? 1 : 0);
        return r;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        onInput(this, down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_test.on && m_player1 && typeinfo_cast<LevelEditorLayer*>(this)) replayStep(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(SlopPlayer, PlayerObject) {
    void playerDestroyed(bool noEffects) {
        if (g_test.on) testFail(getPositionX());
        PlayerObject::playerDestroyed(noEffects);
    }
};

class $modify(SlopPlay, PlayLayer) {
    void levelComplete() {
        PlayLayer::levelComplete();
        saveRoute(m_level, true);
    }

    void onQuit() {
        saveRoute(m_level, false);
        g_rec = {};
        PlayLayer::onQuit();
    }
};

class $modify(SlopEditor, LevelEditorLayer) {
    void onPlaytest() {
        g_rec = {};
        LevelEditorLayer::onPlaytest();
    }

    void onStopPlaytest() {
        if (!g_test.on) saveRoute(m_level, false);
        g_rec = {};
        LevelEditorLayer::onStopPlaytest();
    }
};
