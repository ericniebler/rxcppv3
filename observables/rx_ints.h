#pragma once

namespace rx {

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
            return ctx.lifetime;
        });
    });
};

}