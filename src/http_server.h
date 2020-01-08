#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace errc = boost::system::errc;

template <class... Ts>
struct Visitor : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Visitor(Ts...)->Visitor<Ts...>;

class Body {
public:
    using value_type = std::variant<std::string, boost::filesystem::path>;

    static std::uint64_t size(const value_type& body) {
        return std::visit(Visitor{[](const std::string& value) -> std::uint64_t { return value.size(); },
                                  [](const boost::filesystem::path& value) -> std::uint64_t {
                                      boost::system::error_code error;
                                      return boost::filesystem::file_size(value, error);
                                  }},
                          body);
    }

    class reader {
    public:
        template <bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields>&, value_type& body) : body_(body) {}

        void init(const boost::optional<std::uint64_t>& length, boost::system::error_code& error) {
            if (!length) {
                error = errc::make_error_code(errc::not_supported);
                return;
            }

            std::visit(Visitor{[&](std::string& value) {},
                               [&](boost::filesystem::path& value) {
                                   file_.open(value, std::ios::binary);
                                   if (!file_) error = errc::make_error_code(errc::io_error);
                               }},
                       body_);
        }

        std::size_t put(const boost::asio::const_buffer& buffer, boost::system::error_code& error) {
            error = {};
            return std::visit(Visitor{[&](std::string& value) -> std::size_t {
                                          value.append(boost::asio::buffer_cast<const char*>(buffer), buffer.size());
                                          return buffer.size();
                                      },
                                      [&](boost::filesystem::path& value) -> std::size_t {
                                          if (!file_) {
                                              error = errc::make_error_code(errc::io_error);
                                              return 0;
                                          }
                                          file_.write(boost::asio::buffer_cast<char const*>(buffer), buffer.size());
                                          return buffer.size();
                                      }},
                              body_);
        }

        void finish(boost::system::error_code& error) { error = {}; }

    private:
        value_type& body_;
        boost::filesystem::ofstream file_;
    };

    class writer {
    public:
        using const_buffers_type = boost::asio::const_buffer;

        template <bool isRequest, class Fields>
        explicit writer(const http::header<isRequest, Fields>&, const value_type& body) : body_(body) {}

        void init(boost::system::error_code& error) {
            error = {};
            std::visit(Visitor{[&](const std::string& value) {},
                               [&](const boost::filesystem::path& value) {
                                   buffer_.resize(4096);
                                   file_.open(value, std::ios::binary);
                                   if (!file_) error = errc::make_error_code(errc::io_error);
                               }},
                       body_);
        }

        template <class Buffer = boost::optional<std::pair<boost::asio::const_buffer, bool>>>
        Buffer get(boost::system::error_code& error) {
            error = {};
            return std::visit(Visitor{[&](const std::string& value) -> Buffer {
                                          return {{boost::asio::buffer(value), false}};
                                      },
                                      [&](const boost::filesystem::path&) -> Buffer {
                                          if (file_) {
                                              file_.read(buffer_.data(), buffer_.size());
                                              buffer_.resize(file_.gcount());
                                              return {{boost::asio::buffer(buffer_), file_.good()}};
                                          } else {
                                              error = errc::make_error_code(errc::io_error);
                                              return {};
                                          }
                                      }},
                              body_);
        }

    private:
        const value_type& body_;
        boost::filesystem::ifstream file_;
        std::vector<char> buffer_;
    };
};

using Request = http::request<Body>;
using Response = http::response<Body>;

class Handler {
public:
    virtual bool Accept(Request&) { return true; }
    virtual void Handle(class Session* session) = 0;
};

class Session : public std::enable_shared_from_this<Session> {
    using RequestParser = http::request_parser<Body>;
    using ResponseSerializer = http::response_serializer<Body>;

public:
    explicit Session(Handler* handler, tcp::socket&& socket) : handler_(handler), socket_(std::move(socket)) {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    auto& GetRequest() const { return parser_->get(); }

    void Run() { ReadHeader(); }

    void Send(Response&& response) {
        Send(std::move(response), std::bind(&Session::OnSend, shared_from_this(), std::placeholders::_1));
    }

    void SendChunk(boost::optional<boost::asio::const_buffer> data) {
        if (data) {
            boost::asio::async_write(socket_, http::make_chunk(*data),
                                     std::bind(&Session::OnSendChunk, shared_from_this(), std::placeholders::_1));
        } else {
            boost::asio::async_write(socket_, http::make_chunk_last(),
                                     std::bind(&Session::OnSend, shared_from_this(), std::placeholders::_1));
        }
    }

private:
    template <typename SendHandler>
    void Send(Response&& response, SendHandler&& handler) {
        response_ = std::make_unique<Response>(std::move(response));
        close_ = response_->need_eof();

        if (response_->chunked()) {
            serializer_ = std::make_unique<ResponseSerializer>(*response_);
            http::async_write_header(socket_, *serializer_, std::move(handler));
        } else {
            http::async_write(socket_, *response_, std::move(handler));
        }
    }

    void ReadHeader() {
        parser_ = std::make_unique<RequestParser>();
        http::async_read_header(socket_, read_buffer_, *parser_,
                                std::bind(&Session::OnHeader, shared_from_this(), std::placeholders::_1));
    }

    void OnHeader(const boost::system::error_code& error) {
        if (error || !handler_->Accept(parser_->get())) return;
        if (parser_->get()[http::field::expect] == "100-continue") {
            Response res{http::status::continue_, parser_->get().version()};
            res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
            Send(std::move(res), std::bind(&Session::ReadRequest, shared_from_this()));
        } else {
            ReadRequest();
        }
    }

    void ReadRequest() {
        http::async_read(socket_, read_buffer_, *parser_,
                         std::bind(&Session::OnRequest, shared_from_this(), std::placeholders::_1));
    }

    void OnRequest(const boost::system::error_code& error) {
        if (error) return;
        handler_->Handle(this);
    }

    void OnSend(const boost::system::error_code& error) {
        if (error || close_) return;
        ReadHeader();
    }

    void OnSendChunk(const boost::system::error_code& error) {
        if (error) return;
    }

private:
    Handler* handler_;
    tcp::socket socket_;
    boost::beast::flat_buffer read_buffer_;
    std::unique_ptr<RequestParser> parser_;
    std::unique_ptr<Response> response_;
    std::unique_ptr<ResponseSerializer> serializer_;
    bool close_ = true;
};

class Server : public std::enable_shared_from_this<Server> {
public:
    Server(Handler* handler, boost::asio::io_context& ioc, tcp::endpoint endpoint)
        : handler_(handler), acceptor_(ioc), socket_(ioc) {
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::socket::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(tcp::socket::max_listen_connections);
    }

    void Run() {
        if (!acceptor_.is_open()) return;
        Accept();
    }

    void Accept() {
        acceptor_.async_accept(socket_, std::bind(&Server::OnAccept, shared_from_this(), std::placeholders::_1));
    }

    void OnAccept(const boost::system::error_code& error) {
        if (error) return;
        std::make_shared<Session>(handler_, std::move(socket_))->Run();
        Accept();
    }

private:
    Handler* handler_;
    tcp::acceptor acceptor_;
    tcp::socket socket_;
};
