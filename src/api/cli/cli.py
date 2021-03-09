import grpc

import mme_pb2
import mme_pb2_grpc

if __name__ == '__main__':
    channel = grpc.insecure_channel('localhost:50051')
    stub = mme_pb2_grpc.MMEAPIStub(channel)
    request = mme_pb2.GetMMERequest()
    response = stub.GetMME(request)
    print '===== STATISTICS ====='
    print 'Connected eNBs:', response.mme.nb_enb_connected
    print 'Attached UEs:', response.mme.nb_ue_attached
    print 'Connected UEs:', response.mme.nb_ue_connected
    print 'Default Bearers:', response.mme.nb_default_eps_bearers
    print 'S1-U Bearers:', response.mme.nb_s1u_bearers
