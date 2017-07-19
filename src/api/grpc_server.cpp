#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include "proto/mme.pb.h"
#include "proto/mme.grpc.pb.h"

#include "intertask_interface.h"

#include "mme_app_defs.h"

#include "grpc_server.h"

class GRPCServer final : public MMEAPI::Service {
    public:
        ::grpc::Status GetMME(::grpc::ServerContext* context, const ::GetMMERequest* request, ::GetMMEResponse* response) override {
            response->mutable_mme()->set_name(request->mme().name());

            mme_stats_read_lock(&mme_app_desc);

            response->mutable_mme()->set_nb_enb_connected(mme_app_desc.nb_enb_connected);
            response->mutable_mme()->set_nb_ue_attached(mme_app_desc.nb_ue_attached);
            response->mutable_mme()->set_nb_ue_connected(mme_app_desc.nb_ue_connected);
            response->mutable_mme()->set_nb_default_eps_bearers(mme_app_desc.nb_default_eps_bearers);
            response->mutable_mme()->set_nb_s1u_bearers(mme_app_desc.nb_s1u_bearers);

            mme_stats_unlock(&mme_app_desc);
            return ::grpc::Status::OK;
        }
};

static void* grpc_intertask_interface(void *args_p){
  itti_mark_task_ready(TASK_GRPC);

  std::string server_address("0.0.0.0:50051");
  GRPCServer service;

  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

  OAILOG_INFO(LOG_GRPC, "GRPC API server is ready!\n");

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();

}

int grpc_server_init(void) {
  itti_create_task(TASK_GRPC, &grpc_intertask_interface, NULL);
  return 0;
}
