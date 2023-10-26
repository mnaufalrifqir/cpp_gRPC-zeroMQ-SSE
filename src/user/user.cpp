#include <iostream>
#include <string>
#include <unordered_map>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "user.pb.h"
#include "user.grpc.pb.h"
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <sstream>
#include "rapidjson/document.h"

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;

using google::protobuf::Empty;
using google::protobuf::Int64Value;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using ip::tcp;
using user::User;
using user::UserService;

zmq::context_t context{1};
zmq::socket_t publisher{context, zmq::socket_type::pub};
zmq::socket_t subscriber{context, zmq::socket_type::sub};
std::string message;

pqxx::connection c("postgresql://postgres:postgres@localhost:5432/cpp-grpc-crud-2");

class UserServiceImpl final : public UserService::Service
{
public:
    Status CreateUser(ServerContext *context, const User *request, User *response) override
    {

        User newUser;
        newUser.set_name(request->name());

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        std::tm tm_struct = *std::localtime(&now);
        std::stringstream bufferstr;
        bufferstr << std::put_time(&tm_struct, "%Y-%m-%d %H:%M:%S");
        std::string buffer = bufferstr.str();
        newUser.set_created_at(buffer);
        newUser.set_updated_at(buffer);

        pqxx::work txn(c);

        txn.exec("INSERT INTO users (name, created_at, updated_at) VALUES ( '" + newUser.name() + "', NOW(), NOW())");

        User getUser;

        pqxx::result res = txn.exec("SELECT * FROM users ORDER BY id DESC LIMIT 1");

        txn.commit();

        for (auto row : res)
        {
            nlohmann::json json_data = {
                {"id", row["id"].as<int64_t>()},
                {"name", row["name"].c_str()},
                {"created_at", row["created_at"].c_str()},
                {"updated_at", row["updated_at"].c_str()}};

            getUser.set_id(row["id"].as<int64_t>());
            getUser.set_name(row["name"].c_str());
            getUser.set_created_at(row["created_at"].c_str());
            getUser.set_updated_at(row["updated_at"].c_str());

            users_[row["id"].as<int64_t>()] = newUser;

            std::string json_response = json_data.dump();

            publisher.send(zmq::str_buffer("Hello"), zmq::send_flags::sndmore);
            publisher.send(zmq::buffer(json_response));
        }

        *response = getUser;

        return Status::OK;
    }

    Status ReadUser(ServerContext *context, const Int64Value *request, User *response) override
    {
        int64_t userId = request->value();

        pqxx::work txn(c);

        pqxx::result res = txn.exec("SELECT * FROM users WHERE id = " + std::to_string(userId));

        txn.commit();

        if (res.empty())
        {
            return Status(grpc::StatusCode::NOT_FOUND, "User not found");
        }

        User user;

        for (auto row : res)
        {
            nlohmann::json json_data = {
                {"id", row["id"].as<int64_t>()},
                {"name", row["name"].c_str()},
                {"created_at", row["created_at"].c_str()},
                {"updated_at", row["updated_at"].c_str()}};

            user.set_id(row["id"].as<int64_t>());
            user.set_name(row["name"].c_str());
            user.set_created_at(row["created_at"].c_str());
            user.set_updated_at(row["updated_at"].c_str());

            users_[row["id"].as<int64_t>()] = user;

            std::string json_response = json_data.dump();

            publisher.send(zmq::str_buffer("Hello"), zmq::send_flags::sndmore);
            publisher.send(zmq::buffer(json_response));
        }

        *response = user;

        return Status::OK;
    }

    Status UpdateUser(ServerContext *context, const User *request, User *response) override
    {
        int64_t userId = request->id();

        pqxx::work txn(c);

        pqxx::result res = txn.exec("SELECT * FROM users WHERE id = " + std::to_string(userId));

        if (res.empty())
        {
            return Status(grpc::StatusCode::NOT_FOUND, "User not found");
        }

        txn.exec("INSERT INTO users (name, updated_at) VALUES ( '" + request->name() + "', NOW()) WHERE id = " + std::to_string(userId) + ")");

        txn.commit();

        return Status::OK;
    }

    Status DeleteUser(ServerContext *context, const Int64Value *request, Empty *response) override
    {
        int64_t userId = request->value();

        pqxx::work txn(c);

        txn.exec("DELETE FROM users WHERE id = " + std::to_string(userId));

        txn.commit();

        return Status::OK;
    }

    Status ListUsers(ServerContext *context, const Empty *request, grpc::ServerWriter<User> *writer) override
    {
        for (const auto &pair : users_)
        {
            writer->Write(pair.second);
        }
        return Status::OK;
    }

private:
    int64_t GenerateNewUserId()
    {
        static int64_t nextId = 1;
        return nextId++;
    }

    std::unordered_map<int64_t, User> users_;
};

void StartDB()
{
    pqxx::work txn{c};

    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS users (
            id SERIAL PRIMARY KEY,
            name VARCHAR(50) NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT NOW(),
            updated_at TIMESTAMP NOT NULL DEFAULT NOW()
        )
    )");

    cout << "Table created successfully" << endl;

    txn.commit();
}

void RunPublisher()
{
    publisher.bind("tcp://localhost:5003");
}

void RunSSESubscriber()
{
    io_service io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 5051));
    std::cout << "SSE server is running on http://localhost:5051" << std::endl;
    subscriber.connect("tcp://localhost:5053");
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
        rapidjson::Document data;
        std::string recvMessage = recv_msgs[1].to_string();
        data.Parse(recvMessage.c_str());
        User user;
        if (data.IsObject())
        {
            if (data.HasMember("id") && data["id"].IsInt64())
            {
                user.set_id(data["id"].GetInt64());
            }

            if (data.HasMember("name") && data["name"].IsString())
            {
                user.set_name(data["name"].GetString());
            }

            if (data.HasMember("created_at") && data["created_at"].IsString())
            {
                user.set_created_at(data["created_at"].GetString());
            }

            if (data.HasMember("updated_at") && data["updated_at"].IsString())
            {
                user.set_updated_at(data["updated_at"].GetString());
            }
        }

        pqxx::work txn(c);

        txn.exec("INSERT INTO users (id, name, created_at, updated_at) VALUES (" + std::to_string(user.id()) + ", '" + user.name() + "', '" + user.created_at() + "', '" + user.updated_at() + "')");
        // txn.exec("INSERT INTO users (name, created_at, updated_at) VALUES ('" + user.name() + "', '" + user.created_at() + "', '" + user.updated_at() + "')");

        txn.commit();
        message = "data: " + recvMessage + "\r\n\r\n";
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
        std::string server_address("localhost:5050");
        UserServiceImpl service;

        ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);

        std::unique_ptr<Server> server(builder.BuildAndStart());
        std::cout << "Server listening on " << server_address << std::endl;

        StartDB();
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

int main(int argc, char **argv)
{
    RunServer();
    return 0;
}
