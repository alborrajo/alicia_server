#include <array>
#include <cassert>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "commands.hpp"

namespace alicia {

  enum MessageType {
    Serverbound, // Client to server
    Clientbound  // Server to client
  };

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
      uint16_t message_id, uint16_t message_jumbo, uint16_t message_data_length, uint16_t buffer_size = 0xFFC)
  {
    uint32_t length = buffer_size << 16 | message_data_length;
    uint32_t val = length;
    length = length & 0x3FFF | length << 14;
    const uint16_t magic = (length & 15 | 0xFF80) << 8 | val >> 4 & 0xFF | length & 0xF000;

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
  bool decode_message_information(uint32_t data, uint16_t *out_message_id, uint16_t *out_message_data_length)
  {
    if(data & (1 << 15)) {
      const uint16_t section = data & 0x3FF;
      if(out_message_data_length != nullptr)
      {
        *out_message_data_length = (data & 255) << 4 | section >> 8 & 15 | section & 61440;
      }

      const uint16_t *uintArray = (uint16_t*)(&data);
      const uint16_t firstTwoBytes = uintArray[0];
      const uint16_t secondTwoBytes = uintArray[1];
      const uint16_t xorResult = firstTwoBytes^secondTwoBytes;
      if(out_message_id != nullptr)
      {
        *out_message_id = ~(xorResult & 0xC000) & xorResult;
      }
      return true;
    }
    return false;
  }

  void log_message(boost::asio::ip::tcp::socket &socket, MessageType message_type, const std::vector<std::byte>& message)
  {
    const uint32_t encoded = *reinterpret_cast<const uint32_t*>(message.data());
    uint16_t decoded_message_id, decoded_message_length;
    alicia::decode_message_information(encoded, &decoded_message_id, &decoded_message_length);
    printf("Received %s (ID %d) from %s:%u (Length %u. %uB in total).", GetCommandName(decoded_message_id).c_str(), decoded_message_id, socket.remote_endpoint().address().to_string().c_str(), socket.remote_endpoint().port(), decoded_message_length, message.size());
    int column = 0;
    for (int i = 4; i < message.size(); ++i) {
      switch((i-4)%16)
      {
        case 0:
          printf("\n\t");
          break;
        case 8:
          printf(" ");
          break;
      }
      printf(" %02X", message[i]);
    }
    printf("\n");
  }

} // namespace alicia

void test_magic()
{
  const uint16_t message_length = 29;

  // Test encoding of message information.
  const auto encoded_message_length = alicia::encode_message_information(message_length, 7, 16384);
  assert(encoded_message_length == 0x8D06CD01);

  // Test decoding of message length from message information.
  uint16_t decoded_message_length = 0;
  alicia::decode_message_information(encoded_message_length, nullptr, &decoded_message_length);
  assert(decoded_message_length == message_length);
}

namespace asio = boost::asio;
namespace alicia::protocol
{
  /**
   * Client
   */
  class Client
  {
  public:
    explicit Client(asio::ip::tcp::socket&& socket) noexcept : _socket(std::move(socket))
    {}
  public:
    void read_loop()
    {
      _socket.async_read_some(
        asio::mutable_buffer(_buffer.data(), _buffer.size()),
        [&](boost::system::error_code error, std::size_t size)
          {
            if (error)
            {
              printf(
                "Error occurred on read loop with client %s:%d. What: %s\n",
                 _socket.remote_endpoint().address().to_string().c_str(), 
                 _socket.remote_endpoint().port(),
                 error.message().c_str());
              return;
            }

            log_message(_socket, Serverbound, std::vector<std::byte>(_buffer.begin(), _buffer.begin()+size));


            // Respond with AcCmdCLLoginCancel with response code
            // CR_INVALID_VERSION
            std::array<std::byte, 5> response;
            *reinterpret_cast<uint32_t*>(response.data()) = encode_message_information(9, 16384, 5);
            response[4] = static_cast<std::byte>(3);
            _socket.write_some(boost::asio::const_buffer(response.data(), 5));
            log_message(_socket, Clientbound, std::vector<std::byte>(response.begin(), response.end()));

            read_loop();
          });
    }
  private:
    asio::ip::tcp::socket _socket;
    std::array<std::byte, 4092> _buffer;
  };

  /**
   * Server
   */
  class Server
  {
  public:
    Server() : _acceptor(_io_ctx)
    {}

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

    void run()
    {
      _io_ctx.run();
    }

  private:
    void accept_loop()
    {
      _acceptor.async_accept(
        [&](boost::system::error_code error, asio::ip::tcp::socket client_socket)
         {
           printf("Accepted new client from port %d\n", client_socket.remote_endpoint().port());
           const auto [itr, _] = _clients.emplace(
             client_id++,
             std::move(client_socket));
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
}

int main()
{
  alicia::protocol::Server server;
  server.host();
  server.run();
  return 0;
}
