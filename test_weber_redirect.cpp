// Pure URI/policy regression using the real WinHTTP URL parser. No network,
// certificate bypass, cookies or live account data are involved in this test.
#include "weber_gateway.h"
#include <iostream>
#include <stdexcept>

static void expect(const char *base, const char *location, const char *wanted) {
    std::string actual;
    const bool allowed = simon_weber::redirect_target(base, location, actual);
    if ((wanted == nullptr && allowed) || (wanted && (!allowed || actual != wanted)))
        throw std::runtime_error("redirect policy regression");
}

int main() {
    try {
        expect("https://example.test/user/login?password=SYNTHETIC", "http://example.test/user/guideMenu",
            "https://example.test/user/guideMenu");
        expect("https://EXAMPLE.test:443/login?password=SYNTHETIC", "http://example.test:80/next?group=1",
            "https://example.test/next?group=1");
        expect("https://example.test/login", "http://other.test/next", nullptr);
        expect("https://example.test/login", "http://example.test:8080/next", nullptr);
        expect("https://example.test:444/login", "http://example.test/next", nullptr);
        expect("https://example.test/login", "http://example.test@other.test/next", nullptr);
        expect("https://example.test/login", "http://other.test@example.test/next", nullptr);
        expect("https://example.test/login", "http://example.test.evil/next", nullptr);
        expect("https://example.test/login", "http://example.test\\@other.test/next", nullptr);
        expect("https://example.test/login", "http://example.test/next\r\nInjected:1", nullptr);
        expect("https://example.test/login", "file:///C:/private", nullptr);
        expect("https://example.test/login", "http://example.test/next?new=only#fragment",
            "https://example.test/next?new=only");
        expect("https://example.test/user/login?password=SYNTHETIC", "../guideMenu",
            "https://example.test/guideMenu");
        expect("https://example.test/user/login?password=SYNTHETIC", "?language=EN",
            "https://example.test/user/login?language=EN");
        expect("https://example.test/user/login?password=SYNTHETIC", "/guideMenu",
            "https://example.test/guideMenu");
        expect("https://example.test/user/login", "//other.test/guideMenu",
            "https://other.test/guideMenu");
        expect("http://example.test/login", "https://example.test/next", "https://example.test/next");
        expect("http://example.test/a/b", "../../../../done", "http://example.test/done");
        expect("https://example.test/a/b", "./c/../d?next=%2F", "https://example.test/a/d?next=%2F");
        expect("https://example.test/a/b", "/literal/%2e%2e/d", "https://example.test/literal/%2e%2e/d");
        expect("https://example.test/a/b", "/double//slash", "https://example.test/double//slash");
        std::cout << "PASS strict redirect policy: HTTPS canonical upgrade, origin/port boundaries, query replacement and URI paths\n";
    } catch (...) {
        std::cerr << "FAIL redirect policy\n";
        return 1;
    }
}
