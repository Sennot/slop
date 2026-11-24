#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <cmath>
#include <cstdio>
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

// 0 = not a slope, 1 = 1x1, 2 = 2x1
int sizeClass(int id) {
    static std::unordered_map<int, int> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    int c = 0;
    if (frames().count(id)) {
        if (auto o = GameObject::createWithKey(id); o && o->m_objectType == GameObjectType::Slope) {
            auto s = o->getContentSize();
            float r = s.height > 0 ? s.width / s.height : 0;
            c = r > 1.5f ? 2 : r > .6f ? 1 : 0;
        }
    }
    return cache[id] = c;
}

struct Family {
    int one = 0, two = 0;
};

// The other size of the same block set: nearby id with the longest shared frame name.
Family family(int id) {
    static std::unordered_map<int, Family> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    int c = sizeClass(id);
    if (!c) return cache[id] = {};
    auto const& name = frames().at(id);
    int best = 0, bestScore = -1;
    for (int d = -12; d <= 12; d++) {
        if (!d || sizeClass(id + d) != 3 - c) continue;
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
bool g_probed = false;
CCPoint g_off;

}

class $modify(SlopUI, EditorUI) {
    struct Fields {
        Trace* trace = nullptr;
        bool active = false;
        int mods = -1;
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
        o.rot63 = family(g_tpl.id).two != 0;
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
            if (sel && sizeClass(sel->m_objectID))
                g_tpl = {sel->m_objectID, std::string(sel->getSaveString(m_editorLayer).c_str())};
        }
        if (m_selectedObjectIndex != g_lastIndex) {
            g_lastIndex = m_selectedObjectIndex;
            if (sizeClass(g_lastIndex)) g_tpl = {g_lastIndex, ""};
        }
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

    void place(std::vector<slop::Piece> const& ps) {
        auto fam = family(g_tpl.id);
        if (ps.empty() || (!fam.one && !fam.two)) return;
        if (!g_probed) {
            // save-string coords vs node coords
            if (auto a = m_editorLayer->createObjectsFromString("1,1,2,0,3,0", true, true); a && a->count()) {
                auto o = static_cast<GameObject*>(a->objectAtIndex(0));
                g_off = o->getPosition();
                m_editorLayer->removeObject(o, true);
                g_probed = true;
            }
        }
        int layer = m_editorLayer->m_currentLayer;
        auto extra = stripTemplate(g_tpl.save, layer >= 0);
        int flat = (int)opt<int64_t>("flat-block-id");
        std::string s;
        char buf[160];
        for (auto& p : ps) {
            int id;
            double sx = p.sx, sy = p.sy;
            if (p.kind == slop::Kind::Flat) id = flat;
            else if (p.kind == slop::Kind::S63) id = p.rot == 90 ? fam.two : fam.one;
            else if (fam.one) id = fam.one;
            else id = fam.two, sx = .5;
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
        if (!arr || !arr->count()) return;
        m_editorLayer->addToUndoList(UndoObject::createWithArray(arr, UndoCommand::Paste), false);
        updateButtons();
    }

    bool ccTouchBegan(CCTouch* t, CCEvent* e) {
        if (!opt<bool>("enabled") || !held(kCtrl) || m_editorLayer->m_playbackMode != PlaybackMode::Not ||
            t->getLocation().y < m_toolbarHeight)
            return EditorUI::ccTouchBegan(t, e);
        pickTemplate();
        auto tr = Trace::create();
        m_editorLayer->m_objectLayer->addChild(tr, 9999);
        auto p = toLevel(t);
        tr->raw.push_back({p.x, p.y});
        tr->tick = [this] { refresh(false); };
        m_fields->trace = tr;
        m_fields->active = true;
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
