// webjs - see webjs.h
//
// The split: what has to touch litehtml is native and lives in one JS
// object, __dom (find elements, read and write text, markup and
// attributes, add and remove elements, timers); everything a page
// expects to find on its elements and on document and window -
// textContent as a property, style, classList, addEventListener,
// events that bubble - is written in JavaScript on top of it, in the
// prelude below. That keeps the native side small and the DOM's shape
// where it is easiest to read.

#include "webjs.h"

#include <litehtml/el_text.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

extern "C" {
#include "quickjs.h"
}

using namespace litehtml;

namespace webjs {

struct timer {
    int id;
    long due;                         // in milliseconds of the clock below
};

struct page {
    JSContext* ctx = nullptr;
    document::ptr doc;
    host h;
    std::map<element*, JSValue> wrapped;   // one JS object per element
    std::vector<timer> timers;
    bool dirty = false;               // the tree or its styles changed
    std::map<element*, bool> fresh;   // made by a script, not yet in the tree

    ~page();
};

static JSRuntime* rt;
static JSClassID el_class;
static long deadline;                 // a script running past it stops

static long now_ms()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static page* of(JSContext* ctx)
{
    return (page*)JS_GetContextOpaque(ctx);
}

static void el_finalize(JSRuntime*, JSValue v)
{
    delete (element::ptr*)JS_GetOpaque(v, el_class);
}

// A script that runs on and on is stopped, not waited for: the window
// would stop answering with it
static int interrupt(JSRuntime*, void*)
{
    return deadline && now_ms() > deadline;
}

static JSValue wrap(JSContext* ctx, const element::ptr& el)
{
    if (!el)
        return JS_NULL;
    page* p = of(ctx);
    auto it = p->wrapped.find(el.get());
    if (it != p->wrapped.end())
        return JS_DupValue(ctx, it->second);
    JSValue o = JS_NewObjectClass(ctx, (int)el_class);
    JS_SetOpaque(o, new element::ptr(el));
    p->wrapped[el.get()] = JS_DupValue(ctx, o);
    return o;
}

static element::ptr unwrap(JSValueConst v)
{
    auto* e = (element::ptr*)JS_GetOpaque(v, el_class);
    return e ? *e : nullptr;
}

static std::string str(JSContext* ctx, JSValueConst v)
{
    const char* s = JS_ToCString(ctx, v);
    std::string out = s ? s : "";
    JS_FreeCString(ctx, s);
    return out;
}

static JSValue jstr(JSContext* ctx, const std::string& s)
{
    return JS_NewStringLen(ctx, s.data(), s.size());
}

static std::string escape(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += c == '<' ? std::string("&lt;") : c == '&' ? std::string("&amp;")
                                                         : std::string(1, c);
    return out;
}

// Markup into an element, in place of what it held: parsed as litehtml
// parses a fragment (its children styled), the render tree left to the
// restyle that follows every script
static void set_markup(page* p, const element::ptr& el, const std::string& html)
{
    el->clearRecursive();
    p->doc->append_children_from_string(*el, html.c_str(), false);
    p->dirty = true;
}

// --- __dom: the native half -----------------------------------------------

static JSValue d_by_id(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    std::string id = str(ctx, a[0]);
    page* p = of(ctx);
    for (char c : id)
        if (c == '"' || c == '\\')
            return JS_NULL;
    return wrap(ctx, p->doc->root()->select_one("[id=\"" + id + "\"]"));
}

// query(scope or null, selector, all)
static JSValue d_query(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    page* p = of(ctx);
    element::ptr scope = JS_IsNull(a[0]) ? p->doc->root() : unwrap(a[0]);
    std::string sel = str(ctx, a[1]);
    bool all = JS_ToBool(ctx, a[2]);
    if (!scope)
        return all ? JS_NewArray(ctx) : JS_NULL;
    if (!all)
        return wrap(ctx, scope->select_one(sel));
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (auto& el : scope->select_all(sel))
        JS_SetPropertyUint32(ctx, arr, i++, wrap(ctx, el));
    return arr;
}

static JSValue d_tag(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    if (!el)
        return JS_NULL;
    if (el->is_text())
        return jstr(ctx, "#text");
    std::string t = el->get_tagName();
    for (auto& c : t)
        c = (char)toupper((unsigned char)c);
    return jstr(ctx, t);
}

static JSValue d_attr(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    const char* v = el ? el->get_attr(str(ctx, a[1]).c_str()) : nullptr;
    return v ? jstr(ctx, v) : JS_NULL;
}

static JSValue d_set_attr(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    if (el) {
        el->set_attr(str(ctx, a[1]).c_str(), str(ctx, a[2]).c_str());
        of(ctx)->dirty = true;
    }
    return JS_UNDEFINED;
}

static JSValue d_text(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    std::string t;
    if (el)
        el->get_text(t);
    return jstr(ctx, t);
}

static JSValue d_set_text(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    if (el)
        set_markup(of(ctx), el, escape(str(ctx, a[1])));
    return JS_UNDEFINED;
}

static void markup_of(const element::ptr& el, std::string& out)
{
    for (auto& c : el->children()) {
        if (c->is_text()) {
            std::string t;
            c->get_text(t);
            out += escape(t);
            continue;
        }
        std::string tag = c->get_tagName();
        out += "<" + tag;
        for (const char* name : {"id", "class", "href", "src", "style",
                                 "type", "name", "value"})
            if (const char* v = c->get_attr(name))
                out += std::string(" ") + name + "=\"" + escape(v) + "\"";
        out += ">";
        markup_of(c, out);
        out += "</" + tag + ">";
    }
}

static JSValue d_html(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    std::string out;
    if (el)
        markup_of(el, out);
    return jstr(ctx, out);
}

static JSValue d_set_html(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    if (el)
        set_markup(of(ctx), el, str(ctx, a[1]));
    return JS_UNDEFINED;
}

static JSValue d_parent(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    return el ? wrap(ctx, el->parent()) : JS_NULL;
}

// children(el, with_text): the element children, or all nodes
static JSValue d_children(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    bool text = JS_ToBool(ctx, a[1]);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    if (el)
        for (auto& c : el->children())
            if (text || !c->is_text())
                JS_SetPropertyUint32(ctx, arr, i++, wrap(ctx, c));
    return arr;
}

// append(parent, child, before or null)
static JSValue d_append(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr parent = unwrap(a[0]), child = unwrap(a[1]);
    if (!parent || !child || child == parent)
        return JS_UNDEFINED;
    if (element::ptr old = child->parent())
        old->removeChild(child);
    // An element a script made gets its attributes read now, once - as a
    // parsed page's are, before it is styled
    page* p = of(ctx);
    if (p->fresh.erase(child.get()))
        child->parse_attributes();
    element::ptr before = JS_IsNull(a[2]) ? nullptr : unwrap(a[2]);
    if (!before) {
        parent->appendChild(child);
    } else {
        // litehtml appends only; the ones after 'before' go to the end
        // again behind the new one
        std::vector<element::ptr> after;
        bool seen = false;
        for (auto& c : parent->children()) {
            if (c == before)
                seen = true;
            if (seen)
                after.push_back(c);
        }
        for (auto& c : after)
            parent->removeChild(c);
        parent->appendChild(child);
        for (auto& c : after)
            parent->appendChild(c);
    }
    p->dirty = true;
    return JS_UNDEFINED;
}

static JSValue d_remove(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr child = unwrap(a[0]);
    if (child)
        if (element::ptr parent = child->parent()) {
            parent->removeChild(child);
            of(ctx)->dirty = true;
        }
    return JS_UNDEFINED;
}

// create(tag): an element of the page's own document, its attributes
// parsed when it is given some (setAttribute) or put into the tree
static JSValue d_create(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    page* p = of(ctx);
    std::string tag = str(ctx, a[0]);
    for (auto& c : tag)
        c = (char)tolower((unsigned char)c);
    element::ptr el = p->doc->create_element(tag.c_str(), string_map());
    p->fresh[el.get()] = true;
    return wrap(ctx, el);
}

static JSValue d_create_text(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    page* p = of(ctx);
    return wrap(ctx, std::make_shared<el_text>(str(ctx, a[0]).c_str(), p->doc));
}

static JSValue d_value(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    page* p = of(ctx);
    return el && p->h.get_value ? jstr(ctx, p->h.get_value(el)) : JS_NULL;
}

static JSValue d_set_value(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    page* p = of(ctx);
    if (el && p->h.set_value)
        p->h.set_value(el, str(ctx, a[1]));
    return JS_UNDEFINED;
}

static JSValue d_title(JSContext* ctx, JSValueConst, int argc, JSValueConst* a)
{
    page* p = of(ctx);
    if (argc > 0 && !JS_IsUndefined(a[0])) {
        p->h.set_title(str(ctx, a[0]));
        return JS_UNDEFINED;
    }
    return jstr(ctx, p->h.get_title());
}

static JSValue d_location(JSContext* ctx, JSValueConst, int argc,
                          JSValueConst* a)
{
    page* p = of(ctx);
    if (argc > 0 && !JS_IsUndefined(a[0])) {
        p->h.navigate(p->h.resolve(str(ctx, a[0])));
        return JS_UNDEFINED;
    }
    return jstr(ctx, p->h.location());
}

static JSValue d_say(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    of(ctx)->h.say(str(ctx, a[0]));
    return JS_UNDEFINED;
}

static JSValue d_log(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    std::printf("[js] %s\n", str(ctx, a[0]).c_str());
    std::fflush(stdout);
    return JS_UNDEFINED;
}

// timer(id, ms): __fire(id) is called when it is due
static JSValue d_timer(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    int32_t id = 0, ms = 0;
    JS_ToInt32(ctx, &id, a[0]);
    JS_ToInt32(ctx, &ms, a[1]);
    of(ctx)->timers.push_back({id, now_ms() + (ms > 0 ? ms : 0)});
    return JS_UNDEFINED;
}

static JSValue d_untimer(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    int32_t id = 0;
    JS_ToInt32(ctx, &id, a[0]);
    auto& t = of(ctx)->timers;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i].id == id)
            t.erase(t.begin() + (long)i--);
    return JS_UNDEFINED;
}

// --- the JavaScript half ------------------------------------------------

static const char prelude[] = R"JS(
'use strict';
(function () {
  const D = __dom;
  const E = Element.prototype;
  const kebab = s => s.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
  const def = (o, name, get, set) =>
    Object.defineProperty(o, name, { get, set, configurable: true });

  def(E, 'tagName', function () { return D.tag(this); });
  def(E, 'nodeName', function () { return D.tag(this); });
  def(E, 'nodeType', function () { return D.tag(this) === '#text' ? 3 : 1; });
  def(E, 'id', function () { return D.attr(this, 'id') || ''; },
      function (v) { D.setAttr(this, 'id', String(v)); });
  def(E, 'className', function () { return D.attr(this, 'class') || ''; },
      function (v) { D.setAttr(this, 'class', String(v)); });
  def(E, 'textContent', function () { return D.text(this); },
      function (v) { D.setText(this, String(v)); });
  def(E, 'innerText', function () { return D.text(this); },
      function (v) { D.setText(this, String(v)); });
  def(E, 'innerHTML', function () { return D.html(this); },
      function (v) { D.setHtml(this, String(v)); });
  def(E, 'value', function () { return D.value(this); },
      function (v) { D.setValue(this, String(v)); });
  def(E, 'href', function () { return D.attr(this, 'href') || ''; },
      function (v) { D.setAttr(this, 'href', String(v)); });
  def(E, 'parentNode', function () { return D.parent(this); });
  def(E, 'parentElement', function () { return D.parent(this); });
  def(E, 'children', function () { return D.children(this, false); });
  def(E, 'childNodes', function () { return D.children(this, true); });
  def(E, 'firstElementChild', function () { return D.children(this, false)[0] || null; });
  def(E, 'lastElementChild', function () {
    const c = D.children(this, false); return c[c.length - 1] || null;
  });
  def(E, 'firstChild', function () { return D.children(this, true)[0] || null; });
  E.getAttribute = function (n) { return D.attr(this, n); };
  E.setAttribute = function (n, v) { D.setAttr(this, n, String(v)); };
  E.hasAttribute = function (n) { return D.attr(this, n) !== null; };
  E.removeAttribute = function (n) { D.setAttr(this, n, ''); };
  E.appendChild = function (c) { D.append(this, c, null); return c; };
  E.insertBefore = function (c, b) { D.append(this, c, b || null); return c; };
  E.removeChild = function (c) { D.remove(c); return c; };
  E.remove = function () { D.remove(this); };
  E.querySelector = function (s) { return D.query(this, s, false); };
  E.querySelectorAll = function (s) { return D.query(this, s, true); };
  E.getElementsByTagName = function (t) { return D.query(this, t, true); };
  E.getElementsByClassName = function (c) {
    return D.query(this, c.trim().split(/\s+/).map(x => '.' + x).join(''), true);
  };
  E.matches = function (s) {
    return Array.prototype.includes.call(D.query(null, s, true), this);
  };
  E.closest = function (s) {
    for (let n = this; n; n = D.parent(n)) if (n.matches(s)) return n;
    return null;
  };
  E.contains = function (o) {
    for (let n = o; n; n = D.parent(n)) if (n === this) return true;
    return false;
  };
  E.focus = E.blur = function () {};
  E.click = function () { __dispatch(this, 'click'); };

  // style: an object whose properties write the element's style
  // attribute - read back from it, so that what a page set shows
  def(E, 'style', function () {
    const el = this;
    const read = () => {
      const m = {};
      (D.attr(el, 'style') || '').split(';').forEach(p => {
        const i = p.indexOf(':');
        if (i > 0) m[p.slice(0, i).trim()] = p.slice(i + 1).trim();
      });
      return m;
    };
    return new Proxy({}, {
      get(_, k) {
        if (k === 'cssText') return D.attr(el, 'style') || '';
        if (k === 'setProperty') return (n, v) => { this.set(_, n, v); };
        if (k === 'removeProperty') return n => { this.set(_, n, ''); };
        if (typeof k !== 'string') return undefined;
        return read()[kebab(k)] || '';
      },
      set(_, k, v) {
        if (k === 'cssText') { D.setAttr(el, 'style', String(v)); return true; }
        const m = read();
        const name = k.startsWith('--') ? k : kebab(k);
        if (v === '' || v === null || v === undefined) delete m[name];
        else m[name] = String(v);
        D.setAttr(el, 'style', Object.keys(m).map(n => n + ':' + m[n]).join(';'));
        return true;
      }
    });
  });

  def(E, 'classList', function () {
    const el = this;
    const list = () => (D.attr(el, 'class') || '').split(/\s+/).filter(Boolean);
    const put = l => D.setAttr(el, 'class', l.join(' '));
    return {
      add(...c) { const l = list(); c.forEach(x => l.includes(x) || l.push(x)); put(l); },
      remove(...c) { put(list().filter(x => !c.includes(x))); },
      toggle(c, force) {
        const l = list(), has = l.includes(c);
        const want = force === undefined ? !has : !!force;
        if (want && !has) l.push(c);
        put(want ? l : l.filter(x => x !== c));
        return want;
      },
      contains(c) { return list().includes(c); },
      get length() { return list().length; },
      toString() { return list().join(' '); }
    };
  });

  def(E, 'dataset', function () {
    const el = this;
    return new Proxy({}, {
      get(_, k) { return typeof k === 'string' ? D.attr(el, 'data-' + kebab(k)) || undefined : undefined; },
      set(_, k, v) { D.setAttr(el, 'data-' + kebab(k), String(v)); return true; }
    });
  });

  // Events: listeners kept on the element's own object, and a dispatch
  // that bubbles from the target up to the document
  E.addEventListener = function (t, f) {
    const l = this.__l || (this.__l = {});
    (l[t] = l[t] || []).push(f);
  };
  E.removeEventListener = function (t, f) {
    if (this.__l && this.__l[t]) this.__l[t] = this.__l[t].filter(x => x !== f);
  };

  const docListeners = {}, winListeners = {};
  const listen = l => function (t, f) { (l[t] = l[t] || []).push(f); };
  const unlisten = l => function (t, f) { if (l[t]) l[t] = l[t].filter(x => x !== f); };

  globalThis.__dispatch = function (target, type) {
    const ev = {
      type, target, defaultPrevented: false, bubbles: true, __stop: false,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this.__stop = true; },
      stopImmediatePropagation() { this.__stop = true; }
    };
    const run = (who, f) => {
      try {
        if (f.call(who, ev) === false) ev.preventDefault();
      } catch (e) { __report(e); }
    };
    for (let n = target; n && !ev.__stop; n = D.parent(n)) {
      ev.currentTarget = n;
      let h = n['on' + type];
      const code = D.attr(n, 'on' + type);
      if (typeof h !== 'function' && code) h = new Function('event', code);
      if (typeof h === 'function') run(n, h);
      ((n.__l && n.__l[type]) || []).slice().forEach(f => run(n, f));
    }
    if (!ev.__stop) (docListeners[type] || []).slice().forEach(f => run(document, f));
    return ev.defaultPrevented;
  };

  globalThis.__report = function (e) {
    D.log('error: ' + (e && e.stack ? e + '\n' + e.stack : e));
  };

  globalThis.document = {
    getElementById: id => D.byId(String(id)),
    querySelector: s => D.query(null, s, false),
    querySelectorAll: s => D.query(null, s, true),
    getElementsByTagName: t => D.query(null, t, true),
    getElementsByClassName: c =>
      D.query(null, c.trim().split(/\s+/).map(x => '.' + x).join(''), true),
    getElementsByName: n => D.query(null, '[name="' + n + '"]', true),
    createElement: t => D.create(String(t)),
    createTextNode: t => D.createText(String(t)),
    addEventListener: listen(docListeners),
    removeEventListener: unlisten(docListeners),
    get title() { return D.title(); },
    set title(v) { D.title(String(v)); },
    get body() { return D.query(null, 'body', false); },
    get head() { return D.query(null, 'head', false); },
    get documentElement() { return D.query(null, 'html', false); },
    get readyState() { return __loaded ? 'complete' : 'loading'; },
    get location() { return location; },
    set location(v) { D.location(String(v)); },
    write() { __report('document.write is not supported here'); }
  };
  let __loaded = false;

  const timers = new Map();
  let nextTimer = 1;
  const addTimer = (f, ms, args, repeat) => {
    const id = nextTimer++;
    timers.set(id, { f, ms: Math.max(0, +ms || 0), args, repeat });
    D.timer(id, Math.max(0, +ms || 0));
    return id;
  };
  globalThis.__fire = function (id) {
    const t = timers.get(id);
    if (!t) return;
    if (t.repeat) D.timer(id, Math.max(t.ms, 10)); else timers.delete(id);
    try {
      if (typeof t.f === 'function') t.f(...t.args); else (0, eval)(String(t.f));
    } catch (e) { __report(e); }
  };
  const clear = id => { timers.delete(id); D.untimer(id); };
  globalThis.setTimeout = (f, ms, ...a) => addTimer(f, ms, a, false);
  globalThis.setInterval = (f, ms, ...a) => addTimer(f, ms, a, true);
  globalThis.clearTimeout = clear;
  globalThis.clearInterval = clear;
  globalThis.requestAnimationFrame = f => addTimer(() => f(Date.now()), 16, [], false);
  globalThis.queueMicrotask = f => Promise.resolve().then(f);

  const show = v => typeof v === 'string' ? v :
    (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })();
  globalThis.console = {
    log: (...a) => D.log(a.map(show).join(' ')),
    info: (...a) => D.log(a.map(show).join(' ')),
    warn: (...a) => D.log(a.map(show).join(' ')),
    error: (...a) => D.log(a.map(show).join(' ')),
    debug: (...a) => D.log(a.map(show).join(' '))
  };
  globalThis.alert = m => { D.log('alert: ' + m); D.say(String(m)); };
  globalThis.confirm = m => { D.log('confirm: ' + m); return true; };
  globalThis.prompt = () => null;
  globalThis.location = {
    get href() { return D.location(); },
    set href(v) { D.location(String(v)); },
    assign(v) { D.location(String(v)); },
    replace(v) { D.location(String(v)); },
    reload() { D.location(D.location()); },
    toString() { return D.location(); },
    get hostname() { return (D.location().match(/^\w+:\/\/([^/:]+)/) || [])[1] || ''; },
    get pathname() { return (D.location().match(/^\w+:\/\/[^/]+(\/[^?#]*)/) || [])[1] || '/'; },
    get search() { return (D.location().match(/\?[^#]*/) || [''])[0]; },
    get hash() { return (D.location().match(/#.*/) || [''])[0]; }
  };
  globalThis.navigator = { userAgent: 'ThreadOS Web', language: 'en' };
  globalThis.window = globalThis;
  globalThis.self = globalThis;
  globalThis.addEventListener = listen(winListeners);
  globalThis.removeEventListener = unlisten(winListeners);
  const store = {};
  globalThis.localStorage = {
    getItem: k => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); },
    removeItem: k => { delete store[k]; },
    clear: () => { for (const k in store) delete store[k]; }
  };
  globalThis.__loadedNow = function () {
    __loaded = true;
    for (const [l, who] of [[docListeners, document], [winListeners, globalThis]])
      for (const t of ['DOMContentLoaded', 'load'])
        (l[t] || []).slice().forEach(f => {
          try { f.call(who, { type: t, target: who }); } catch (e) { __report(e); }
        });
    if (typeof globalThis.onload === 'function') {
      try { globalThis.onload({ type: 'load' }); } catch (e) { __report(e); }
    }
  };
})();
)JS";

// Runs code, saying what went wrong. Afterwards the promise jobs it
// queued, and if the tree changed, the browser styles it again.
static void run(page* p, const char* code, size_t len, const char* file)
{
    JSContext* ctx = p->ctx;
    deadline = now_ms() + 4000;
    JSValue r = JS_Eval(ctx, code, len, file, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        std::printf("[js] error: %s%s%s\n", str(ctx, e).c_str(),
                    JS_IsUndefined(stack) ? "" : " ",
                    JS_IsUndefined(stack) ? "" : str(ctx, stack).c_str());
        std::fflush(stdout);
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JSContext* job;
    while (JS_ExecutePendingJob(rt, &job) > 0)
        ;
    deadline = 0;
}

static void settle(page* p)
{
    if (p->dirty) {
        p->dirty = false;
        p->h.changed();
    }
}

page::~page()
{
    if (!ctx)
        return;
    for (auto& w : wrapped)
        JS_FreeValue(ctx, w.second);
    wrapped.clear();
    JS_FreeContext(ctx);
    JS_RunGC(rt);
}

std::shared_ptr<page> start(const document::ptr& doc, const host& h)
{
    if (!rt) {
        // One runtime for the browser; a context for each page
        rt = JS_NewRuntime();
        JS_SetMemoryLimit(rt, 64 << 20);
        JS_SetMaxStackSize(rt, 256 << 10);
        JS_NewClassID(&el_class);
        JSClassDef def = {};
        def.class_name = "Element";
        def.finalizer = el_finalize;
        JS_NewClass(rt, el_class, &def);
    }
    auto p = std::make_shared<page>();
    p->doc = doc;
    p->h = h;
    p->ctx = JS_NewContext(rt);
    JS_SetContextOpaque(p->ctx, p.get());
    JS_SetInterruptHandler(rt, interrupt, nullptr);
    JSContext* ctx = p->ctx;

    JSValue proto = JS_NewObject(ctx);
    JS_SetClassProto(ctx, el_class, JS_DupValue(ctx, proto));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ctor, "prototype", proto);
    JS_SetPropertyStr(ctx, global, "Element", JS_DupValue(ctx, ctor));
    JS_SetPropertyStr(ctx, global, "HTMLElement", ctor);

    struct { const char* name; JSCFunction* fn; int args; } natives[] = {
        {"byId", d_by_id, 1}, {"query", d_query, 3}, {"tag", d_tag, 1},
        {"attr", d_attr, 2}, {"setAttr", d_set_attr, 3}, {"text", d_text, 1},
        {"setText", d_set_text, 2}, {"html", d_html, 1},
        {"setHtml", d_set_html, 2}, {"parent", d_parent, 1},
        {"children", d_children, 2}, {"append", d_append, 3},
        {"remove", d_remove, 1}, {"create", d_create, 1},
        {"createText", d_create_text, 1}, {"value", d_value, 1},
        {"setValue", d_set_value, 2}, {"title", d_title, 1},
        {"location", d_location, 1}, {"say", d_say, 1}, {"log", d_log, 1},
        {"timer", d_timer, 2}, {"untimer", d_untimer, 1},
    };
    JSValue dom = JS_NewObject(ctx);
    for (auto& n : natives)
        JS_SetPropertyStr(ctx, dom, n.name,
                          JS_NewCFunction(ctx, n.fn, n.name, n.args));
    JS_SetPropertyStr(ctx, global, "__dom", dom);
    JS_FreeValue(ctx, global);
    run(p.get(), prelude, sizeof(prelude) - 1, "<prelude>");

    // The page's scripts, in order: inline, or fetched from their src
    int n = 0;
    for (auto& s : doc->root()->select_all("script")) {
        std::string type = s->get_attr("type", "");
        for (auto& c : type)
            c = (char)tolower((unsigned char)c);
        if (!type.empty() && type != "text/javascript" &&
            type != "application/javascript" && type != "module")
            continue;                 // data, templates: not for running
        std::string code, name = "<inline>";
        if (const char* src = s->get_attr("src")) {
            name = h.resolve(src);
            if (!h.fetch(name, code)) {
                std::printf("[js] could not fetch %s\n", name.c_str());
                continue;
            }
        } else {
            s->get_text(code);
        }
        run(p.get(), code.c_str(), code.size(), name.c_str());
        n++;
    }
    const char loaded[] = "__loadedNow()";
    run(p.get(), loaded, sizeof(loaded) - 1, "<load>");
    std::printf("[js] ran %d scripts\n", n);
    std::fflush(stdout);
    settle(p.get());
    return p;
}

bool click(const std::shared_ptr<page>& p, const element::ptr& target)
{
    if (!p || !target)
        return false;
    JSContext* ctx = p->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__dispatch");
    JSValue args[2] = {wrap(ctx, target), JS_NewString(ctx, "click")};
    deadline = now_ms() + 4000;
    JSValue r = JS_Call(ctx, fn, global, 2, args);
    deadline = 0;
    bool prevented = JS_ToBool(ctx, r) > 0;
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    JSContext* job;
    while (JS_ExecutePendingJob(rt, &job) > 0)
        ;
    settle(p.get());
    return prevented;
}

int run_timers(const std::shared_ptr<page>& p)
{
    if (!p)
        return -1;
    long now = now_ms();
    std::vector<int> due;
    for (size_t i = 0; i < p->timers.size(); i++)
        if (p->timers[i].due <= now) {
            due.push_back(p->timers[i].id);
            p->timers.erase(p->timers.begin() + (long)i--);
        }
    for (int id : due) {
        char code[32];
        int n = std::snprintf(code, sizeof(code), "__fire(%d)", id);
        run(p.get(), code, (size_t)n, "<timer>");
    }
    settle(p.get());
    long next = -1;
    for (auto& t : p->timers)
        if (next < 0 || t.due < next)
            next = t.due;
    return next < 0 ? -1 : (int)(next > now ? next - now : 0);
}

}  // namespace webjs
