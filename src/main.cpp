#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
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

// ---- slope families -------------------------------------------------------

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
    auto& cache = g_fam;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    int c = sizeClass(lel, id);
    if (!c) return cache[id] = {};
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
    return cache[id] = c == 1 ? Family{id, best} : Family{best, id};
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

// ---- route recording -----------------------------------------------------------

constexpr double kCap = 300;

struct Recorder {
    GJBaseGameLayer* layer = nullptr;
    std::vector<GameObject*> cands;
    std::vector<slop::Sample> s;
    bool brk[2] = {true, true}, has[2] = {false, false}, mini[2] = {false, false};
    float lx[2] = {0, 0}, ly[2] = {0, 0};
};

Recorder g_rec;
bool g_routeDirty = true;

std::filesystem::path routeFile(GJGameLevel* level) {
    std::string n = level ? std::string(level->m_levelName.c_str()) : "";
    for (auto& c : n)
        if (!std::isalnum((unsigned char)c)) c = '_';
    auto dir = Mod::get()->getSaveDir() / "routes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / ((n.empty() ? "unnamed" : n) + ".txt");
}

std::vector<slop::Sample> loadRoute(GJGameLevel* level) {
    std::vector<slop::Sample> s;
    std::ifstream f(routeFile(level));
    slop::Sample x;
    int p, mini, brk;
    while (f >> p >> mini >> brk >> x.x >> x.y >> x.hh >> x.up >> x.down) {
        x.p = (unsigned char)p, x.mini = (unsigned char)mini, x.brk = (unsigned char)brk;
        s.push_back(x);
    }
    return s;
}

// The new run replaces the stored route over the x range it covers, so a part can be re-recorded.
void saveRoute(GJGameLevel* level) {
    if (g_rec.s.empty()) return;
    float a = 1e9f, b = -1e9f;
    for (auto& x : g_rec.s) a = std::min(a, x.x), b = std::max(b, x.x);
    auto old = loadRoute(level);
    std::vector<slop::Sample> out;
    for (auto& x : old)
        if (x.x < a) out.push_back(x);
    bool first[2] = {true, true};
    for (auto x : g_rec.s) {
        if (first[x.p]) x.brk = 1, first[x.p] = false;
        out.push_back(x);
    }
    first[0] = first[1] = true;
    for (auto x : old) {
        if (x.x <= b) continue;
        if (first[x.p]) x.brk = 1, first[x.p] = false;
        out.push_back(x);
    }
    std::ofstream f(routeFile(level));
    f.precision(10);
    for (auto& x : out)
        f << (int)x.p << ' ' << (int)x.mini << ' ' << (int)x.brk << ' ' << x.x << ' ' << x.y << ' ' << x.hh << ' ' << x.up << ' ' << x.down << '\n';
    g_rec.s.clear();
    g_routeDirty = true;
    Notification::create("slop: route saved, " + std::to_string(slop::sections(out, kCap).size()) + " wave parts", NotificationIcon::Success)->show();
}

// One physics step: wave hitbox + free space above/below it against everything that can kill
// it right now (moved by triggers, toggled, rotated - whatever the game says at this moment).
void sample(GJBaseGameLayer* gl, PlayerObject* pl, int idx) {
    if (!pl || !opt<bool>("record")) return;
    if (g_rec.layer != gl) {
        g_rec = {};
        g_rec.layer = gl;
        for (auto o : CCArrayExt<GameObject*>(gl->m_objects)) {
            auto t = o->m_objectType;
            if (t == GameObjectType::Solid || t == GameObjectType::Hazard || t == GameObjectType::Slope) g_rec.cands.push_back(o);
        }
    }
    if (!pl->m_isDart) {
        g_rec.brk[idx] = true;
        return;
    }
    auto r = pl->getObjectRect();
    double x0 = r.getMinX(), x1 = r.getMaxX(), y0 = r.getMinY(), y1 = r.getMaxY();
    float cx = r.getMidX(), cy = r.getMidY();
    bool mini = pl->m_vehicleSize < .9f;
    auto& R = g_rec;
    if (R.has[idx]) {
        if (cx < R.lx[idx] - 40) { // respawn: drop what came after the respawn point
            while (!R.s.empty() && R.s.back().x > cx - 1) R.s.pop_back();
            R.brk[0] = R.brk[1] = true;
        } else if (std::abs(cx - R.lx[idx]) > 40 || std::abs(cy - R.ly[idx]) > 40 || mini != R.mini[idx]) {
            R.brk[idx] = true;
        } else if (std::abs(cx - R.lx[idx]) < .01f && std::abs(cy - R.ly[idx]) < .01f) {
            return;
        }
    }
    double up = kCap, down = kCap;
    bool baseRight = opt<bool>("base-rises-right");
    for (auto o : R.cands) {
        if (o->m_isGroupDisabled || o->m_isDisabled || o->m_isNoTouch || o->m_isPassable) continue;
        auto const& b = o->getObjectRect();
        if (b.getMaxX() < x0 || b.getMinX() > x1 || b.getMinY() > y1 + kCap || b.getMaxY() < y0 - kCap) continue;
        slop::V poly[4];
        int n = 4;
        float rot = o->getRotation(), m90 = std::fmod(std::abs(rot), 90.f);
        if (o->m_objectType == GameObjectType::Slope) {
            int r90 = ((int)std::lround(rot / 90) % 4 + 4) % 4;
            slop::slopeTri(b.getMidX(), b.getMidY(), b.size.width, b.size.height, o->m_isFlipX, o->m_isFlipY, r90 * 90, baseRight, poly);
            n = 3;
        } else if (m90 > .5f && m90 < 89.5f && o->m_orientedBox) {
            for (int i = 0; i < 4; i++) poly[i] = {o->m_orientedBox->m_corners[i].x, o->m_orientedBox->m_corners[i].y};
        } else {
            poly[0] = {b.getMinX(), b.getMinY()};
            poly[1] = {b.getMaxX(), b.getMinY()};
            poly[2] = {b.getMaxX(), b.getMaxY()};
            poly[3] = {b.getMinX(), b.getMaxY()};
        }
        slop::clearance(poly, n, x0, x1, y0, y1, up, down);
    }
    R.s.push_back({cx, cy, (float)(y1 - y0) / 2, (float)up, (float)down, (unsigned char)idx, (unsigned char)mini, (unsigned char)R.brk[idx]});
    R.brk[idx] = false;
    R.has[idx] = true;
    R.lx[idx] = cx, R.ly[idx] = cy, R.mini[idx] = mini;
}

}

class $modify(SlopUI, EditorUI) {
    struct Fields {
        Trace* trace = nullptr;
        bool active = false;
        int mods = -1;
        CCDrawNode* view = nullptr;
        CCLabelBMFont* status = nullptr;
        std::vector<slop::Section> secs;
        std::vector<std::vector<slop::Piece>> pcs;
        std::vector<double> margin;
        std::vector<Ref<CCArray>> placed;
        int cur = 0;
        bool shown = true;
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

    // Whichever changed since the last stroke wins: build-tab button or a selected slope.
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
            if (sx != 1) std::snprintf(buf, sizeof buf, ",128,%.4f", sx), s += buf;
            if (sy != 1) std::snprintf(buf, sizeof buf, ",129,%.4f", sy), s += buf;
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
        auto bar = CCLayerColor::create({0, 0, 0, 170}, 10, 18);
        bar->ignoreAnchorPointForPosition(false);
        bar->setAnchorPoint({.5f, 0});
        bar->setID("route-bar"_spr);
        addChild(bar, 100);
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        bar->addChild(menu);
        float x = 5;
        auto add = [&](char const* text, SEL_MenuHandler sel) {
            auto l = CCLabelBMFont::create(text, "chatFont.fnt");
            l->setScale(.6f);
            auto b = CCMenuItemSpriteExtra::create(l, this, sel);
            b->setPosition({x + b->getContentSize().width / 2, 9});
            x += b->getContentSize().width + 8;
            menu->addChild(b);
        };
        add("<", menu_selector(SlopUI::onRoutePrev));
        auto st = CCLabelBMFont::create("", "chatFont.fnt");
        st->setScale(.6f);
        st->setAnchorPoint({0, .5f});
        st->setPosition({x, 9});
        bar->addChild(st);
        m_fields->status = st;
        x += 105;
        add(">", menu_selector(SlopUI::onRouteNext));
        add("-", menu_selector(SlopUI::onRouteMinus));
        add("+", menu_selector(SlopUI::onRoutePlus));
        add("Place", menu_selector(SlopUI::onRoutePlace));
        add("All", menu_selector(SlopUI::onRouteAll));
        add("Hide", menu_selector(SlopUI::onRouteHide));
        add("Del", menu_selector(SlopUI::onRouteDel));
        bar->setContentSize({x, 18});
        bar->setPosition({CCDirector::get()->getWinSize().width / 2, m_toolbarHeight + 4});
        auto view = CCDrawNode::create();
        view->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        lel->m_objectLayer->addChild(view, 9998);
        m_fields->view = view;
        loadSections();
        return true;
    }

    void loadSections() {
        m_fields->secs = slop::sections(loadRoute(m_editorLayer->m_level), opt<double>("max-gap"));
        size_t n = m_fields->secs.size();
        m_fields->margin.assign(n, 0);
        m_fields->placed.clear();
        m_fields->placed.resize(n);
        m_fields->cur = std::clamp(m_fields->cur, 0, std::max(0, (int)n - 1));
        g_routeDirty = false;
        // the family is only known once probed in the level; assume a 2x1 exists until then
        auto it = g_fam.find(g_tpl.id);
        bool two = it == g_fam.end() || it->second.two;
        m_fields->pcs.assign(n, {});
        for (size_t i = 0; i < n; i++) slop::walls(m_fields->secs[i], 0, two, opt<bool>("base-rises-right"), m_fields->pcs[i]);
        redrawRoute();
    }

    bool isPlaced(size_t i) {
        if (i >= m_fields->placed.size() || !m_fields->placed[i]) return false;
        for (auto o : CCArrayExt<GameObject*>(m_fields->placed[i].data()))
            if (o->getParent()) return true;
        return false;
    }

    void redrawRoute() {
        auto v = m_fields->view;
        if (!v) return;
        v->clear();
        auto const& secs = m_fields->secs;
        int cur = m_fields->cur;
        if (m_fields->shown) {
            for (size_t i = 0; i < secs.size(); i++) {
                bool on = (int)i == cur;
                auto const& P = secs[i].path;
                for (size_t j = 1; j < P.size(); j++)
                    v->drawSegment(ccp(P[j - 1].x, P[j - 1].y), ccp(P[j].x, P[j].y), .75f, {1, 1, 1, on ? .9f : .35f});
                for (auto const& p : m_fields->pcs[i]) {
                    CCPoint q[4];
                    for (int k = 0; k < p.n; k++) q[k] = ccp(p.poly[k].x, p.poly[k].y);
                    float t = (float)p.tight;
                    v->drawPolygon(q, p.n, {1, .25f + .75f * t, .25f + .75f * t, on ? .45f : .15f}, .5f, {1, 1, 1, on ? .6f : .2f});
                }
            }
        }
        std::string s = "no route";
        if (!secs.empty()) {
            auto const& sec = secs[cur];
            int m = (int)m_fields->margin[cur];
            s = std::to_string(cur + 1) + "/" + std::to_string(secs.size()) + (sec.mini ? " mini" : " big") + (sec.p ? " p2" : "") +
                " gap " + (m >= 0 ? "+" : "") + std::to_string(m) + (isPlaced(cur) ? " ok" : "");
        }
        m_fields->status->setString(s.c_str());
    }

    void go(int d) {
        if (g_routeDirty) loadSections();
        int n = (int)m_fields->secs.size();
        if (!n) return;
        m_fields->cur = ((m_fields->cur + d) % n + n) % n;
        auto c = m_fields->secs[m_fields->cur].mid;
        auto ol = m_editorLayer->m_objectLayer;
        auto win = CCDirector::get()->getWinSize();
        ol->setPosition({win.width / 2 - (float)c.x * ol->getScale(), win.height / 2 - (float)c.y * ol->getScale()});
        redrawRoute();
    }

    void adjust(double d) {
        int i = m_fields->cur;
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        m_fields->margin[i] = std::clamp(m_fields->margin[i] + d, -30.0, 300.0);
        auto it = g_fam.find(g_tpl.id);
        m_fields->pcs[i].clear();
        slop::walls(m_fields->secs[i], m_fields->margin[i], it == g_fam.end() || it->second.two, opt<bool>("base-rises-right"), m_fields->pcs[i]);
        redrawRoute();
    }

    void placeSection(size_t i) {
        pickTemplate();
        auto fam = family(m_editorLayer, g_tpl.id);
        if (!fam.one && !fam.two) {
            Notification::create("slop: pick a slope in the build tab", NotificationIcon::Warning)->show();
            return;
        }
        std::vector<slop::Piece> ps;
        slop::walls(m_fields->secs[i], m_fields->margin[i], fam.two != 0, opt<bool>("base-rises-right"), ps);
        m_fields->pcs[i] = ps;
        if (auto arr = place(ps)) m_fields->placed[i] = arr;
    }

    void onRoutePrev(CCObject*) { go(-1); }
    void onRouteNext(CCObject*) { go(1); }
    void onRouteMinus(CCObject*) { adjust(-2); }
    void onRoutePlus(CCObject*) { adjust(2); }

    void onRoutePlace(CCObject*) {
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        placeSection(m_fields->cur);
        redrawRoute();
    }

    void onRouteAll(CCObject*) {
        if (g_routeDirty || m_fields->secs.empty()) return go(0);
        for (size_t i = 0; i < m_fields->secs.size(); i++)
            if (!isPlaced(i)) placeSection(i);
        redrawRoute();
    }

    void onRouteHide(CCObject*) {
        m_fields->shown = !m_fields->shown;
        redrawRoute();
    }

    void onRouteDel(CCObject*) {
        std::error_code ec;
        std::filesystem::remove(routeFile(m_editorLayer->m_level), ec);
        loadSections();
    }

    void onStopPlaytest(CCObject* sender) {
        EditorUI::onStopPlaytest(sender);
        if (g_routeDirty) loadSections();
    }

    bool ccTouchBegan(CCTouch* t, CCEvent* e) {
        if (!opt<bool>("enabled") || !held(kCtrl) || m_editorLayer->m_playbackMode != PlaybackMode::Not ||
            t->getLocation().y < m_toolbarHeight)
            return EditorUI::ccTouchBegan(t, e);
        pickTemplate();
        m_fields->active = true;
        if (auto fam = family(m_editorLayer, g_tpl.id); !fam.one && !fam.two) {
            Notification::create("slop: pick a slope in the build tab", NotificationIcon::Warning)->show();
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
};

class $modify(SlopPlay, PlayLayer) {
    void levelComplete() {
        PlayLayer::levelComplete();
        saveRoute(m_level);
    }

    void onQuit() {
        saveRoute(m_level);
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
        saveRoute(m_level);
        g_rec = {};
        LevelEditorLayer::onStopPlaytest();
    }
};
