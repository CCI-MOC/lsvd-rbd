#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/system/detail/error_code.hpp>
#include <string>

using boost::asio::ip::tcp;

namespace repl
{
class tcp_connection : public boost::enable_shared_from_this<tcp_connection>
{
  public:
    typedef boost::shared_ptr<tcp_connection> pointer;

    static pointer create(boost::asio::io_context &io_context)
    {
        return pointer(new tcp_connection(io_context));
    }

    tcp::socket &socket() { return socket_; }

    void start() {}

  private:
    tcp_connection(boost::asio::io_context &io_context) : socket_(io_context) {}

    void handle_write(const boost::system::error_code & /*error*/,
                      size_t /*bytes transferred*/)
    {
    }
    void handle_read(const boost::system::error_code & /*error*/,
                     size_t /*bytes transferred*/)
    {
    }

    tcp::socket socket_;
    std::string message_;
};

class repl_server
{

  private:
    boost::asio::io_context &io_context;
    tcp::acceptor acceptor;

    void start_accept()
    {
        tcp_connection::pointer new_connection =
            tcp_connection::create(io_context);

        acceptor.async_accept(new_connection->socket(),
                              boost::bind(&repl_server::handle_accept, this,
                                          new_connection,
                                          boost::asio::placeholders::error));
    }

    void handle_accept(tcp_connection::pointer new_connection,
                       const boost::system::error_code &error)
    {
        if (!error) {
            new_connection->start();
        }
        start_accept();
    }

  public:
    repl_server(boost::asio::io_context &io_context, int port)
        : io_context(io_context),
          acceptor(io_context, tcp::endpoint(tcp::v4(), port))
    {
    }
};
} // namespace repl
