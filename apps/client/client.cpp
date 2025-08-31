#include <iostream>
#include <protocol/http.h>
#include <random>
#include <test_type.h>

int main() {
    using namespace m::net::rpc;
    using namespace std;
    using namespace MyTypeList;
    Client cli;
    cli.dial("/*server address*/ : /*server ip*/");
    cout << cli.call<string, string>("echo", "i am client, rpc okay").value() << endl;

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(1.0, 20.0);

    for (int i = 0; i < 13; ++i) {
        auto res = cli.call<Arg, string>("calculate", Arg{dist(rng), static_cast<int>(dist(rng))});
        if (res.has_value()) cout << res.value() << '\n';
        else cout << "rpc faild\n";
        }
    }