// The control page for an iPad or phone on the same network: open the address Barline shows,
// enter the pairing code once, and bookmark it (or Add to Home Screen). It answers only devices on
// a private network (home, venue, or the laptop's own hotspot) and never talks to the internet.
#pragma once
#include <memory>
#include <string>

#include "app.h"

namespace web {

class Page {
public:
    explicit Page(app::Control &ctl);
    ~Page();
    // starts serving; returns the address to open ("http://192.168.1.20:8580"), or "" (and err)
    std::string start(std::string *err);
    void stop();
    int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace web
