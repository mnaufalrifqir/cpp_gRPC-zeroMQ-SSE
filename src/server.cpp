#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <grpcpp/grpcpp.h>
#include "helloworld.grpc.pb.h"
#include <boost/asio.hpp>

using namespace boost::asio;
using namespace boost::asio::ip;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;
using ip::tcp;

zmq::context_t context{1};
zmq::socket_t publisher{context, zmq::socket_type::pub};
zmq::socket_t subscriber{context, zmq::socket_type::sub};
std::string message;

class GreeterServiceImpl final : public Greeter::Service
{
public:
    Status SayHello(ServerContext *context, const HelloRequest *request, HelloReply *reply) override
    {
        std::string prefix("Hello, ");
        reply->set_message(prefix + request->name());
        publisher.send(zmq::str_buffer("Hello"), zmq::send_flags::sndmore);
        publisher.send(zmq::buffer(request->name()));
        std::cout << "Sent: " << request->name() << std::endl;
        return Status::OK;
    }
};

void RunPublisher()
{
    publisher.bind("tcp://localhost:5555");
}

// void RunSubscriber()
// {
//     subscriber.connect("tcp://localhost:5555");
//     subscriber.set(zmq::sockopt::subscribe, "Hello");

//     if (subscriber.connected())
//     {
//         std::cout << "Subscriber listen!" << std::endl;
//     }

//     while (true)
//     {
//         std::vector<zmq::message_t> recv_msgs;
//         zmq::recv_result_t result = zmq::recv_multipart(subscriber, std::back_inserter(recv_msgs));
//         assert(result && "recv failed");
//         assert(*result == 2);
//         message = "[" + recv_msgs[0].to_string() + "] " + recv_msgs[1].to_string();
//         std::cout << "Subscriber: " << message << std::endl;
//         socket.write_some(buffer(message));
//         std::this_thread::sleep_for(std::chrono::microseconds(100));
//     }
// }

void RunSSESubscriber()
{
    io_service io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 8080));
    std::cout << "SSE server is running on http://localhost:8080" << std::endl;
    subscriber.connect("tcp://localhost:5555");
    subscriber.set(zmq::sockopt::subscribe, "Hello");

    if (subscriber.connected())
    {
        std::cout << "Subscriber listen!" << std::endl;
    }

    tcp::socket socket(io);
    acceptor.accept(socket);

    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/event-stream\r\n";
    response += "Cache-Control: no-cache\r\n";
    response += "Connection: keep-alive\r\n";
    response += "\r\n";

    socket.write_some(buffer(response));

    while (true)
    {
        std::vector<zmq::message_t> recv_msgs;
        zmq::recv_result_t result = zmq::recv_multipart(subscriber, std::back_inserter(recv_msgs));
        assert(result && "recv failed");
        assert(*result == 2);
        message = "data: [" + recv_msgs[0].to_string() + "] " + recv_msgs[1].to_string() + "\r\n\r\n";
        socket.write_some(buffer(message));
        std::cout << "Subscriber: " << message << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void RunServer()
{
    std::cout << "hello world" << std::endl;

    try
    {
        std::string server_address("localhost:5000");
        GreeterServiceImpl service;

        ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);

        std::unique_ptr<Server> server(builder.BuildAndStart());
        std::cout << "Server listening on " << server_address << std::endl;

        std::thread publisherThread(RunPublisher);
        std::thread sseServerThread(RunSSESubscriber); // Add SSE server thread

        server->Wait();
        publisherThread.join();
        sseServerThread.join(); 
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception caught: " << e.what() << std::endl;
    }
}

int main()
{
    RunServer();
    return 0;
}
