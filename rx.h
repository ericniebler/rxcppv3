#pragma once

#include <sstream>

namespace rx {
    
// based on Walter Brown's void_t proposal
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n3911.pdf
namespace detail {
    template<class... TN> struct void_type {typedef void type;};
}
template<class... TN>
using void_t = typename detail::void_type<TN...>::type;

namespace shapes {

template<class Payload>
struct state;

template<class Payload, class Clock>
struct context;

struct subscription
{
    bool is_stopped();
    void stop();

    void insert(const subscription& s);
    void erase(const subscription& s);

    void insert(function<void()> stopper);

    template<class Payload, class... ArgN>
    state<Payload> make_state(ArgN... argn);
    template<class Payload>
    state<Payload> copy_state(const state<Payload>&);
};

template<class Payload>
struct state {
    subscription lifetime;
    Payload& get();
};

struct starter {
    template<class Payload, class Clock>
    subscription start(context<Payload, Clock>);
};

struct observer {
    template<class T>
    void next(T);
    template<class E>
    void error(E);
    void complete();
};

struct subscriber {
    template<class Payload, class Clock>
    observer create(context<Payload, Clock>);
};

struct observable {
    starter bind(subscriber);
};

struct lifter {
    subscriber lift(subscriber);
};

struct adaptor {
    observable adapt(observable);
};

struct terminator {
    starter terminate(observable);
};

template<class Clock>
struct strand {
    subscription lifetime;

    typename Clock::time_point now();
    void defer_at(typename Clock::time_point, observer);
};

template<class Clock>
void defer(strand<Clock>, observer);
template<class Clock>
void defer_at(strand<Clock>, typename Clock::time_point, observer);
template<class Clock>
void defer_after(strand<Clock>, typename Clock::duration, observer);
template<class Clock>
void defer_periodic(strand<Clock>, typename Clock::time_point, typename Clock::duration, observer);

template<class Payload, class Clock>
struct context {
    subscription lifetime;
    
    typename Clock::time_point now();
    void defer_at(typename Clock::time_point, observer);

    Payload& get();
};

template<class Payload, class Clock>
void defer(context<Payload, Clock>, observer);
template<class Payload, class Clock>
void defer_at(context<Payload, Clock>, typename Clock::time_point, observer);
template<class Payload, class Clock>
void defer_after(context<Payload, Clock>, typename Clock::duration, observer);
template<class Payload, class Clock>
void defer_periodic(context<Payload, Clock>, typename Clock::time_point, typename Clock::duration, observer);

}

using std::decay_t;

namespace detail {

/// test type against a model (no match)
template<class T, template<class...> class M>
struct is_specialization_of : public false_type {};

/// test type against a model (matches)
template<template<class...> class M, class... TN>
struct is_specialization_of<M<TN...>, M> : public true_type {};

/// enabled if T is matched 
template<class T, template<class...> class M>
using for_specialization_of_t = enable_if_t<is_specialization_of<decay_t<T>, M>::value>;

/// enabled if T is not a match 
template<class T, template<class...> class M>
using not_specialization_of_t = enable_if_t<!is_specialization_of<decay_t<T>, M>::value>;

/// enabled if T is same 
template<class T, class M>
using for_same_t = enable_if_t<is_same<decay_t<T>, decay_t<M>>::value>;

/// enabled if T is not same 
template<class T, class M>
using not_same_t = enable_if_t<!is_same<decay_t<T>, decay_t<M>>::value>;

}

template<class T>
using payload_t = typename decay_t<T>::payload_type;

template<class Payload = void>
struct state;

///
/// \brief A subscription represents the scope of an async operation. 
/// Holds a set of nested lifetimes. 
/// Can be used to make state that is scoped to the subscription. 
/// Can call arbitratry functions at the end of the lifetime.
///
struct subscription
{
private:
    struct shared
    {
        ~shared(){
            auto expired = std::move(destructors);
            for (auto& d : expired) {
                d();
            }
        }
        shared() : stopped(false) {info("new lifetime");}
        bool stopped;
        set<subscription> others;
        deque<function<void()>> stoppers;
        deque<function<void()>> destructors;
    };
public:
    subscription() : store(make_shared<shared>()) {}
    explicit subscription(shared_ptr<shared> o) : store(o) {}
    /// \brief used to exit loops or otherwise stop work scoped to this subscription.
    /// \returns bool - if true do not access any state objects.
    bool is_stopped() const {
        return !store || store->stopped;
    }
    /// \brief 
    void insert(const subscription& s) const {
        if (is_stopped()) {
            s.stop();
            return;
        }
        if (s == *this) {std::abort();}
        // nest
        store->others.insert(s);
        // unnest when child is stopped
        weak_ptr<shared> p = store;
        weak_ptr<shared> c = s.store;
        s.insert([p, c](){
            auto storep = p.lock();
            auto storec = c.lock();
            if (storep && storec) {
                auto that = subscription(storep);
                auto s = subscription(storec);
                that.erase(s);
            }
        });
    }
    /// \brief 
    void erase(const subscription& s) const {
        if (is_stopped()) {
            return;
        }
        if (s == *this) {std::abort();}
        store->others.erase(s);
    }
    /// \brief 
    void insert(function<void()> stopper) const {
        if (is_stopped()) {
            stopper();
            return;
        }
        store->stoppers.emplace_front(stopper);
        if (store->stopped) stop();
    }
    /// \brief 
    template<class Payload, class... ArgN>
    state<Payload> make_state(ArgN&&... argn) const;
    /// \brief 
    state<> make_state() const;
    /// \brief 
    state<> copy_state(const state<>&) const;
    /// \brief 
    template<class Payload>
    state<Payload> copy_state(const state<Payload>&) const;
    /// \brief 
    void stop() const {
        if (is_stopped()) {
            return;
        }
        auto st = move(store);
        st->stopped = true;
        {
            auto others = std::move(st->others);
            for (auto& o : others) {
                o.stop();
            }
        }
        {
            auto stoppers = std::move(st->stoppers);
            for (auto& s : stoppers) {
                s();
            }
        }
    }
private:
    shared_ptr<shared> store;
    friend bool operator==(const subscription&, const subscription&);
    friend bool operator<(const subscription&, const subscription&);
};
bool operator==(const subscription& lhs, const subscription& rhs) {
    return lhs.store == rhs.store;
}
bool operator!=(const subscription& lhs, const subscription& rhs) {
    return !(lhs == rhs);
}
bool operator<(const subscription& lhs, const subscription& rhs) {
    return lhs.store < rhs.store;
}

namespace detail {

template<class T>
using for_subscription = for_same_t<T, subscription>;

template<class T>
using not_subscription = not_same_t<T, subscription>;

}

template<>
struct state<void>
{
    using payload_type = void;
    subscription lifetime;
    explicit state(subscription lifetime) 
        : lifetime(lifetime) {
    }
    state(const state&) = default;
    template<class Payload>
    state(const state<Payload>& o)
        : lifetime(o.lifetime) {
    }
};

template<class Payload>
struct state
{
    using payload_type = decay_t<Payload>;
    subscription lifetime;
    state(subscription l, Payload* p) : lifetime(l), p(p) {}
    Payload& get() {
        return *p;
    }
    Payload& get() const {
        return *p;
    }
    explicit operator state<>(){
        return {lifetime};
    }
private:
    mutable Payload* p;
};

class lifetime_error : public logic_error {
public:
  explicit lifetime_error (const string& what_arg) : logic_error(what_arg) {}
  explicit lifetime_error (const char* what_arg) : logic_error(what_arg) {}
};

template<class Payload, class... ArgN>
state<Payload> subscription::make_state(ArgN&&... argn) const {
    if (is_stopped()) {
        throw lifetime_error("subscription is stopped!");
    }
    auto p = make_unique<Payload>(forward<ArgN>(argn)...);
    auto result = state<Payload>{*this, p.get()};
    store->destructors.emplace_front(
        [d=p.release()]() mutable {
            auto p = d; 
            d = nullptr; 
            delete p;
        });
    return result;
}
state<> subscription::make_state() const {
    if (is_stopped()) {
        throw lifetime_error("subscription is stopped!");
    }
    auto result = state<>{*this};
    return result;
}

state<> subscription::copy_state(const state<>&) const{
    if (is_stopped()) {
        throw lifetime_error("subscription is stopped!");
    }
    return make_state();
}

template<class Payload>
state<Payload> subscription::copy_state(const state<Payload>& o) const{
    if (is_stopped()) {
        throw lifetime_error("subscription is stopped!");
    }
    return make_state<Payload>(o.get());
}

template<class Payload, class... ArgN>
state<Payload> make_state(subscription lifetime, ArgN... argn) {
    return lifetime.template make_state<Payload>(forward<ArgN>(argn)...);
}
inline state<> make_state(subscription lifetime) {
    return lifetime.make_state();
}

state<> copy_state(subscription lifetime, const state<>&) {
    return lifetime.make_state();
}

template<class Payload>
state<Payload> copy_state(subscription lifetime, const state<Payload>& o) {
    return lifetime.template make_state<Payload>(o.get());
}

namespace detail {

template<class T>
using for_state = for_specialization_of_t<T, state>;

template<class T>
using not_state = not_specialization_of_t<T, state>;

}

/// selects default implementation
struct defaults {};
/// selects interface implementation
template<class... TN>
struct interface {};

template<class Select = defaults, class... ON>
struct observer;

namespace detail {
    
template<class T>
struct observer_check : public false_type {};

template<class... ON>
struct observer_check<observer<ON...>> : public true_type {};

template<class T>
using for_observer = enable_if_t<observer_check<decay_t<T>>::value>;

template<class T>
using not_observer = enable_if_t<!observer_check<decay_t<T>>::value>;

auto report = [](auto&& e, auto&& f, auto&&... args){
    try{f(args...);} catch(...) {e(current_exception());}
};

auto enforce = [](const subscription& lifetime, auto&& f) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) f(args...);
    };
};

auto end = [](const subscription& lifetime, auto&& f, auto&&... cap) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) { 
            f(cap..., args...); 
            lifetime.stop();
        }
    };
};

struct noop
{
    template<class V, class CheckV = not_observer<V>>
    void operator()(V&&) const {
    }
    template<class Delegatee, class V>
    void operator()(const Delegatee& d, V&& v) const {
        d.next(std::forward<V>(v));
    }
    inline void operator()() const {
    }
    template<class Delegatee, class Check = for_observer<Delegatee>>
    void operator()(const Delegatee& d) const {
        d.complete();
    }
};
struct ignore
{
    template<class E>
    void operator()(E&&) const {
    }
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee& d, E&& e) const {
        d.error(forward<E>(e));
    }
};
struct pass
{
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee& d, E&& e) const {
        d.error(forward<E>(e));
    }
    template<class Delegatee, class Check = for_observer<Delegatee>>
    void operator()(const Delegatee& d) const {
        d.complete();
    }
};
struct skip
{
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee& d, E&& e) const {
    }
    template<class Delegatee, class Check = for_observer<Delegatee>>
    void operator()(const Delegatee& d) const {
    }
};
struct fail
{
    template<class E>
    void operator()(E&&) const {
        info("abort! ");
        std::abort();
    }
    template<class Delegatee, class E, class CheckD = for_observer<Delegatee>>
    void operator()(const Delegatee&, E&&) const {
        info("abort! ");
        std::abort();
    }
};

}

namespace detail{
    template<class V, class E>
    struct abstract_observer
    {
        virtual ~abstract_observer(){}
        virtual void next(const V&) const = 0;
        virtual void error(const E&) const = 0;
        virtual void complete() const = 0;
    };

    template<class V, class E, class... ON>
    struct basic_observer : public abstract_observer<V, E> {
        using value_type = decay_t<V>;
        using errorvalue_type = decay_t<E>;
        basic_observer(const observer<ON...>& o)
            : d(o){
        }
        observer<ON...> d;
        virtual void next(const value_type& v) const {
            d.next(v);
        }
        virtual void error(const errorvalue_type& err) const {
            d.error(err);
        }
        virtual void complete() const {
            d.complete();
        }
    };
}
template<class V, class E>
struct observer<interface<V, E>> {
    using value_type = decay_t<V>;
    using errorvalue_type = decay_t<E>;
    observer(const observer& o) = default;
    template<class... ON>
    observer(const observer<ON...>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_observer<V, E, ON...>>(o)) {
    }
    subscription lifetime;
    shared_ptr<detail::abstract_observer<value_type, errorvalue_type>> d;
    void next(const value_type& v) const {
        d->next(v);
    }
    void error(const errorvalue_type& err) const {
        d->error(err);
    }
    void complete() const {
        d->complete();
    }
    template<class... TN>
    observer as_interface() const {
        return {*this};
    }
};
template<class V, class E>
using observer_interface = observer<interface<V, E>>;


template<class Next, class Error, class Complete>
struct observer<Next, Error, Complete> {
    subscription lifetime;
    mutable Next n;
    mutable Error e;
    mutable Complete c;
    template<class V>
    void next(V&& v) const {
        using namespace detail;
        report(end(lifetime, e), enforce(lifetime, n), std::forward<V>(v));
    }
    template<class E>
    void error(E&& err) const {
        using namespace detail;
        report(fail{}, end(lifetime, e), std::forward<E>(err));
    }
    void complete() const {
        using namespace detail;
        report(fail{}, end(lifetime, c));
    }
    template<class V, class E = exception_ptr>
    observer_interface<V, E> as_interface() const {
        using observer_t = detail::basic_observer<V, E, Next, Error, Complete>;
        return {lifetime, make_shared<observer_t>(*this)};
    }
};
template<class Delegatee, class Next, class Error, class Complete>
struct observer<Delegatee, Next, Error, Complete> {
    Delegatee d;
    subscription lifetime;
    mutable Next n;
    mutable Error e;
    mutable Complete c;
    template<class V>
    void next(V&& v) const {
        using namespace detail;
        report(end(lifetime, e, d), enforce(lifetime, n), d, std::forward<V>(v));
    }
    template<class E>
    void error(E&& err) const {
        using namespace detail;
        report(fail{}, end(lifetime, e), d, std::forward<E>(err));
    }
    void complete() const {
        using namespace detail;
        report(fail{}, end(lifetime, c), d);
    }
    template<class V, class E = exception_ptr>
    observer_interface<V, E> as_interface() const {
        using observer_t = detail::basic_observer<V, E, Delegatee, Next, Error, Complete>;
        return {lifetime, make_shared<observer_t>(*this)};
    }
};

template<class Next = detail::noop, class Error = detail::fail, class Complete = detail::noop, 
    class CheckN = detail::not_observer<Next>>
auto make_observer(subscription lifetime, Next&& n = Next{}, Error&& e = Error{}, Complete&& c = Complete{}) {
    return observer<decay_t<Next>, decay_t<Error>, decay_t<Complete>>{
        lifetime,
        forward<Next>(n), 
        forward<Error>(e), 
        forward<Complete>(c)
    };
}

template<class Delegatee, class Next = detail::noop, class Error = detail::pass, class Complete = detail::pass, 
    class CheckD = detail::for_observer<Delegatee>>
auto make_observer(Delegatee&& d, subscription lifetime, Next&& n = Next{}, Error&& e = Error{}, Complete&& c = Complete{}) {
    return observer<decay_t<Delegatee>, decay_t<Next>, decay_t<Error>, decay_t<Complete>>{
        forward<Delegatee>(d), 
        lifetime,
        forward<Next>(n), 
        forward<Error>(e), 
        forward<Complete>(c)
    };
}

template<class T>
using time_point_t = typename decay_t<T>::time_point;
template<class T>
using duration_t = typename decay_t<T>::duration;

template<class T>
using clock_t = typename decay_t<T>::clock_type;
template<class T>
using clock_time_point_t = time_point_t<clock_t<T>>;
template<class T>
using clock_duration_t = duration_t<clock_t<T>>;


template<class Select = defaults, class... TN>
struct strand;

namespace detail {
    template<class C>
    using re_defer_at_t = function<void(time_point_t<C>)>;

    template<class C, class E>
    struct abstract_strand
    {
        virtual ~abstract_strand(){}
        virtual time_point_t<C> now() const = 0;
        virtual void defer_at(time_point_t<C>, observer_interface<re_defer_at_t<C>, E>) const = 0;
    };

    template<class C, class E, class Execute, class Now>
    struct basic_strand : public abstract_strand<C, E> {
        using clock_type = decay_t<C>;
        using errorvalue_type = decay_t<E>;
        basic_strand(const strand<Execute, Now, C>& o)
            : d(o){
        }
        strand<Execute, Now, C> d;
        virtual clock_time_point_t<basic_strand> now() const {
            return d.now();
        }
        virtual void defer_at(clock_time_point_t<basic_strand> at, observer_interface<re_defer_at_t<C>, E> out) const {
            d.defer_at(at, out);
        }
    };

    template<class Clock>
    struct immediate {
        subscription lifetime;
        template<class... ON>
        void operator()(time_point_t<Clock> at, observer<ON...> out) const {
            auto next = at;
            bool stop = false;
            info("immediate::defer_at");
            while (!stop && !lifetime.is_stopped()) {
                info("immediate::defer_at sleep_until");
                this_thread::sleep_until(next);
                stop = true;
                info("immediate::defer_at next");
                out.next([&](typename Clock::time_point at){
                    info("immediate::defer_at self");
                    stop = false;
                    next = at;
                });
            }
            info("immediate::defer_at complete");
            out.complete();
        }
    };

    template<class Clock>
    struct now {
        time_point_t<Clock> operator()() const {
            return Clock::now();
        }
    };
}

template<class C, class E>
struct strand<interface<C, E>> {
    using clock_type = decay_t<C>;
    using errorvalue_type = decay_t<E>;
    strand(const strand& o) = default;
    template<class Execute, class Now>
    strand(const strand<Execute, Now, C>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_strand<C, E, Execute, Now>>(o)) {
    }
    subscription lifetime;
    shared_ptr<detail::abstract_strand<clock_type, errorvalue_type>> d;
    time_point_t<clock_type> now() const {
        return d->now();
    }
    void defer_at(time_point_t<clock_type> at, observer_interface<detail::re_defer_at_t<C>, E> out) const {
        d->defer_at(at, out);
    }
    template<class... TN>
    strand as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using strand_interface = strand<interface<C, E>>;

template<class Execute, class Now, class Clock>
struct strand<Execute, Now, Clock> {
    using clock_type = decay_t<Clock>;
    subscription lifetime;
    Execute e;
    Now n;
    time_point_t<clock_type> now() const {
        return n();
    }
    template<class... ON>
    void defer_at(time_point_t<clock_type> at, observer<ON...> out) const {
        e(at, out);
    }
    template<class E = exception_ptr>
    strand_interface<Clock, E> as_interface() const {
        using strand_t = detail::basic_strand<Clock, E, Execute, Now>;
        return {lifetime, make_shared<strand_t>(*this)};
    }
};

template<class Clock = steady_clock, class Execute = detail::immediate<Clock>, class Now = detail::now<Clock>>
auto make_strand(subscription lifetime, Execute&& e = Execute{}, Now&& n = Now{}) {
    return strand<decay_t<Execute>, decay_t<Now>, Clock>{
        lifetime,
        forward<Execute>(e), 
        forward<Now>(n)
    };
}

template<class SharedStrand>
struct shared_strand_maker {
    using shared_strand_type = decay_t<SharedStrand>;
    shared_strand_type s;
    auto operator()(subscription lifetime) const {
        s->lifetime.insert(lifetime);
        lifetime.insert([lifetime, l = s->lifetime](){
            l.erase(lifetime);
        });
        return make_strand<clock_t<decltype(*s)>>(lifetime, 
            [s = this->s, lifetime](auto at, auto o){
                lifetime.insert(o.lifetime);
                o.lifetime.insert([lifetime = o.lifetime, l = lifetime](){
                    l.erase(lifetime);
                });
                s->defer_at(at, o);
            },
            [s = this->s](){return s->now();});
    }
};


template<class Strand>
shared_strand_maker<shared_ptr<decay_t<Strand>>> make_shared_strand_maker(Strand&& s){
    return {make_shared<decay_t<Strand>>(forward<Strand>(s))};
}

template<class MakeStrand>
auto make_shared_make_strand(MakeStrand make) {
    auto strand = make(subscription{});
    return make_shared_strand_maker(strand);
}

namespace detail {

template<class T>
using for_strand = for_specialization_of_t<T, strand>;

template<class T>
using not_strand = not_specialization_of_t<T, strand>;

}

template<class... SN, class... ON>
void defer(strand<SN...> s, observer<ON...> out) {
    s.defer_at(s.now(), out);
}
template<class... SN, class... ON>
void defer_at(strand<SN...> s, clock_time_point_t<strand<SN...>> at, observer<ON...> out) {
    s.defer_at(at, out);
}
template<class... SN, class... ON>
void defer_after(strand<SN...> s, clock_duration_t<strand<SN...>> delay, observer<ON...> out) {
    s.defer_at(s.now() + delay, out);
}
template<class... SN, class... ON>
void defer_periodic(strand<SN...> s, clock_time_point_t<strand<SN...>> initial, clock_duration_t<strand<SN...>> period, observer<ON...> out) {
    long count = 0;
    auto target = initial;
    s.defer_at(initial, make_observer(
        out, 
        out.lifetime, 
        [count, target, period](observer<ON...>& out, auto& self) mutable {
            if (!out.lifetime.is_stopped()) {
                out.next(count++);
                target += period;
                self(target);
            }
        }, detail::pass{}, detail::skip{}));
}

template<class Select = defaults, class... TN>
struct context;

namespace detail {
    template<class C, class E>
    struct abstract_context : public abstract_strand<C, E>
    {
        virtual ~abstract_context(){}
    };

    template<class C, class E, class MakeStrand>
    struct basic_context : public abstract_context<C, E> {
        using clock_type = decay_t<C>;
        using errorvalue_type = decay_t<E>;
        basic_context(context<void, MakeStrand, C> o)
            : d(o){
        }
        template<class Defaults>
        basic_context(context<Defaults> o)
            : d(o.lifetime, o.m){
        }
        context<void, MakeStrand, C> d;
        virtual time_point_t<clock_type> now() const {
            return d.now();
        }
        virtual void defer_at(time_point_t<clock_type> at, observer_interface<re_defer_at_t<C>, E> out) const {
            d.defer_at(at, out);
        }
    };
    
    template<class C, class E>
    using make_strand_t = function<strand_interface<C, E>(subscription)>;

    template<class Clock = steady_clock>
    struct make_immediate {
        auto operator()(subscription lifetime) const {
            return make_strand<Clock>(lifetime, detail::immediate<Clock>{lifetime}, detail::now<Clock>{});
        }
    };

}

#if !RX_SLOW
template<class Clock>
auto make_shared_make_strand(const detail::make_immediate<Clock>& make) {
    return make;
}
#endif

template<class C, class E>
struct context<interface<C, E>> {
    using payload_type = void;
    using clock_type = decay_t<C>;
    using errorvalue_type = decay_t<E>;
    context(const context& o) = default;
    context(context&& o) = default;
    context(const context<void, void, clock_type>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_context<C, E, void>>(o))
        , m([m = o.m](subscription lifetime){
            return m(lifetime);
        }) {
    }
    context(context<void, void, clock_type>&& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_context<C, E, void>>(o))
        , m([m = o.m](subscription lifetime){
            return m(lifetime);
        }) {
    }
    template<class... CN>
    context(const context<CN...>& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_context<C, E, decay_t<decltype(o.m)>>>(o))
        , m([m = o.m](subscription lifetime){
            return m(lifetime);
        }) {
    }
    template<class... CN>
    context(context<CN...>&& o)
        : lifetime(o.lifetime)
        , d(make_shared<detail::basic_context<C, E, decay_t<decltype(o.m)>>>(o))
        , m([m = o.m](subscription lifetime){
            return m(lifetime);
        }) {
    }

    subscription lifetime;
    shared_ptr<detail::abstract_context<clock_type, errorvalue_type>> d;
    detail::make_strand_t<clock_type, E> m;
    time_point_t<clock_type> now() const {
        return d->now();
    }
    void defer_at(time_point_t<clock_type> at, observer_interface<detail::re_defer_at_t<clock_type>, E> out) const {
        d->defer_at(at, out);
    }
    template<class... TN>
    context as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using context_interface = context<interface<C, E>>;

template<>
struct context<defaults> {
    using payload_type = void;
    using Clock = steady_clock;
    using clock_type = decay_t<Clock>;
    using make_strand_type = detail::make_immediate<Clock>;
    using strand_type = decay_t<decltype(declval<make_strand_type>()(declval<subscription>()))>;
    subscription lifetime;
    make_strand_type m;
private:
    struct State {
        explicit State(strand_type&& s) : s(s) {}
        explicit State(const strand_type& s) : s(s) {}
        strand_type s;
    };
    state<State> s;    
public:

    explicit context(subscription lifetime) 
        : lifetime(lifetime)
        , m()
        , s(make_state<State>(lifetime, m(subscription{}))) {
        lifetime.insert(s.get().s.lifetime);
    }
    context(subscription lifetime, make_strand_type m, strand_type strand) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, strand)) {
        lifetime.insert(s.get().s.lifetime);
    }
    time_point_t<clock_type> now() const {
        return s.get().s.now();
    }
    template<class... ON>
    void defer_at(time_point_t<clock_type> at, observer<ON...> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<Clock, E> as_interface() const {
        using context_t = detail::basic_context<Clock, E, make_strand_type>;
        return {lifetime, make_shared<context_t>(*this)};
    }
};

template<class Clock>
struct context<void, void, Clock> {
    using payload_type = void;
    using clock_type = decay_t<Clock>;
    using make_strand_type = detail::make_immediate<Clock>;
    using strand_type = decay_t<decltype(declval<make_strand_type>()(declval<subscription>()))>;
    subscription lifetime;
    make_strand_type m;
private:
    struct State {
        explicit State(strand_type&& s) : s(s) {}
        explicit State(const strand_type& s) : s(s) {}
        strand_type s;
    };
    state<State> s;    
public:

    explicit context(subscription lifetime) 
        : lifetime(lifetime)
        , m()
        , s(make_state<State>(lifetime, m(subscription{}))) {
        lifetime.insert(s.get().s.lifetime);
    }
    context(subscription lifetime, make_strand_type m, strand_type s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
        lifetime.insert(s.get().s.lifetime);
    }
    time_point_t<clock_type> now() const {
        return s.get().s.now();
    }
    template<class... ON>
    void defer_at(time_point_t<clock_type> at, observer<ON...> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<clock_type, E> as_interface() const {
        using context_t = detail::basic_context<clock_type, E, make_strand_type>;
        return {lifetime, make_shared<context_t>(*this)};
    }
};

template<class MakeStrand, class Clock>
struct context<void, MakeStrand, Clock> {
    using payload_type = void;
    using clock_type = decay_t<Clock>;
    using make_strand_type = decay_t<MakeStrand>;
    using strand_type = decay_t<decltype(declval<make_strand_type>()(declval<subscription>()))>;
    subscription lifetime;
    MakeStrand m;

private:
    struct State {
        explicit State(strand_type&& s) : s(s) {}
        explicit State(const strand_type& s) : s(s) {}
        strand_type s;
    };
    state<State> s;  

public:
    context(subscription lifetime, make_strand_type m) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, m(subscription{}))) {
        lifetime.insert(s.get().s.lifetime);
    }
    context(subscription lifetime, make_strand_type m, strand_type s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
        lifetime.insert(this->s.get().s.lifetime);
    }
    time_point_t<clock_type> now() const {
        return s.get().s.now();
    }
    template<class... ON>
    void defer_at(time_point_t<clock_type> at, observer<ON...> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<Clock, E> as_interface() const {
        using context_t = detail::basic_context<Clock, E, make_strand_type>;
        return {lifetime, make_shared<context_t>(*this)};
    }  
};

template<class Payload, class MakeStrand, class Clock>
struct context<Payload, MakeStrand, Clock> {
    using payload_type = decay_t<Payload>;
    using clock_type = decay_t<Clock>;
    using make_strand_type = decay_t<MakeStrand>;
    using strand_type = decay_t<decltype(declval<make_strand_type>()(declval<subscription>()))>;
    subscription lifetime;
    MakeStrand m;
    context(subscription lifetime, payload_type p, make_strand_type m) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, m(subscription{}), move(p))) {
        lifetime.insert(s.get().s.lifetime);
    }
    time_point_t<clock_type> now() const {
        return s.get().s.now();
    }
    template<class... ON>
    void defer_at(time_point_t<clock_type> at, observer<ON...> out) const {
        s.get().s.defer_at(at, out);
    }
    template<class E = exception_ptr>
    context_interface<clock_type, E> as_interface() const {
        using context_t = detail::basic_context<clock_type, E, make_strand_type>;
        return {lifetime, make_shared<context_t>(*this)};
    }
    payload_type& get(){
        return s.get().p;
    }
    const payload_type& get() const {
        return s.get().p;
    }
    operator context<void, make_strand_type, clock_type> () const {
        return context<void, make_strand_type, clock_type>(lifetime, m, s.get().s);
    }
private:
    using Strand = decay_t<decltype(declval<MakeStrand>()(declval<subscription>()))>;
    struct State {
        State(strand_type&& s, payload_type&& p) : s(s), p(p) {}
        State(const strand_type& s, const payload_type& p) : s(s), p(p) {}
        strand_type s;
        payload_type p;
    };
    state<State> s;    
};

inline auto make_context(subscription lifetime) {
    return context<>{
        lifetime
    };
}

template<class Payload, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    return context<Payload, detail::make_immediate<steady_clock>, steady_clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<steady_clock>{}
    };
}

template<class Payload, class Clock, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    return context<Payload, detail::make_immediate<Clock>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<Clock>{}
    };
}

template<class Payload, class Clock, class MakeStrand, class... AN>
auto make_context(subscription lifetime, MakeStrand&& m, AN&&... an) {
    return context<Payload, decay_t<MakeStrand>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        forward<MakeStrand>(m)
    };
}

template<class Clock, class MakeStrand, class C = void_t<typename Clock::time_point>>
auto make_context(subscription lifetime, MakeStrand&& m) {
    return context<void, decay_t<MakeStrand>, Clock>{
        lifetime,
        forward<MakeStrand>(m)
    };
}

inline auto copy_context(subscription lifetime, const context<>&) {
    return make_context(lifetime);
}

template<class Payload, class MakeStrand, class Clock>
auto copy_context(subscription lifetime, const context<Payload, MakeStrand, Clock>& o) {
    return make_context<Payload, Clock>(lifetime, o.m, o.get());
}

template<class MakeStrand, class Clock>
auto copy_context(subscription lifetime, const context<void, MakeStrand, Clock>& o) {
    return make_context<Clock>(lifetime, o.m);
}

template<class Clock>
auto copy_context(subscription lifetime, const context<void, void, Clock>& o) {
    return make_context<Clock>(lifetime, o.m);
}

template<class NewMakeStrand, class... CN>
auto copy_context(subscription lifetime, NewMakeStrand&& makeStrand, const context<CN...>& o) {
    return make_context<clock_t<context<CN...>>>(lifetime, forward<NewMakeStrand>(makeStrand));
}

template<class C, class E>
auto copy_context(subscription lifetime, const context_interface<C, E>& o) {
    return context_interface<C, E>{
        context<void, detail::make_strand_t<C, E>, C> {
            lifetime,
            o.m
        }
    };
}

namespace detail {

template<class T>
using for_context = for_specialization_of_t<T, context>;

template<class T>
using not_context = not_specialization_of_t<T, context>;

}

template<class... CN, class... ON>
void defer(context<CN...> s, observer<ON...> out) {
    s.defer_at(s.now(), out);
}
template<class... CN, class... ON>
void defer_at(context<CN...> s, clock_time_point_t<context<CN...>> at, observer<ON...> out) {
    s.defer_at(at, out);
}
template<class... CN, class... ON>
void defer_after(context<CN...> s, clock_duration_t<context<CN...>> delay, observer<ON...> out) {
    s.defer_at(s.now() + delay, out);
}
template<class... CN, class... ON>
void defer_periodic(context<CN...> s, clock_time_point_t<context<CN...>> initial, clock_duration_t<context<CN...>> period, observer<ON...> out) {
    long count = 0;
    auto target = initial;
    s.defer_at(initial, make_observer(
        out, 
        out.lifetime, 
        [count, target, period](const observer<ON...>& out, auto& self) mutable {
            out.next(count++);
            target += period;
            self(target);
        }, detail::pass{}, detail::skip{}));
}

template<class Select = defaults>
struct starter;

namespace detail {
    template<class C, class E>
    using start_t = function<subscription(context_interface<C, E>)>;
}
template<class C, class E>
struct starter<interface<C, E>> {
    detail::start_t<C, E> s;
    starter(const starter&) = default;
    template<class Start>
    starter(const starter<Start>& s)
        : s(s.s) {
    }
    subscription start(context_interface<C, E> ctx) const {
        return s(move(ctx));
    }
    template<class... TN>
    starter as_interface() const {
        return {*this};
    }
};
template<class C, class E>
using starter_interface = starter<interface<C, E>>;

template<class Start>
struct starter {
    Start s;
    template<class... CN>
    subscription start(context<CN...> ctx) const {
        return s(ctx);
    }
    template<class C = steady_clock, class E = exception_ptr>
    starter_interface<C, E> as_interface() const {
        return {*this};
    }
};
template<class Start>
starter<Start> make_starter(Start&& s) {
    return {forward<Start>(s)};
}

namespace detail {

template<class T>
struct starter_check : public false_type {};

template<class Select>
struct starter_check<starter<Select>> : public true_type {};

template<class T>
using for_starter = enable_if_t<starter_check<decay_t<T>>::value>;

template<class T>
using not_starter = enable_if_t<!starter_check<decay_t<T>>::value>;

}

template<class Select = defaults>
struct subscriber;

namespace detail {
    template<class V, class C, class E>
    using create_t = function<observer_interface<V, E>(context_interface<C, E>)>;
}
template<class V, class C, class E>
struct subscriber<interface<V, C, E>> {
    detail::create_t<V, C, E> c;
    subscriber(const subscriber&) = default;
    template<class Create>
    subscriber(const subscriber<Create>& o)
        : c(o.c) {
    }
    observer_interface<V, E> create(context_interface<C, E> ctx) const {
        return c(ctx);
    }
    template<class... TN>
    subscriber as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using subscriber_interface = subscriber<interface<V, C, E>>;

template<class Create>
struct subscriber {
    Create c;
    /// \brief returns observer
    template<class... CN>
    auto create(context<CN...> ctx) const {
        static_assert(detail::is_specialization_of<decltype(c(ctx)), observer>::value, "subscriber function must return observer!");
        return c(ctx);
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    subscriber_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Create>
subscriber<Create> make_subscriber(Create&& c) {
    return {forward<Create>(c)};
}

auto make_subscriber() {
    return make_subscriber([](auto ctx){return make_observer(ctx.lifetime);});
}

namespace detail {

template<class T>
using for_subscriber = for_specialization_of_t<T, subscriber>;

template<class T>
using not_subscriber = not_specialization_of_t<T, subscriber>;

}

template<class Select = defaults>
struct observable;

namespace detail {
    template<class V, class C, class E>
    using bind_t = function<starter_interface<C, E>(subscriber_interface<V, C, E>)>;
}
template<class V, class C, class E>
struct observable<interface<V, C, E>> {
    detail::bind_t<V, C, E> b;
    observable(const observable&) = default;
    template<class Bind>
    observable(const observable<Bind>& o) 
        : b(o.b) {
    } 
    starter_interface<C, E> bind(subscriber_interface<V, C, E> s) const {
        return b(s);
    }
    template<class... TN>
    observable as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using observable_interface = observable<interface<V, C, E>>;

template<class Bind>
struct observable {
    Bind b;
    /// \brief 
    /// \returns starter
    template<class Subscriber>
    auto bind(Subscriber&& s) const {
        static_assert(detail::is_specialization_of<decltype(b(s)), starter>::value, "observable function must return starter!");
        return b(s);
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    observable_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Bind>
observable<Bind> make_observable(Bind&& b) {
    return {forward<Bind>(b)};
}

namespace detail {

template<class T>
using for_observable = for_specialization_of_t<T, observable>;

template<class T>
using not_observable = not_specialization_of_t<T, observable>;

}

template<class Select = defaults>
struct lifter;

namespace detail {
    template<class VL, class CL, class EL, class VR, class CR, class ER>
    using lift_t = function<subscriber_interface<VL, CL, EL>(subscriber_interface<VR, CR, ER>)>;
}
template<class VL, class CL, class EL, class VR, class CR, class ER>
struct lifter<interface<VL, CL, EL, VR, CR, ER>> {
    detail::lift_t<VL, CL, EL, VR, CR, ER> l;
    lifter(const lifter&) = default;
    template<class Lift>
    lifter(const lifter<Lift>& l) 
        : l(l.l){
    }
    subscriber_interface<VL, CL, EL> lift(subscriber_interface<VR, CR, ER> s) const {
        return l(s);
    }
    template<class... TN>
    lifter as_interface() const {
        return {*this};
    }
};
template<class VL, class CL, class EL, class VR, class CR, class ER>
using lifter_interface = lifter<interface<VL, CL, EL, VR, CR, ER>>;

template<class Lift>
struct lifter {
    using lift_type = decay_t<Lift>;
    lift_type l;
    /// \brief returns subscriber    
    template<class... SN>
    auto lift(subscriber<SN...> s) const {
        static_assert(detail::is_specialization_of<decltype(l(s)), subscriber>::value, "lift function must return subscriber!");
        return l(s);
    }
    template<class VL, class CL = steady_clock, class EL = exception_ptr, class VR = VL, class CR = CL, class ER = EL>
    lifter_interface<VL, CL, EL, VR, CR, ER> as_interface() const {
        return {*this};
    }
};

template<class Lift>
lifter<Lift> make_lifter(Lift&& l) {
    return {forward<Lift>(l)};
}

namespace detail {

template<class T>
using for_lifter = for_specialization_of_t<T, lifter>;

template<class T>
using not_lifter = not_specialization_of_t<T, lifter>;

}

template<class Select = defaults>
struct adaptor;

namespace detail {
    template<class VL, class CL, class EL, class VR, class CR, class ER>
    using adapt_t = function<observable_interface<VR, CR, ER>(const observable_interface<VL, CL, EL>&)>;
}
template<class VL, class CL, class EL, class VR, class CR, class ER>
struct adaptor<interface<VL, CL, EL, VR, CR, ER>> {
    detail::adapt_t<VL, CL, EL, VR, CR, ER> a;
    adaptor(const adaptor&) = default;
    template<class Adapt>
    adaptor(const adaptor<Adapt>& a)
        : a(a.a) {
    }
    observable_interface<VR, CR, ER> adapt(const observable_interface<VL, CL, EL>& ovr) const {
        return a(ovr);
    }
    template<class... TN>
    adaptor as_interface() const {
        return {*this};
    }
};
template<class VL, class CL, class EL, class VR, class CR, class ER>
using adaptor_interface = adaptor<interface<VL, CL, EL, VR, CR, ER>>;

template<class Adapt>
struct adaptor {
    Adapt a;
    /// \brief returns observable
    template<class... ON>
    auto adapt(observable<ON...> o) const {
        static_assert(detail::is_specialization_of<decltype(a(o)), observable>::value, "adaptor function must return observable!");
        return a(o);
    }
    template<class VL, class CL = steady_clock, class EL = exception_ptr, class VR = VL, class CR = CL, class ER = EL>
    adaptor_interface<VL, CL, EL, VR, CR, ER> as_interface() const {
        return {*this};
    }
};

template<class Adapt>
adaptor<Adapt> make_adaptor(Adapt&& l) {
    return {forward<Adapt>(l)};
}

namespace detail {

template<class T>
using for_adaptor = for_specialization_of_t<T, adaptor>;

template<class T>
using not_adaptor = not_specialization_of_t<T, adaptor>;

}

template<class Select = defaults>
struct terminator;

namespace detail {
    template<class V, class C, class E>
    using terminate_t = function<starter_interface<C, E>(const observable_interface<V, C, E>&)>;
}
template<class V, class C, class E>
struct terminator<interface<V, C, E>> {
    detail::terminate_t<V, C, E> t;
    terminator(const terminator&) = default;
    template<class Terminate>
    terminator(const terminator<Terminate>& t) 
        : t(t.t) {
    }
    starter_interface<C, E> terminate(const observable_interface<V, C, E>& ovr) const {
        return t(ovr);
    }
    template<class... TN>
    terminator as_interface() const {
        return {*this};
    }
};
template<class V, class C, class E>
using terminator_interface = terminator<interface<V, C, E>>;

template<class Terminate>
struct terminator {
    Terminate t;
    /// \brief returns starter
    template<class... ON>
    auto terminate(observable<ON...> o) const {
        static_assert(detail::is_specialization_of<decltype(t(o)), starter>::value, "terminator function must return starter!");
        return t(o);
    }
    template<class V, class C = steady_clock, class E = exception_ptr>
    terminator_interface<V, C, E> as_interface() const {
        return {*this};
    }
};

template<class Terminate>
terminator<Terminate> make_terminator(Terminate&& t) {
    return {forward<Terminate>(t)};
}

namespace detail {

template<class T>
using for_terminator = for_specialization_of_t<T, terminator>;

template<class T>
using not_terminator = not_specialization_of_t<T, terminator>;

}


inline context<> start(subscription lifetime = subscription{}) {
    info("start");
    return make_context(lifetime);
}

template<class Payload, class... AN>
auto start(AN&&... an) {
    info("start payload");
    return make_context<Payload>(subscription{}, forward<AN>(an)...);
}

template<class Payload, class... ArgN>
auto start(subscription lifetime, ArgN&&... an) {
    info("start liftime & payload");
    return make_context<Payload>(lifetime, forward<ArgN>(an)...);
}

template<class Payload, class Clock, class... AN>
auto start(AN&&... an) {
    info("start clock & payload");
    return make_context<Payload, Clock>(subscription{}, forward<AN>(an)...);
}

template<class Payload, class Clock, class... AN>
auto start(subscription lifetime, AN&&... an) {
    info("start liftime & clock & payload");
    return make_context<Payload, Clock>(lifetime, forward<AN>(an)...);
}

template<class Payload, class MakeStrand, class Clock>
auto start(const context<Payload, MakeStrand, Clock>& o) {
    info("start copy");
    return o;
}

template<class... CN>
auto start(subscription lifetime, const context<CN...>& o) {
    return copy_context(lifetime, o);
}

template<class... TN>
struct interface_extractor{
    template<class O>
    auto extract(O&& o){
        return o.template as_interface<TN...>();
    }
};
template<class... TN>
interface_extractor<TN...> as_interface() {
    return {};
}

/// \brief chain operator overload for
/// AnyInterface = Any | InterfaceExtractor
/// \param any
/// \param interface_extractor
/// \returns any_interface
template<class O, class... TN>
auto operator|(O&& o, interface_extractor<TN...>&& ie) {
    return ie.extract(forward<O>(o));
}

const auto intervals = [](auto initial, auto period){
    info("new intervals");
    return make_observable([=](auto scrb){
        info("intervals bound to subscriber");
        return make_starter([=](auto ctx) {
            info("intervals bound to context");
            auto r = scrb.create(ctx);
            info("intervals started");
            defer_periodic(ctx, initial, period, r);
            return ctx.lifetime;
        });
    });
};

const auto ints = [](auto first, auto last){
    info("new ints");
    return make_observable([=](auto scrb){
        info("ints bound to subscriber");
        return make_starter([=](auto ctx) {
            info("ints bound to context");
            auto r = scrb.create(ctx);
            info("ints started");
            for(auto i = first;!ctx.lifetime.is_stopped(); ++i){
                r.next(i);
                if (i == last) break;
            }
            r.complete();
            return ctx.lifetime;
        });
    });
};

template<class MakeStrand>
auto observe_on(MakeStrand makeStrand){
    info("new observe_on");
    return make_lifter([=](auto scbr){
        info("observe_on bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("observe_on bound to context");
            subscription lifetime;
            lifetime.insert(ctx.lifetime);
            auto outcontext = copy_context(lifetime, makeStrand, ctx);
            auto r = scbr.create(outcontext);
            return make_observer(r, ctx.lifetime, 
                [=](auto& r, auto v){
                    auto next = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.next(v);
                    }, detail::pass{}, detail::skip{});
                    defer(outcontext, next);
                },
                [=](auto& r, auto e){
                    auto error = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.error(e);
                    }, detail::pass{}, detail::skip{});
                    defer(outcontext, error);
                },
                [=](auto& r){
                    auto complete = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.complete();
                    }, detail::pass{}, detail::skip{});
                    defer(outcontext, complete);
                });
        });
    });
}

#if !RX_SLOW
template<class Clock>
auto observe_on(const detail::make_immediate<Clock>&){
    info("new observe_on");
    return make_lifter([=](auto scbr){
        info("observe_on bound to subscriber");
        return scbr;
    });
}
#endif

const auto delay = [](auto makeStrand, auto delay){
    info("new delay");
    return make_lifter([=](auto scbr){
        info("delay bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("delay bound to context");
            subscription lifetime;
            lifetime.insert(ctx.lifetime);
            auto outcontext = copy_context(lifetime, makeStrand, ctx);
            auto r = scbr.create(outcontext);
            return make_observer(r, r.lifetime, 
                [=](auto& r, auto v){
                    auto next = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.next(v);
                    }, detail::pass{}, detail::skip{});
                    defer_after(outcontext, delay, next);
                },
                [=](auto& r, auto e){
                    auto error = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.error(e);
                    }, detail::pass{}, detail::skip{});
                    defer_after(outcontext, delay, error);
                },
                [=, l = ctx.lifetime](auto& r){
                    auto complete = make_observer(r, subscription{}, [=](auto& r, auto& self){
                        r.complete();
                    }, detail::pass{}, detail::skip{});
                    defer_after(outcontext, delay, complete);
                });
        });
    });
};

const auto copy_if = [](auto pred){
    info("new copy_if");
    return make_lifter([=](auto scbr){
        info("copy_if bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("copy_if bound to context");
            auto r = scbr.create(ctx);
            return make_observer(r, ctx.lifetime, [=](auto& r, auto v){
                if (pred(v)) r.next(v);
            });
        });
    });
};

const auto transform = [](auto f){
    info("new transform");
    return make_lifter([=](auto scbr){
        info("transform bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("transform bound to context");
            auto r = scbr.create(ctx);
            return make_observer(r, ctx.lifetime, [=](auto& r, auto& v){
                r.next(f(v));
            });
        });
    });
};

const auto last_or_default = [](auto def){
        info("new last_or_default");
    return make_lifter([=](auto scbr){
        info("last_or_default bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("last_or_default bound to context");
            auto r = scbr.create(ctx);
            auto last = ctx.lifetime.template make_state<std::decay_t<decltype(def)>>(def);
            return make_observer(r, ctx.lifetime, 
                [last](auto& r, auto v){
                    last.get() = v;
                },
                detail::ignore{},
                [last](auto& r){
                    r.next(last.get());
                    r.complete();
                });
        });
    });
};

const auto take = [](int n){
    info("new take");
    return make_adaptor([=](auto source){
        info("take bound to source");
        return make_observable([=](auto scrb){
            info("take bound to subscriber");
            return source.bind(
                make_subscriber([=](auto ctx){
                    info("take bound to context");
                    auto r = scrb.create(ctx);
                    auto remaining = ctx.lifetime.template make_state<int>(n);
                    return make_observer(r, ctx.lifetime, 
                    [remaining](auto& r, auto v){
                        if (remaining.get()-- == 0) {
                            r.complete();
                            return;
                        }
                        r.next(v);
                    });
                }));
        });
    });
};

const auto merge = [](auto makeStrand){
    info("new merge");
    return make_adaptor([=](auto source){
        info("merge bound to source");
        auto sharedmakestrand = make_shared_make_strand(makeStrand);
        info("merge-input start");
        return source |
            observe_on(sharedmakestrand) |
            make_lifter([=](auto scrb) {
                info("merge bound to subscriber");
                return make_subscriber([=](auto ctx){
                    info("merge bound to context");
                    
                    subscription destlifetime;

                    auto pending = make_state<set<subscription>>(destlifetime);
                    pending.get().insert(ctx.lifetime);

                    destlifetime.insert([pending](){
                        info("merge-output stopping all inputs");
                        // stop all the inputs
                        while (!pending.get().empty()) {
                            (*pending.get().begin()).stop();
                        }
                        info("merge-output stop");
                    });

                    auto destctx = copy_context(destlifetime, sharedmakestrand, ctx);
                    auto r = scrb.create(destctx);

                    ctx.lifetime.insert([pending, r, l = ctx.lifetime, destctx](){
                        defer(destctx, make_observer(subscription{}, detail::noop{}, detail::fail{}, [=](){
                            pending.get().erase(l);
                            if (pending.get().empty()){
                                info("merge-input complete destination");
                                r.complete();
                            }
                            info("merge-input stop");
                        }));
                    });

                    return make_observer(r, ctx.lifetime, 
                        [pending, destctx](auto& r, auto& v){
                            info("merge-nested start");
                            subscription nestedlifetime;
                            pending.get().insert(nestedlifetime);
                            nestedlifetime.insert([pending, r, l = nestedlifetime, destctx](){
                                defer(destctx, make_observer(subscription{}, detail::noop{}, detail::fail{}, [=](){
                                    pending.get().erase(l);
                                    if (pending.get().empty()){
                                        info("merge-nested complete destination");
                                        r.complete();
                                    }
                                    info("merge-nested stop");
                                }));
                            });
                            v |
                                observe_on(destctx.m) |
                                make_subscriber([=](auto ctx){
                                    info("merge-nested bound to context");
                                    return make_observer(r, ctx.lifetime, 
                                        [pending](auto& r, auto& v){
                                            r.next(v);
                                        }, detail::pass{}, detail::skip{});
                                }) | 
                                start(nestedlifetime, destctx);
                        }, detail::pass{}, detail::skip{});
                });
            });
    });
};

template<class MakeStrand, class F>
auto transform_merge(MakeStrand&& makeStrand, F&& f) {
    return transform(forward<F>(f)) | merge(forward<MakeStrand>(makeStrand));
};

const auto printto = [](auto& output){
    info("new printto");
    return make_subscriber([&](auto ctx) {
        info("printto bound to context");
        auto values = ctx.lifetime.template make_state<int>(0);
        return make_observer(
            ctx.lifetime,
            [&, values](auto v) {
                ++values.get();
                output << v << endl;
            },
            [&](exception_ptr ep){
                output << what(ep) << endl;
            },
            [&, values](){
                output << values.get() << " values received - done!" << endl;
            });
    });
};


/// \brief chain operator overload for
/// Subscriber = Lifter | Subscriber
/// \param lifter
/// \param subscriber
/// \returns subscriber
template<class... LN, class... SN>
auto operator|(lifter<LN...> l, subscriber<SN...> scbr) {
    return l.lift(scbr);
}

/// \brief chain operator overload for
/// Lifter = Lifter | Lifter
/// \param lifter
/// \param lifter
/// \returns Lifter
template<class... LLN, class... LRN>
auto operator|(lifter<LLN...> lhs, lifter<LRN...> rhs) {
    return make_lifter([lhs = move(lhs), rhs = move(rhs)](auto scbr){
        lhs.lift(rhs.lift(scbr));
    });
}

/// \brief chain operator overload for
/// Observable = Observable | Lifter
/// \param observable
/// \param lifter
/// \returns observable
template<class... ON, class... LN>
auto operator|(observable<ON...> s, lifter<LN...> l) {
    return make_observable([=](auto scrb){
        return s.bind(l.lift(scrb));
    });
}


/// \brief chain operator overload for
/// Starter = Observable | Subscriber
/// \param observable
/// \param subscriber
/// \returns starter
template<class... ON, class... SN>
auto operator|(observable<ON...> s, subscriber<SN...> scbr) {
    return s.bind(scbr);
}

/// \brief chain operator overload for
/// subscription = Starter | Context
/// \param starter
/// \param context
/// \returns subscription
template<class... SN, class... CN>
subscription operator|(starter<SN...> s, context<CN...> ctx) {
    return s.start(ctx);
}

/// \brief chain operator overload for
/// Adaptor = Adaptor | Adaptor
/// \param adaptor
/// \param adaptor
/// \returns adaptor
template<class... ALN, class... ARN>
auto operator|(adaptor<ALN...> lhs, adaptor<ARN...> rhs) {
    return make_adaptor([=](auto source){
        return rhs.adapt(lhs.adapt(source));
    });
}

/// \brief chain operator overload for
/// Adaptor = Adaptor | Lifter
/// \param adaptor
/// \param lifter
/// \returns adaptor
template<class... AN, class... LN>
auto operator|(adaptor<AN...> a, lifter<LN...> l) {
    return make_adaptor([=](auto source){
        auto s = a.adapt(source);
        return make_observable([=](auto scrb){
            return s.bind(l.lift(scrb));
        });
    });
}

/// \brief chain operator overload for
/// Adaptor = Lifter | Adaptor
/// \param lifter
/// \param adaptor
/// \returns adaptor
template<class... LN, class... AN>
auto operator|(lifter<LN...> l, adaptor<AN...> a) {
    return make_adaptor([=](auto source){
        return a.adapt(make_observable([=](auto scrb){
            return source.bind(l.lift(scrb));
        }));
    });
}

/// \brief chain operator overload for
/// Observable = Observable | Adaptor
/// \param observable
/// \param adaptor
/// \returns observable
template<class... ON, class... AN>
auto operator|(observable<ON...> s, adaptor<AN...> a) {
    return a.adapt(s);
}

/// \brief chain operator overload for
/// Terminator = Adaptor | Subscriber
/// \param adaptor
/// \param subscriber
/// \returns terminator
template<class... AN, class... SN>
auto operator|(adaptor<AN...> a, subscriber<SN...> scrb) {
    return make_terminator([=](auto source){
        return a.adapt(source).bind(scrb);
    });
}

/// \brief chain operator overload for
/// starter = Observable | Terminator
/// \param observable
/// \param terminator
/// \returns starter
template<class... ON, class... TN>
auto operator|(observable<ON...> s, terminator<TN...> t) {
    return t.terminate(s);
}


}

extern"C" {
    void designcontext(int, int);
}

void designcontext(int first, int last){
    using namespace std::chrono;

    using namespace rx;
    using rx::copy_if;
    using rx::transform;
    using rx::merge;

auto makeStrand = rx::detail::make_immediate<>{};

auto strand = makeStrand(subscription{});
defer(strand, make_observer(subscription{}, [](auto& self){
    cout << "deferred immediate strand" << endl;
}));

{
    auto ctx = copy_context(subscription{}, makeStrand, start<shared_ptr<destruction>>(make_shared<destruction>()));
    defer(ctx, make_observer(subscription{}, [](auto& self){
        cout << "deferred immediate context" << endl;
    }));
}

auto sharedmakestrand = make_shared_make_strand(makeStrand);

auto sharedstrand = sharedmakestrand(subscription{});
defer(sharedstrand, make_observer(subscription{}, [](auto& self){
    cout << "deferred shared-immediate strand" << endl;
}));

{
    auto ctx = copy_context(subscription{}, sharedmakestrand, start<shared_ptr<destruction>>(make_shared<destruction>()));
    defer(ctx, make_observer(subscription{}, [](auto& self){
        cout << "deferred shared-immediate context" << endl;
    }));
}

{
 cout << "compile-time polymorphism (canary)" << endl;
    auto lifetime = ints(1, 3) | 
        transform_merge(sharedmakestrand,
            [=](int){
                return ints(1, 10);
            }) |
            printto(cout) |
            start();
}

{
 cout << "intervals" << endl;
    auto threeeven = copy_if(even) | 
        take(3) |
        delay(sharedmakestrand, 1s);

    auto lifetime = intervals(steady_clock::now(), 1s) | 
        threeeven |
        printto(cout) |
        start<shared_ptr<destruction>>(make_shared<destruction>());

    lifetime.insert([](){cout << "caller stopped" << endl;});
}

#if !RX_INFO
{
 cout << "compile-time polymorphism" << endl;
    auto lastofeven = copy_if(even) | 
        take(100000000) |
        last_or_default(42);

 auto t0 = high_resolution_clock::now();
    auto lifetime = ints(0, 2) | 
        transform_merge(rx::detail::make_immediate<>{},
            [=](int){
                return ints(first, last * 100) |
                    lastofeven;
            }) |
            printto(cout) |
            start();

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = ((last * 100) - first) * 3;
 cout << d / sc << " ms per value\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " values per second\n"; 
}

{
 cout << "interface polymorphism" << endl;
    auto lastofeven = copy_if(even) | 
        as_interface<int>() |
        take(100000000) |
        as_interface<int>() |
        last_or_default(42) |
        as_interface<int>();
        
 auto t0 = high_resolution_clock::now();
    auto lifetime = ints(0, 2) | 
        as_interface<int>() |
        transform_merge(rx::detail::make_immediate<>{},
            [=](int){
                return ints(first, last * 100) |
                    as_interface<int>() |
                    lastofeven |
                    as_interface<int>();
            }) |
            as_interface<int>() |
            printto(cout) |
            as_interface<>() |
            start();

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = ((last * 100) - first) * 3;
 cout << d / sc << " ms per value\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " values per second\n"; 
}

{
 cout << "for" << endl;
 auto t0 = high_resolution_clock::now();
    for(auto i = first; i < last; ++i) {
        auto lifetime = ints(0, 0) |
            transform([](int i) {
                return to_string(i);
            }) |
            transform([](const string& s) {
                int i = '0' - s[0];
                return i;
            }) |
        make_subscriber() |
        start();

    }

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}

{
 cout << "transform | merge" << endl;
 auto t0 = high_resolution_clock::now();

    auto lifetime = ints(first, last) | 
        transform([=](int){
            return ints(0, 0) |
                transform([](int i) {
                    return to_string(i);
                }) |
                transform([](const string& s) {
                    int i = '0' - s[0];
                    return i;
                });
        }) |
        merge(rx::detail::make_immediate<>{}) |
        make_subscriber() |
        start();

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}

{
 cout << "transform_merge" << endl;
 auto t0 = high_resolution_clock::now();

    auto lifetime = ints(first, last) | 
        transform_merge(rx::detail::make_immediate<>{}, 
            [=](int){
                return ints(0, 0) |
                    transform([](int i) {
                        return to_string(i);
                    }) |
                    transform([](const string& s) {
                        int i = '0' - s[0];
                        return i;
                    });
            }) |
            make_subscriber() |
            start();

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}
#endif
}
