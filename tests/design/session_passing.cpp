// Evidence for docs/cpp-wrapper/DESIGN.md §5: `with` hands the user callback
// a prvalue Session (non-copyable, non-movable).
//
// Checks, built by tests/design/run.sh with each compiler it finds:
//   (default)  the accepted parameter forms compile, and ~Session (the
//              handback) has run before `with` executes its next statement
//   -DBAD_REF       `auto& s`         must NOT compile (prvalue doesn't bind to a non-const lvalue ref)
//   -DBAD_SESSREF   `Session& s`      must NOT compile (same)
//   -DBAD_RETURN    `return s;`       must NOT compile (a Session can't escape the callback)
//   -DBAD_CONST_OP  `const auto s` then a scan op must NOT compile (scan ops are non-const)
#include <cassert>
#include <cstdio>
#include <utility>

struct Handback { bool done = false; };

class Session {
public:
    Session(int id, Handback& hb) : id_(id), hb_(&hb) {}
    Session(const Session&) = delete;
    Session(Session&&) = delete;
    ~Session() { hb_->done = true; }          // the real one calls aji_unlock_lock_chain and records errors in hb

    int  dr()         { return id_; }           // scan ops are non-const: they change library/session state
    int  id() const   { return id_; }

private:
    int id_;
    Handback* hb_;
};

// The chosen shape: the callback gets a prvalue. Whether the parameter is
// destroyed when the callee returns or at the end of this full-expression is
// implementation-defined ([expr.call]); both happen before the next statement
// of `with`, so `hb.done` is always set by the time we read it.
template<class F>
auto with(F&& f)
{
    Handback hb;
    auto r = std::forward<F>(f)(Session{1, hb});
    assert(hb.done && "handback must have run before with() continues");
    std::printf("  handback done before with() continued: %s\n", hb.done ? "yes" : "no");
    return r;
}

int main()
{
    std::puts("accepted forms:");
    with([](auto s)              { return s.dr(); });   // recommended
    with([](auto&& s)            { return s.dr(); });
    with([](Session s)           { return s.dr(); });
    with([](Session&& s)         { return s.dr(); });
    with([](const Session& s)    { return s.id(); });   // read-only: no scan ops
    with([](const auto s)        { return s.id(); });   // read-only: no scan ops

#ifdef BAD_REF
    with([](auto& s)             { return s.dr(); });
#endif
#ifdef BAD_SESSREF
    with([](Session& s)          { return s.dr(); });
#endif
#ifdef BAD_RETURN
    with([](Session s)           { return s; });
#endif
#ifdef BAD_CONST_OP
    with([](const auto s)        { return s.dr(); });
#endif
}
