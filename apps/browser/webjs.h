// webjs - the browser's JavaScript: QuickJS with a page's DOM
//
// A page's scripts run in a QuickJS context of their own, against the
// litehtml tree of that page: the elements are JS objects (one object
// per element, so that === and the properties a script hangs on one
// hold), and what scripts change in the tree - text, markup, attributes,
// classes, styles, elements added and taken away - is styled and laid
// out again when they are done. The rest of the browser sees few
// calls: start a page's scripts, let an event be handled by them, run
// the timers that are due, stop.

#ifndef WEBJS_H
#define WEBJS_H

#include <functional>
#include <memory>
#include <string>
#include <litehtml.h>

namespace webjs {

// What a request a script made got back
struct reply {
    int status = 0;
    std::string type, text;
};

// What the scripts need from the browser
struct host {
    // A script's fetch() or XMLHttpRequest: GET, or POST with a body of
    // that type; false when nothing answered at all
    std::function<bool(const std::string& where, const std::string* body,
                       const std::string& type, reply& r)> request;
    // form.submit(): sent as though its button were pressed, after the
    // script is done
    std::function<void(const litehtml::element::ptr& form)> submit;
    std::function<bool(const std::string& where, std::string& out)> fetch;
    std::function<std::string(const std::string& link)> resolve;
    std::function<void()> changed;            // style and lay out again
    std::function<void(const std::string&)> navigate;
    std::function<void(const std::string&)> say;   // alert(), on the status line
    std::function<std::string()> location;
    std::function<std::string()> get_title;
    std::function<void(const std::string&)> set_title;
    std::function<std::string(const litehtml::element::ptr&)> get_value;
    std::function<void(const litehtml::element::ptr&, const std::string&)> set_value;
    // A box's tick: -1 for what is not a checkbox or a radio button
    std::function<int(const litehtml::element::ptr&)> get_checked;
    std::function<void(const litehtml::element::ptr&, bool)> set_checked;
};

struct page;

// Runs the page's <script>s, in order, and then its load handlers
std::shared_ptr<page> start(const litehtml::document::ptr& doc,
                            const host& h);

// A click on 'target' (the element under the pointer), bubbling up:
// true when a handler called preventDefault()
bool click(const std::shared_ptr<page>& p,
           const litehtml::element::ptr& target);

// Any other event, the same way: input, change, submit, keydown and
// keyup ('key' is what a key event says was pressed, as KeyboardEvent's
// key names it). No target: the page's body.
bool dispatch(const std::shared_ptr<page>& p,
              const litehtml::element::ptr& target, const char* type,
              const std::string& key = "");

// The timers that are due; the milliseconds to the next, or -1
int run_timers(const std::shared_ptr<page>& p);

}  // namespace webjs

#endif
