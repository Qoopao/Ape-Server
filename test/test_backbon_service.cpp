#include "services/backbon_service/server.h"
#include "services/backbon_service/client.h"
#include <ctime>

void testBackbonServiceStartup();


void testBackbonServiceServer(){
    BackbonServiceImpl server("backbon_service", "127.0.0.1:50309");
    server.Start();
}

void testBackbonServiceClient(){
    BackbonClient client(
      grpc::CreateChannel("127.0.0.1:50309", grpc::InsecureChannelCredentials()));
    
    for(int i = 0; i < 3; i++){
        client.RegisterService({"backbon_service", {"127.0.0.1:50001", "127.0.0.1:50002"}, {"CheckUserOnline", "RegisterService", "UnregisterService"}});
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
