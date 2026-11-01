#include "sandbox/session_wire.hpp"
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    try {
        std::string input((std::istreambuf_iterator<char>(std::cin)), {});
        std::string op = argv[1];
        if (op == "decode-request") std::cout << sandbox::session_wire::decode_request(input).dump();
        else if (op == "decode-response") std::cout << sandbox::session_wire::decode_response(input).dump();
        else if (op == "encode-request") std::cout << sandbox::session_wire::encode_request(sandbox::Json::parse(input));
        else if (op == "encode-response") {
            auto value = sandbox::Json::parse(input);
            std::cout << sandbox::session_wire::encode_response(
                value.at("request").at("op").get<std::string>(), value.at("answer"));
        } else return 2;
    } catch (const std::exception &e) { std::cerr << e.what(); return 1; }
}
