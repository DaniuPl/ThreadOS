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

static JSValue d_checked(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    page* p = of(ctx);
    int c = el && p->h.get_checked ? p->h.get_checked(el) : -1;
    return c < 0 ? JS_NULL : JS_NewBool(ctx, c);
}

static JSValue d_set_checked(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    page* p = of(ctx);
    if (el && p->h.set_checked)
        p->h.set_checked(el, JS_ToBool(ctx, a[1]) > 0);
    return JS_UNDEFINED;
}

// Where an address's page comes from: scheme, host and port
static std::string origin(const std::string& where)
{
    size_t s = where.find("://");
    if (s == std::string::npos)
        return "";
    size_t e = where.find_first_of("/?#", s + 3);
    std::string o = where.substr(0, e);
    for (auto& c : o)
        c = (char)tolower((unsigned char)c);
    return o;
}

static bool is_http(const std::string& where)
{
    return origin(where).compare(0, 4, "http") == 0;
}

// request(address, method, body, type): what a script's fetch() and
// XMLHttpRequest come down to. Only to the page's own site: elsewhere
// a browser would ask the other site first (CORS) whether this page
// may read its answer - which is the person's, sent with their cookies
// - and without asking, it may not. A page off the disk reads the disk.
static JSValue d_request(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    page* p = of(ctx);
    std::string where = p->h.resolve(str(ctx, a[0]));
    where = where.substr(0, where.find('#'));
    std::string method = str(ctx, a[1]), base = p->h.location();
    for (auto& c : method)
        c = (char)toupper((unsigned char)c);
    bool post = method == "POST";
    if (!post && method != "GET")
        return JS_ThrowTypeError(ctx, "%s requests are not supported",
                                 method.c_str());
    if (where.compare(0, 6, "about:") == 0 ||
        (is_http(base) ? origin(where) != origin(base)
                       : is_http(where) || post)) {
        std::printf("[js] request refused: %s from %s\n", where.c_str(),
                    base.c_str());
        std::fflush(stdout);
        return JS_ThrowTypeError(ctx, "a request to %s from this page is "
                                 "not allowed", where.c_str());
    }
    std::string body = post && !JS_IsNull(a[2]) && !JS_IsUndefined(a[2])
                           ? str(ctx, a[2]) : "";
    std::string type = JS_IsNull(a[3]) || JS_IsUndefined(a[3])
                           ? "" : str(ctx, a[3]);
    reply r;
    long left = deadline - now_ms();   // (the wait is not the script's)
    bool ok = p->h.request && p->h.request(where, post ? &body : nullptr,
                                           type, r);
    deadline = now_ms() + (left > 0 ? left : 0);
    std::printf("[js] %s %s: %d\n", method.c_str(), where.c_str(), r.status);
    std::fflush(stdout);
    if (!ok)
        return JS_ThrowTypeError(ctx, "%s could not be fetched", where.c_str());
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, r.status));
    JS_SetPropertyStr(ctx, o, "type", jstr(ctx, r.type));
    JS_SetPropertyStr(ctx, o, "text", jstr(ctx, r.text));
    JS_SetPropertyStr(ctx, o, "url", jstr(ctx, where));
    return o;
}

static JSValue d_submit(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    element::ptr el = unwrap(a[0]);
    page* p = of(ctx);
    if (el && p->h.submit)
        p->h.submit(el);
    return JS_UNDEFINED;
}

static JSValue d_resolve(JSContext* ctx, JSValueConst, int, JSValueConst* a)
{
    return jstr(ctx, of(ctx)->h.resolve(str(ctx, a[0])));
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
  def(E, 'checked', function () { return !!D.checked(this); },
      function (v) { D.setChecked(this, !!v); });
  def(E, 'name', function () { return D.attr(this, 'name') || ''; },
      function (v) { D.setAttr(this, 'name', String(v)); });
  def(E, 'type', function () { return D.attr(this, 'type') || ''; });
  def(E, 'form', function () { return this.closest('form'); });
  E.focus = E.blur = function () {};
  E.click = function () { __dispatch(this, 'click'); };
  // submit() sends the form as it is; requestSubmit() asks its submit
  // handlers first, as the button would
  E.submit = function () { D.submit(this); };
  E.requestSubmit = function () { if (!__dispatch(this, 'submit')) D.submit(this); };
  E.reset = function () {};

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

  // The codes old scripts read off a key event instead of its name
  const codes = { Enter: 13, Backspace: 8, Tab: 9, Escape: 27, ' ': 32,
    PageUp: 33, PageDown: 34, Home: 36, ArrowLeft: 37, ArrowUp: 38,
    ArrowRight: 39, ArrowDown: 40 };

  globalThis.__dispatch = function (target, type, key) {
    if (!target) target = document.body;
    const ev = {
      type, target, defaultPrevented: false, bubbles: true, __stop: false,
      cancelable: true, timeStamp: Date.now(),
      ctrlKey: false, altKey: false, metaKey: false, shiftKey: false,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this.__stop = true; },
      stopImmediatePropagation() { this.__stop = true; }
    };
    if (key) {
      ev.key = key;
      ev.keyCode = ev.which = codes[key] || key.toUpperCase().charCodeAt(0);
      ev.code = key.length === 1 && /[a-z]/i.test(key) ? 'Key' + key.toUpperCase() : key;
      ev.shiftKey = key.length === 1 && key !== key.toLowerCase();
    }
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
    if (!ev.__stop) {
      ev.currentTarget = document;
      if (typeof document['on' + type] === 'function') run(document, document['on' + type]);
      (docListeners[type] || []).slice().forEach(f => run(document, f));
    }
    if (!ev.__stop) {
      ev.currentTarget = globalThis;
      if (type !== 'load' && typeof globalThis['on' + type] === 'function')
        run(globalThis, globalThis['on' + type]);
      (winListeners[type] || []).slice().forEach(f => run(globalThis, f));
    }
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

  // --- requests: fetch(), XMLHttpRequest, and what they send ----------
  const enc = s => encodeURIComponent(s).replace(/%20/g, '+');
  const dec = s => decodeURIComponent(s.replace(/\+/g, ' '));
  class URLSearchParams {
    constructor(init) {
      this.__p = [];
      if (typeof init === 'string')
        init.replace(/^\?/, '').split('&').filter(Boolean).forEach(kv => {
          const i = kv.indexOf('=');
          this.__p.push(i < 0 ? [dec(kv), ''] : [dec(kv.slice(0, i)), dec(kv.slice(i + 1))]);
        });
      else if (init && typeof init === 'object')
        (Array.isArray(init) ? init : Object.entries(init))
          .forEach(([k, v]) => this.__p.push([String(k), String(v)]));
    }
    append(k, v) { this.__p.push([String(k), String(v)]); }
    set(k, v) { this.delete(k); this.append(k, v); }
    get(k) { const e = this.__p.find(x => x[0] === k); return e ? e[1] : null; }
    getAll(k) { return this.__p.filter(x => x[0] === k).map(x => x[1]); }
    has(k) { return this.__p.some(x => x[0] === k); }
    delete(k) { this.__p = this.__p.filter(x => x[0] !== k); }
    forEach(f) { this.__p.forEach(([k, v]) => f(v, k, this)); }
    entries() { return this.__p.map(x => x.slice())[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this.__p.map(([k, v]) => enc(k) + '=' + enc(v)).join('&'); }
  }
  globalThis.URLSearchParams = URLSearchParams;
  // A form's fields, as the form would send them; sent on as
  // name=value pairs (there is no multipart here)
  class FormData extends URLSearchParams {
    constructor(form) {
      super();
      // (in the order they stand in, as the form sends them)
      const walk = el => D.children(el, false).forEach(c => {
        const tag = D.tag(c).toLowerCase();
        if (tag !== 'input' && tag !== 'select' && tag !== 'textarea') return walk(c);
        const n = D.attr(c, 'name'), t = (D.attr(c, 'type') || '').toLowerCase();
        if (!n || ['submit', 'button', 'image', 'reset'].includes(t)) return;
        if ((t === 'checkbox' || t === 'radio') && !D.checked(c)) return;
        this.append(n, D.value(c) || '');
      });
      if (form) walk(form);
    }
  }
  globalThis.FormData = FormData;

  const headerOf = (h, name) => {
    if (!h) return null;
    if (typeof h.get === 'function') return h.get(name);
    for (const k in h) if (k.toLowerCase() === name) return String(h[k]);
    return null;
  };
  const bodyOf = (body, type) => {
    if (body === undefined || body === null) return [null, type];
    if (body instanceof URLSearchParams)
      return [String(body), type || 'application/x-www-form-urlencoded;charset=UTF-8'];
    return [String(body), type || 'text/plain;charset=UTF-8'];
  };
  const statusText = s => ({ 200: 'OK', 201: 'Created', 204: 'No Content',
    400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found',
    500: 'Internal Server Error' })[s] || '';
  class Headers {
    constructor(type) { this.__t = type || null; }
    get(n) { return String(n).toLowerCase() === 'content-type' ? this.__t : null; }
    has(n) { return this.get(n) !== null; }
    forEach(f) { if (this.__t) f(this.__t, 'content-type', this); }
  }
  globalThis.Headers = Headers;
  const response = r => {
    let used = false;
    const take = () => {
      if (used) return Promise.reject(new TypeError('the body was read already'));
      used = true;
      return Promise.resolve(r.text);
    };
    return {
      ok: r.status >= 200 && r.status < 300, status: r.status,
      statusText: statusText(r.status), url: r.url, redirected: false,
      type: 'basic', headers: new Headers(r.type),
      get bodyUsed() { return used; },
      text: take,
      json: () => take().then(t => JSON.parse(t)),
      clone() { return response(r); }
    };
  };
  // The request waits for its turn - fetch() is never answered before
  // the script that made it is done - and the answer comes as a promise
  globalThis.fetch = function (input, init) {
    init = init || {};
    const where = String(input && typeof input === 'object' && input.url ? input.url : input);
    const method = String(init.method || 'GET');
    const [body, type] = bodyOf(init.body, headerOf(init.headers, 'content-type'));
    return new Promise((ok, fail) => setTimeout(() => {
      try { ok(response(D.request(where, method, body, type))); } catch (e) { fail(e); }
    }, 0));
  };

  class XMLHttpRequest {
    constructor() {
      this.readyState = 0; this.status = 0; this.statusText = '';
      this.responseText = ''; this.response = ''; this.responseType = '';
      this.responseURL = ''; this.__h = {}; this.__l = {};
    }
    open(method, where, async) {
      this.__m = String(method); this.__u = String(where);
      this.__async = async !== false;
      this.readyState = 1;
      this.__fire('readystatechange');
    }
    setRequestHeader(n, v) { this.__h[String(n).toLowerCase()] = String(v); }
    getResponseHeader(n) { return String(n).toLowerCase() === 'content-type' ? this.__type || null : null; }
    getAllResponseHeaders() { return this.__type ? 'content-type: ' + this.__type + '\r\n' : ''; }
    addEventListener(t, f) { (this.__l[t] = this.__l[t] || []).push(f); }
    removeEventListener(t, f) { if (this.__l[t]) this.__l[t] = this.__l[t].filter(x => x !== f); }
    __fire(t) {
      const ev = { type: t, target: this, currentTarget: this };
      const h = [this['on' + t]].concat(this.__l[t] || []);
      h.forEach(f => { if (typeof f === 'function') try { f.call(this, ev); } catch (e) { __report(e); } });
    }
    send(data) {
      const go = () => {
        try {
          const [body, type] = bodyOf(data, this.__h['content-type']);
          const r = D.request(this.__u, this.__m, body, type);
          this.status = r.status; this.statusText = statusText(r.status);
          this.responseText = r.text; this.__type = r.type; this.responseURL = r.url;
          this.response = r.text;
          if (this.responseType === 'json')
            try { this.response = JSON.parse(r.text); } catch (e) { this.response = null; }
          this.readyState = 4;
          this.__fire('readystatechange');
          this.__fire('load');
        } catch (e) {
          this.readyState = 4; this.status = 0;
          this.__fire('readystatechange');
          this.__fire('error');
        }
        this.__fire('loadend');
      };
      if (this.__async) setTimeout(go, 0); else go();
    }
    abort() {}
  }
  Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });
  globalThis.XMLHttpRequest = XMLHttpRequest;
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
        {"checked", d_checked, 1}, {"setChecked", d_set_checked, 2},
        {"request", d_request, 4}, {"submit", d_submit, 1},
        {"resolve", d_resolve, 1},
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
    return target && dispatch(p, target, "click");
}

bool dispatch(const std::shared_ptr<page>& p, const element::ptr& target,
              const char* type, const std::string& key)
{
    if (!p)
        return false;
    JSContext* ctx = p->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__dispatch");
    JSValue args[3] = {target ? wrap(ctx, target) : JS_NULL,
                       JS_NewString(ctx, type), jstr(ctx, key)};
    deadline = now_ms() + 4000;
    JSValue r = JS_Call(ctx, fn, global, 3, args);
    if (JS_IsException(r))
        JS_FreeValue(ctx, JS_GetException(ctx));
    deadline = 0;
    bool prevented = JS_ToBool(ctx, r) > 0;
    JS_FreeValue(ctx, r);
    for (auto& v : args)
        JS_FreeValue(ctx, v);
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
