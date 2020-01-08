#include "http_server.h"

class RequestHandler : public Handler {
public:
    bool Accept(Request& req) override {
        if (req.has_content_length()) {
            auto length = std::atoll(req.at(http::field::content_length).begin());
            if (length >= 4 * 1024)
                req.body() = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
        }
        return true;
    }

    void Handle(Session* session) override {
        auto& req = session->GetRequest();

        if (req.target() == "/timer") {
            if (timer_.joinable()) timer_.join();

            timer_ = std::thread{[session = session->shared_from_this()]() {
                Response res{http::status::ok, session->GetRequest().version()};
                res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
                res.chunked(true);
                session->Send(std::move(res));

                std::string str;
                for (int i = 0; i < 10; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    str = std::to_string(i) + "\r\n";

                    session->SendChunk(boost::optional<boost::asio::const_buffer>(boost::asio::buffer(str)));
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
                session->SendChunk(boost::none);
            }};
        } else {
            Response res{http::status::not_found, req.version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res.prepare_payload();
            session->Send(std::move(res));
        }
    }

private:
    std::thread timer_;
};

int main(int argc, char* argv[]) {
    RequestHandler handler;

    boost::asio::io_context ioc;

    const auto port = unsigned short{8000};
    const auto address = boost::asio::ip::make_address("0.0.0.0");
    std::make_shared<Server>(&handler, ioc, tcp::endpoint{address, port})->Run();

    ioc.run();

    return 0;
}
