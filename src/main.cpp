#include <array>
#include <cassert>
#include <fstream>
#include <unordered_map>

#include <istream>
#include <ostream>
#include <boost/asio.hpp>

namespace alicia {

  //! Appy XORcodec to buffer.
  //!
  //! @param buffer Buffer.
  //! @returns XORcoded buffer.
  template <typename Buffer> void xor_codec_cpp(Buffer& buffer)
  {
    const std::array control{
        static_cast<std::byte>(0xCB),
        static_cast<std::byte>(0x91),
        static_cast<std::byte>(0x01),
        static_cast<std::byte>(0xA2),
    };

    for(size_t idx = 0; idx < buffer.size(); idx++) {
      const auto shift = idx % 4;
      buffer[idx] ^= control[shift];
    }
  }

  /**
   * Encode message information.
   *
   * @param message_id  Message identifier.
   * @param message_jumbo Message jumbo.
   * @param message_data_length Message data length.
   * @param buffer_size Message data buffer size.
   * @return Encoded message information.
   */
  uint32_t encode_message_information(
      uint16_t message_id, uint16_t message_jumbo, uint16_t message_data_length, uint16_t buffer_size = 4092)
  {
    uint32_t length = buffer_size << 16 | message_data_length;
    uint32_t val = length;
    length = length & 16383 | length << 14;
    const uint16_t magic = (length & 15 | 65408) << 8 | val >> 4 & 255 | length & 61440;

    message_id = message_jumbo & 0xFFFF | message_id & 0xFFFF;
    uint32_t encoded = magic;
    encoded |= ((magic ^ message_id) << 16);
    return encoded;
  }

  /**
   * Decode message length from message information.
   *
   * @param data Message information to decode.
   * @return Decoded message length.
   */
  uint32_t decode_message_length(uint32_t data)
  {
    if(data & (1 << 15)) {
      const uint16_t section = data & 16383;
      return (data & 255) << 4 | section >> 8 & 15 | section & 61440;
    }
    return data;
  }

} // namespace alicia

void test_magic()
{
  const uint16_t message_length = 29;

  // Test encoding of message information.
  const auto encoded_message_length = alicia::encode_message_information(message_length, 7, 16384);
  assert(encoded_message_length == 0x8D06CD01);

  // Test decoding of message length from message information.
  const auto decoded_message_length = alicia::decode_message_length(encoded_message_length);
  assert(decoded_message_length == message_length);
}

namespace asio = boost::asio;

namespace alicia {

  template <typename ValType> void read(std::istream& stream, ValType& val)
  {
    stream.read(reinterpret_cast<char*>(&val), sizeof(val));
  }

  void read(std::istream& stream, std::string& val)
  {
    while(true) {
      char v{0};
      stream.read(&v, sizeof(v));
      if(v == 0)
        break;
      val += v;
    }
  }

  template <typename ValType> void read(std::istream& stream, std::vector<ValType>& val)
  {
    for(auto& v : val) {
      read(stream, v);
    }
  }

  std::array message_table = {0, 1};

  class AcCmdCLLogin {
  public:
    uint16_t constant0;
    uint16_t constant1;
    std::string login_id;
    uint32_t member_no;
    std::string auth_key;

    void Serialize(std::istream& buffer)
    {
      read(buffer, constant0);
      read(buffer, constant1);
      read(buffer, login_id);
      read(buffer, member_no);
      read(buffer, auth_key);
    }
  };

  struct AcCmdCLLoginOK {};

  struct AcCmdCLLoginCancel {};

  /**
   * Client
   */
  class Client {
  public:
    explicit Client(asio::ip::tcp::socket&& socket) noexcept : _socket(std::move(socket)) {}

  public:
    void read_loop()
    {
      _socket.async_read_some(_buffer.prepare(4096), [&](boost::system::error_code error, std::size_t size) {
        if(error) {
          printf(
              "Error occurred on read loop with client on port %d. What: %s\n",
              _socket.remote_endpoint().port(),
              error.message().c_str());
          return;
        }

        // Commit the recieved buffer.
        _buffer.commit(size);
        std::istream stream(&_buffer);

        // Read the message magic.
        uint32_t magic = 0;
        read(stream, magic);
        if(magic == 0x0) {
          throw std::runtime_error("invalid message magic");
        }

        const auto message_length = decode_message_length(magic);

        // Read the message data.
        std::vector<std::byte> data;
        data.resize(message_length);
        read(stream, data);

        // XOR decode
        xor_codec_cpp(data);

        asio::streambuf data_buf;
        data_buf.prepare(data.size());
        std::ostream data_buf_o(&data_buf);

        for(auto value : data) {
          data_buf_o << bit_cast<uint8_t>(value);
        }

        std::istream data_stream(&data_buf);

        AcCmdCLLogin login;
        login.Serialize(data_stream);
        printf("AcCmdCLLogin: \n");
        printf("\tconstant0: %d\n", login.constant0);
        printf("\tconstant1: %d\n", login.constant1);
        printf("\tLogin ID: %s\n", login.login_id.c_str());
        printf("\tMember NO.: %d\n", login.member_no);
        printf("\tAuth Key: %s\n", login.auth_key.c_str());

        std::array<std::byte, 5> test;
        *reinterpret_cast<uint32_t*>(test.data()) = encode_message_information(9, 16384, 5);
        test[4] = static_cast<std::byte>(3);
        _socket.write_some(boost::asio::const_buffer(test.data(), 5));

        read_loop();
      });
    }

  private:
    asio::ip::tcp::socket _socket;
    asio::streambuf _buffer;
  };

  /**
   * Server
   */
  class Server {
  public:
    Server() : _acceptor(_io_ctx) {}

  public:
    void host()
    {
      asio::ip::tcp::endpoint server_endpoint(asio::ip::tcp::v4(), 10030);
      printf("Hosting the server on port 10030\n");
      _acceptor.open(server_endpoint.protocol());
      _acceptor.bind(server_endpoint);
      _acceptor.listen();
      accept_loop();
    }

    void run() { _io_ctx.run(); }

  private:
    void accept_loop()
    {
      _acceptor.async_accept([&](boost::system::error_code error, asio::ip::tcp::socket client_socket) {
        printf("Accepted new client from port %d\n", client_socket.remote_endpoint().port());
        const auto [itr, _] = _clients.emplace(client_id++, std::move(client_socket));
        itr->second.read_loop();

        accept_loop();
      });
    }

  private:
    asio::io_context _io_ctx;
    asio::ip::tcp::acceptor _acceptor;

    uint32_t client_id = 0;
    std::unordered_map<uint32_t, Client> _clients;
  };

} // namespace alicia

int main()
{
  alicia::Server server;
  server.host();
  server.run();
  return 0;
}