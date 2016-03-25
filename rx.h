#pragma once

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
    using lock_type = mutex;
    using guard_type = unique_lock<lock_type>;
    struct shared
    {
        ~shared(){
            info(to_string(reinterpret_cast<ptrdiff_t>(this)) + " - subscription: destroy");
            {
                auto expired = std::move(destructors);
                for (auto& d : expired) {
                    d();
                }
            }
            info(to_string(reinterpret_cast<ptrdiff_t>(this)) + " - end lifetime");
        }
        shared() 
            : defer([](function<void()> target){target();})
            , stopped(false) {
            info(to_string(reinterpret_cast<ptrdiff_t>(this)) + " - new lifetime");
        }
        lock_type lock;
        mutex joinlock;
        condition_variable joined;
        function<void(function<void()>)> defer;
        atomic<bool> stopped;
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
        if (!store || store->stopped) info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: is_stopped true");
        else info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: is_stopped false");
        return !store || store->stopped;
    }
    /// \brief 
    void insert(const subscription& s) const {
        if (is_stopped()) {
            s.stop();
            return;
        }
        if (s == *this) {
            info("subscription: inserting self!");
            std::abort();
        }

        guard_type guard(store->lock);
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
        if (s == *this) {
            info("subscription: erasing self!");
            std::abort();
        }

        guard_type guard(store->lock);
        store->others.erase(s);
    }
    /// \brief 
    void insert(function<void()> stopper) const {
        if (is_stopped()) {
            stopper();
            return;
        }

        guard_type guard(store->lock);
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
    void bind_defer(function<void(function<void()>)> d) {
        if (is_stopped()) {
            return;
        }
        guard_type guard(store->lock);
        store->defer = d;
    }
    /// \brief 
    void stop() const {
        if (is_stopped()) {
            return;
        }
        guard_type guard(store->lock);

        store->stopped = true;
        info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: stopped set to true");
        is_stopped();

        auto st = move(store);
        st->defer([=](){
            info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: stop");
            {
                auto others = std::move(st->others);
                for (auto& o : others) {
                    o.stop();
                    o.join();
                }
            }
            {
                auto stoppers = std::move(st->stoppers);
                for (auto& s : stoppers) {
                    s();
                }
            }
            st->defer = [](function<void()> target){target();};

            info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: notify");
            unique_lock<mutex> guard(st->joinlock);
            st->joined.notify_all();
            info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: stopped");
        });
    }
    /// \brief
    void join() const {
        if (is_stopped()) {
            return;
        }
        info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: join");
        unique_lock<mutex> guard(store->joinlock);
        store->joined.wait(guard, [st = this->store](){return !!st->stopped;});
        info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: joined");
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
    info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: make_state " + typeid(Payload).name());
    if (is_stopped()) {
        throw lifetime_error("subscription is stopped!");
    }
    guard_type guard(store->lock);
    auto p = make_unique<Payload>(forward<ArgN>(argn)...);
    auto result = state<Payload>{*this, p.get()};
    store->destructors.emplace_front(
        [d=p.release(), s=store.get()]() mutable {
            info(to_string(reinterpret_cast<ptrdiff_t>(s)) + " - subscription: destroy make_state " + typeid(Payload).name());
            auto p = d; 
            d = nullptr; 
            delete p;
        });
    return result;
}
state<> subscription::make_state() const {
    info(to_string(reinterpret_cast<ptrdiff_t>(store.get())) + " - subscription: make_state");
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
    void operator()(const Delegatee& , E&& ) const {
    }
    template<class Delegatee, class Check = for_observer<Delegatee>>
    void operator()(const Delegatee& ) const {
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
            while (!stop && !lifetime.is_stopped() && !out.lifetime.is_stopped()) {
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

template<class Strand>
struct shared_strand {
    using strand_type = decay_t<Strand>;
    template<class F>
    explicit shared_strand(F&& f) : st(forward<F>(f)) {}
    strand_type st;
    ~shared_strand() {
        info("shared_strand: destroy stop");
        st.lifetime.stop();
        info("shared_strand: destroy join");
        st.lifetime.join();
    }
};

template<class Strand>
struct shared_strand_maker {
    using strand_type = decay_t<Strand>;
    shared_ptr<shared_strand<strand_type>> ss;
    auto operator()(subscription lifetime) const {
        ss->st.lifetime.insert(lifetime);
        lifetime.insert([lifetime, l = ss->st.lifetime](){
            info("shared_strand_maker: erase");
            l.erase(lifetime);
        });
        return make_strand<clock_t<strand_type>>(lifetime, 
            [ss = this->ss, lifetime](auto at, auto o){
                lifetime.insert(o.lifetime);
                o.lifetime.insert([lifetime = o.lifetime, l = lifetime](){
                    l.erase(lifetime);
                });
                ss->st.defer_at(at, o);
            },
            [ss = this->ss](){return ss->st.now();});
    }
};


template<class Strand>
shared_strand_maker<Strand> make_shared_strand_maker(Strand&& s){
    using strand_type = decay_t<Strand>;
    return {make_shared<shared_strand<strand_type>>(forward<Strand>(s))};
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
subscription defer(strand<SN...> s, observer<ON...> out) {
    s.defer_at(s.now(), out);
    return out.lifetime;
}
template<class... SN, class... ON>
subscription defer_at(strand<SN...> s, clock_time_point_t<strand<SN...>> at, observer<ON...> out) {
    s.defer_at(at, out);
    return out.lifetime;
}
template<class... SN, class... ON>
subscription defer_after(strand<SN...> s, clock_duration_t<strand<SN...>> delay, observer<ON...> out) {
    s.defer_at(s.now() + delay, out);
    return out.lifetime;
}
template<class... SN, class... ON>
subscription defer_periodic(strand<SN...> s, clock_time_point_t<strand<SN...>> initial, clock_duration_t<strand<SN...>> period, observer<ON...> out) {
    auto lifetime = subscription{};
    auto state = make_state<pair<long, clock_time_point_t<strand<SN...>>>>(lifetime, make_pair(0, initial));
    s.defer_at(initial, make_observer(
        out,
        out.lifetime, 
        [state, period](const observer<ON...>& out, auto& self) mutable {
            auto& s = state.get();
            out.next(s.first++);
            s.second += period;
            self(s.second);
        }, detail::pass{}, detail::skip{}));
    return out.lifetime;
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
        auto& ref = s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
    }
    context(subscription lifetime, make_strand_type m, strand_type strand) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, strand)) {
        lifetime.insert(s.get().s.lifetime);
        auto& ref = this->s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
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
        auto& ref = s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
    }
    context(subscription lifetime, make_strand_type m, strand_type s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
        lifetime.insert(s.get().s.lifetime);
        auto& ref = this->s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
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
        auto& ref = s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
    }
    context(subscription lifetime, make_strand_type m, strand_type s) 
        : lifetime(lifetime)
        , m(m)
        , s(make_state<State>(lifetime, s)) {
        lifetime.insert(this->s.get().s.lifetime);
        auto& ref = this->s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
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
        auto& ref = s.get();
        lifetime.bind_defer([&](function<void()> target){
            defer(ref.s, make_observer(subscription{}, [target](auto& ){
                return target();
            }));
        });
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


template<class... CN, class... ON>
subscription defer(context<CN...> s, observer<ON...> out) {
    s.defer_at(s.now(), out);
    return out.lifetime;
}
template<class... CN, class... ON>
subscription defer_at(context<CN...> s, clock_time_point_t<context<CN...>> at, observer<ON...> out) {
    s.defer_at(at, out);
    return out.lifetime;
}
template<class... CN, class... ON>
subscription defer_after(context<CN...> s, clock_duration_t<context<CN...>> delay, observer<ON...> out) {
    s.defer_at(s.now() + delay, out);
    return out.lifetime;
}
template<class... CN, class... ON>
subscription defer_periodic(context<CN...> s, clock_time_point_t<context<CN...>> initial, clock_duration_t<context<CN...>> period, observer<ON...> out) {
    auto lifetime = subscription{};
    auto state = make_state<pair<long, clock_time_point_t<context<CN...>>>>(lifetime, make_pair(0, initial));
    s.defer_at(initial, make_observer(
        out,
        out.lifetime, 
        [state, period](const observer<ON...>& out, auto& self) mutable {
            auto& s = state.get();
            out.next(s.first++);
            s.second += period;
            self(s.second);
        }, detail::pass{}, detail::skip{}));
    return out.lifetime;
}

inline auto make_context(subscription lifetime) {
    auto c = context<>{
        lifetime
    };
    return c;
}

template<class Payload, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    auto c = context<Payload, detail::make_immediate<steady_clock>, steady_clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<steady_clock>{}
    };
    return c;
}

template<class Payload, class Clock, class... AN>
auto make_context(subscription lifetime, AN&&... an) {
    auto c = context<Payload, detail::make_immediate<Clock>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        detail::make_immediate<Clock>{}
    };
    return c;
}

template<class Payload, class Clock, class MakeStrand, class... AN>
auto make_context(subscription lifetime, MakeStrand&& m, AN&&... an) {
    auto c = context<Payload, decay_t<MakeStrand>, Clock>{
        lifetime,
        Payload(forward<AN>(an)...),
        forward<MakeStrand>(m)
    };
    return c;
}

template<class Clock, class MakeStrand, class C = void_t<typename Clock::time_point>>
auto make_context(subscription lifetime, MakeStrand&& m) {
    auto c = context<void, decay_t<MakeStrand>, Clock>{
        lifetime,
        forward<MakeStrand>(m)
    };
    return c;
}

template<class MakeStrand, class C = detail::for_strand<decltype(declval<MakeStrand>()(declval<subscription>()))>>
auto make_context(subscription lifetime, MakeStrand&& m) {
    auto c = context<void, decay_t<MakeStrand>, steady_clock>{
        lifetime,
        forward<MakeStrand>(m)
    };
    return c;
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
auto copy_context(subscription lifetime, NewMakeStrand&& makeStrand, const context<CN...>& ) {
    return make_context<clock_t<context<CN...>>>(lifetime, forward<NewMakeStrand>(makeStrand));
}

template<class C, class E>
auto copy_context(subscription lifetime, const context_interface<C, E>& o) {
    auto c = context_interface<C, E>{
        context<void, detail::make_strand_t<C, E>, C> {
            lifetime,
            o.m
        }
    };
    return c;
}

namespace detail {

template<class T>
using for_context = for_specialization_of_t<T, context>;

template<class T>
using not_context = not_specialization_of_t<T, context>;

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

struct joiner {    
};
joiner join() {
    return {};
}

/// \brief chain operator overload for
/// void = Subscription | Joiner
/// \param subscription
/// \param joiner
/// \returns void
void operator|(subscription s, joiner ) {
    s.join();
}

const auto intervals = [](auto makeStrand, auto initial, auto period){
    info("new intervals");
    return make_observable([=](auto scrb){
        info("intervals bound to subscriber");
        return make_starter([=](auto ctx) {
            info("intervals bound to context");
            subscription lifetime;
            ctx.lifetime.insert(lifetime);
            auto intervalcontext = copy_context(lifetime, makeStrand, ctx);
            auto r = scrb.create(ctx);
            info("intervals started");
            defer_periodic(intervalcontext, initial, period, r);
            return r.lifetime;
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
            for(auto i = first;!r.lifetime.is_stopped(); ++i){
                r.next(i);
                if (i == last) break;
            }
            r.complete();
            return r.lifetime;
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
            ctx.lifetime.insert(lifetime);
            auto outcontext = copy_context(ctx.lifetime, makeStrand, ctx);
            auto r = scbr.create(outcontext);
            return make_observer(r, lifetime, 
                [=](auto& r, auto v){
                    auto next = make_observer(r, subscription{}, [=](auto& r, auto& ){
                        r.next(v);
                    }, detail::pass{}, detail::skip{});
                    defer(outcontext, next);
                },
                [=](auto& r, auto e){
                    auto error = make_observer(r, subscription{}, [=](auto& r, auto& ){
                        r.error(e);
                    }, detail::pass{}, detail::skip{});
                    defer(outcontext, error);
                },
                [=](auto& r){
                    auto complete = make_observer(r, subscription{}, [=](auto& r, auto& ){
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
            ctx.lifetime.insert(lifetime);
            auto outcontext = copy_context(ctx.lifetime, makeStrand, ctx);
            auto r = scbr.create(outcontext);
            return make_observer(r, lifetime, 
                [=](auto& r, auto v){
                    auto next = make_observer(r, subscription{}, [=](auto& r, auto& ){
                        r.next(v);
                    }, detail::pass{}, detail::skip{});
                    defer_after(outcontext, delay, next);
                },
                [=](auto& r, auto e){
                    auto error = make_observer(r, subscription{}, [=](auto& r, auto& ){
                        r.error(e);
                    }, detail::pass{}, detail::skip{});
                    defer_after(outcontext, delay, error);
                },
                [=](auto& r){
                    auto complete = make_observer(r, subscription{}, [=](auto& r, auto& ){
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

const auto finally = [](auto f){
    info("new finally");
    return make_lifter([=](auto scbr){
        info("finally bound to subscriber");
        return make_subscriber([=](auto ctx){
            info("finally bound to context");
            auto r = scbr.create(ctx);
            r.lifetime.insert(f);
            return make_observer(r, ctx.lifetime);
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
                [last](auto& , auto v){
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
                    
                    auto sourcecontext = make_context(subscription{}, sharedmakestrand);

                    auto pending = make_state<set<subscription>>(ctx.lifetime);
                    pending.get().insert(sourcecontext.lifetime);

                    ctx.lifetime.insert([pending](){
                        info("merge-output stopping all inputs");
                        // stop all the inputs
                        for (auto l : pending.get()) {
                            l.stop();
                            l.join();
                        }
                        pending.get().clear();
                        info("merge-output stop");
                    });

                    auto destctx = copy_context(ctx.lifetime, sharedmakestrand, ctx);
                    auto r = scrb.create(destctx);

                    sourcecontext.lifetime.insert([pending, r, l = sourcecontext.lifetime](){
                        pending.get().erase(l);
                        if (pending.get().empty()){
                            info("merge-input complete destination");
                            r.complete();
                        }
                        info("merge-input stop");
                    });

                    return make_observer(r, sourcecontext.lifetime, 
                        [pending, sharedmakestrand](auto& r, auto& v){
                            info("merge-nested start");
                            auto nestedcontext = make_context(subscription{}, sharedmakestrand);
                            pending.get().insert(nestedcontext.lifetime);
                            nestedcontext.lifetime.insert([pending, r, l = nestedcontext.lifetime](){
                                pending.get().erase(l);
                                if (pending.get().empty()){
                                    info("merge-nested complete destination");
                                    r.complete();
                                }
                                info("merge-nested stop");
                            });
                            v |
                                observe_on(sharedmakestrand) |
                                make_subscriber([=](auto ctx){
                                    info("merge-nested bound to context");
                                    return make_observer(r, ctx.lifetime, 
                                        [](auto& r, auto& v){
                                            r.next(v);
                                        }, detail::pass{}, detail::skip{});
                                }) | 
                                start(nestedcontext);
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
            [=, &output](auto v) {
                ++values.get();
                defer(ctx, make_observer(subscription{}, [v, &output](auto& ){
                    output << v << endl;
                }));
            },
            [=, &output](exception_ptr ep){
                defer(ctx, make_observer(subscription{}, [ep, &output](auto& ){
                    output << what(ep) << endl;
                }));
            },
            [=, &output](){
                defer(ctx, make_observer(subscription{}, [=, &output](auto& ){
                    output << values.get() << " values received - done!" << endl;
                }));
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


template<class Clock, class Observer>
struct observe_at;

template<class Clock, class... ON>
struct observe_at<Clock, observer<ON...>>
{
    using clock_type = decay_t<Clock>;
    using observer_type = observer<ON...>;

    observe_at(time_point<clock_type> when, observer_type o)
        : when(when)
        , what(std::move(o))
    {
    }
    time_point<clock_type> when;
    observer_type what;
};


// Sorts observe_at items in priority order sorted
// on value of observe_at.when. Items with equal
// values for when are sorted in fifo order.

template<class Clock, class Observer>
class observe_at_queue;

template<class Clock, class... ON>
class observe_at_queue<Clock, observer<ON...>> {
public:
    using clock_type = decay_t<Clock>;
    using observer_type = observer<ON...>;
    using item_type = observe_at<clock_type, observer_type>;
    using elem_type = std::pair<item_type, int64_t>;
    using container_type = std::vector<elem_type>;
    using const_reference = const item_type&;

private:
    struct compare_elem
    {
        bool operator()(const elem_type& lhs, const elem_type& rhs) const {
            if (lhs.first.when == rhs.first.when) {
                return lhs.second > rhs.second;
            }
            else {
                return lhs.first.when > rhs.first.when;
            }
        }
    };

    typedef std::priority_queue<
        elem_type,
        container_type,
        compare_elem
    > queue_type;

    queue_type q;

    int64_t ordinal;
public:
    const_reference top() const {
        return q.top().first;
    }

    void pop() {
        q.pop();
    }

    bool empty() const {
        return q.empty();
    }

    void push(const item_type& value) {
        q.push(elem_type(value, ordinal++));
    }

    void push(item_type&& value) {
        q.push(elem_type(std::move(value), ordinal++));
    }
};


template<class Clock = steady_clock, class Error = exception_ptr>
struct run_loop {
    using clock_type = decay_t<Clock>;
    using error_type = decay_t<Error>;
    using lock_type = mutex;
    using guard_type = unique_lock<lock_type>;
    using observer_type = observer_interface<detail::re_defer_at_t<clock_type>, error_type>;
    using item_type = observe_at<clock_type, observer_type>;
    using queue_type = observe_at_queue<clock_type, observer_type>;

    struct guarded_loop {
        ~guarded_loop() {
            info(to_string(reinterpret_cast<ptrdiff_t>(this)) + " - run_loop: guarded_loop destroy");
        }
        lock_type lock;
        condition_variable wake;
        queue_type deferred;
    };

    subscription lifetime;
    state<guarded_loop> loop;
    
    explicit run_loop(subscription l) 
        : lifetime(l)
        , loop(make_state<guarded_loop>(lifetime)) {
        lifetime.insert([loop = this->loop](){
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: stop notify_all");
            guard_type guard(loop.get().lock);
            loop.get().wake.notify_all();
        });
    }
    ~run_loop(){
        info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: destroy");
    }
    
    bool is_ready(guard_type& guard) const {
        if (!guard.owns_lock()) { 
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: is_ready caller must own lock!");
            abort(); 
        }
        auto& deferred = loop.get().deferred;
        return !deferred.empty() && deferred.top().when <= clock_type::now();
    }

    bool wait(guard_type& guard) const {
        if (!guard.owns_lock()) { 
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait caller must own lock!");
            abort(); 
        }
        auto& deferred = loop.get().deferred;
        info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait");
        if (!loop.lifetime.is_stopped()) {
            if (!deferred.empty()) {
                info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait_until top when");
                loop.get().wake.wait_until(guard, deferred.top().when, [&](){
                    bool r = is_ready(guard) || loop.lifetime.is_stopped();
                    info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait wakeup is_ready - " + to_string(r));
                    return r;
                });
            } else {
                info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait for notify");
                loop.get().wake.wait(guard, [&](){
                    bool r = !deferred.empty() || loop.lifetime.is_stopped();
                    info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wait wakeup is_ready - " + to_string(r));
                    return r;
                });
            }
        }
        info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: wake");
        return !loop.lifetime.is_stopped();
    }

    void call(guard_type& guard, item_type& next) const {
        if (guard.owns_lock()) { 
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: call caller must not own lock!");
            abort(); 
        }
        info("run_loop: call");
        auto& deferred = loop.get().deferred;
        bool complete = true;
        next.what.next([&](time_point<clock_type> at){
            unique_lock<guard_type> nestedguard(guard);
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: call self");
            if (lifetime.is_stopped() || next.what.lifetime.is_stopped()) return;
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: call push self");
            next.when = at;
            deferred.push(next);
            complete = false;
        });
        if (complete) {
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: call complete");
            next.what.complete();
        }
    }

    void step(guard_type& guard) const {
        if (!guard.owns_lock()) { 
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: step caller must own lock!");
            abort(); 
        }
        auto& deferred = loop.get().deferred;
        while (!loop.lifetime.is_stopped() && is_ready(guard)) {
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: step");

            auto next = move(deferred.top());
            deferred.pop();
            
            guard.unlock();
            call(guard, next);
            guard.lock();
        }
    }
    
    void run() const {
        guard_type guard(loop.get().lock);
        info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: run");
        while (wait(guard)) {
            step(guard);
        }
        info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: exit");
    }

    struct strand {
        subscription lifetime;
        state<guarded_loop> loop;
        
        template<class... OON>
        void operator()(time_point<clock_type> at, observer<OON...> out) const {
            guard_type guard(loop.get().lock);
            lifetime.insert(out.lifetime);
            out.lifetime.insert([lifetime = this->lifetime, l = out.lifetime](){lifetime.erase(l);});
            loop.get().deferred.push(item_type{at, out});
            info(to_string(reinterpret_cast<ptrdiff_t>(addressof(loop.get()))) + " - run_loop: defer_at notify_all");
            loop.get().wake.notify_all();
        }
    };

    auto make() const {
        return [loop = this->loop](subscription lifetime) {
            lifetime.insert(loop.lifetime);
            return make_strand<clock_type>(lifetime, strand{lifetime, loop}, detail::now<clock_type>{});
        };
    }
};

struct threadjoin
{
    thread worker;
    function<void()> notify;
    template<class W, class N>
    threadjoin(W&& w, N&& n) 
        : worker(forward<W>(w))
        , notify(forward<N>(n)) {
        worker.detach();
    }
    ~threadjoin(){
        info("threadjoin: destroy notify");
        notify();
    }
    
};

template<class Strand>
struct new_thread {
    using strand_type = decay_t<Strand>;

    subscription lifetime;
    strand_type strand;
    state<threadjoin> worker;
    
    new_thread(strand_type&& s, state<threadjoin>&& t) 
        : lifetime(s.lifetime)
        , strand(move(s))
        , worker(move(t)) {
    }
    
    template<class At, class... ON>
    void operator()(At at, observer<ON...> out) const {
        strand.defer_at(at, out);
    }
};

template<class Clock = steady_clock, class Error = exception_ptr>
struct make_new_thread {
    auto operator()(subscription lifetime) const {
        info("new_thread: create");
        run_loop<Clock, Error> loop(subscription{});
        auto strand = loop.make()(lifetime);
        auto t = make_state<threadjoin>(lifetime, [=](){loop.run();}, [l = loop.lifetime](){l.stop(); l.join();});
        return make_strand<Clock>(lifetime, new_thread<decltype(strand)>(move(strand), move(t)), detail::now<Clock>{});
    }
};

}

extern"C" {
    void designcontext(int, int);
}

auto twointervals(long c) {
    using namespace std::chrono;

    using namespace rx;
    using rx::transform;

    auto period = 700ms + (c * 10ms);
    return intervals(make_new_thread<>{}, steady_clock::now() + period, period) |
        take(2) |
        transform([=](long n){return 700 + (c * 10) + n;}) |
        as_interface<long>();
}

void designcontext(int first, int last){
    using namespace std::chrono;

    using namespace rx;
    using rx::copy_if;
    using rx::transform;
    using rx::merge;

    [](int, int) {} (first, last);

auto makeStrand = rx::detail::make_immediate<>{};

auto strand = makeStrand(subscription{});

strand.now();

#if 1
{
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer(strand, make_observer(defer_lifetime, [](auto& ){
        cout << this_thread::get_id() << " - deferred immediate strand" << endl;
    }));
}

{
    auto ctx = copy_context(subscription{}, makeStrand, start<shared_ptr<destruction>>(make_shared<destruction>()));
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer(ctx, make_observer(defer_lifetime, [](auto& ){
        cout << this_thread::get_id() << " - deferred immediate context" << endl;
    }));
}

auto sharedmakestrand = make_shared_make_strand(makeStrand);

{
    auto sharedstrand = sharedmakestrand(subscription{});
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer(sharedstrand, make_observer(defer_lifetime, [](auto& ){
        cout << this_thread::get_id() << " - deferred shared-immediate strand" << endl;
    }));
}

{
    auto ctx = copy_context(subscription{}, sharedmakestrand, start<shared_ptr<destruction>>(make_shared<destruction>()));
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer(ctx, make_observer(defer_lifetime, [](auto& ){
        cout << this_thread::get_id() << " - deferred shared-immediate context" << endl;
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
#endif

auto makeThread = make_shared_make_strand(make_new_thread<>{});

auto thread = makeThread(subscription{});

#if 1

{
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer(thread, make_observer(defer_lifetime, [](auto& ){
        cout << this_thread::get_id() << " - deferred thread strand" << endl;
    })).join();
}
this_thread::sleep_for(1s);
cout << endl;
#endif

#if 1
{
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer_periodic(thread, thread.now(), 1s, make_observer(defer_lifetime, [=](long c){
        cout << this_thread::get_id() << " - deferred thread strand periodic - " << c << endl;
        if (c > 2) {
            defer_lifetime.stop();
        }
    })).join();
}
this_thread::sleep_for(2s);
cout << endl;
#endif

#if 1
{
    auto c = make_context<steady_clock>(subscription{}, makeThread);
    auto defer_lifetime = subscription{};
    make_state<shared_ptr<destruction>>(defer_lifetime, make_shared<destruction>());
    defer_periodic(c, c.now(), 1s, make_observer(defer_lifetime, [=](long c){
        cout << this_thread::get_id() << " - deferred thread context periodic - " << c << endl;
        if (c > 2) {
            defer_lifetime.stop();
        }
    })).join();
}
this_thread::sleep_for(2s);
cout << endl;
#endif

#if 1
{
 cout << "intervals" << endl;
    auto threeeven = copy_if(even) | 
        take(3) |
        delay(makeThread, 1s);

    intervals(makeThread, steady_clock::now() + 1s, 1s) | 
        threeeven |
        as_interface<long>() |
        finally([](){cout << "caller stopped" << endl;}) |
        printto(cout) |
        start<shared_ptr<destruction>>(subscription{}, make_shared<destruction>()) |
        join();
}
this_thread::sleep_for(2s);
cout << endl;
#endif

#if 1
{
 cout << "merged multi-thread intervals" << endl;

    // intervals(makeThread, steady_clock::now() + 1s, 1s) | 
    //     take(5) |
    //     transform_merge(makeThread, twointervals) |
    ints(1, 5) |
        transform_merge(make_new_thread<>{}, twointervals) |
        as_interface<long>() |
        finally([](){cout << "caller stopped" << endl;}) |
        printto(cout) |
        start<shared_ptr<destruction>>(subscription{}, make_shared<destruction>()) |
        join();
}
this_thread::sleep_for(2s);
cout << endl;
#endif

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

{
 cout << "transform_merge new_thread" << endl;
 auto t0 = high_resolution_clock::now();

    ints(first, last) | 
        transform_merge(make_new_thread<>{}, 
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
        as_interface<int>() |
        make_subscriber() |
        start() |
        join();

 auto t1 = high_resolution_clock::now();
 auto d = duration_cast<milliseconds>(t1-t0).count() * 1.0;
 auto sc = last - first;
 cout << d / sc << " ms per subscription\n"; 
 auto s = d / 1000.0;
 cout << sc / s << " subscriptions per second\n"; 
}
this_thread::sleep_for(1s);
cout << endl;

#endif

}
