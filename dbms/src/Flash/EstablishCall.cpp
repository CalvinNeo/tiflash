#include <Flash/EstablishCall.h>
#include <Flash/FlashService.h>
#include <Flash/Mpp/Utils.h>

namespace DB
{
EstablishCallData::EstablishCallData(AsyncFlashService * service, grpc::ServerCompletionQueue * cq, grpc::ServerCompletionQueue * notify_cq, const std::shared_ptr<std::atomic<bool>> & is_shutdown)
    : service(service)
    , cq(cq)
    , notify_cq(notify_cq)
    , is_shutdown(is_shutdown)
    , responder(&ctx)
    , state(NEW_REQUEST)
{
    // As part of the initial CREATE state, we *request* that the system
    // start processing requests. In this request, "this" acts are
    // the tag uniquely identifying the request.
    service->RequestEstablishMPPConnection(&ctx, &request, &responder, cq, notify_cq, this);
}

EstablishCallData * EstablishCallData::spawn(AsyncFlashService * service, grpc::ServerCompletionQueue * cq, grpc::ServerCompletionQueue * notify_cq, const std::shared_ptr<std::atomic<bool>> & is_shutdown)
{
    return new EstablishCallData(service, cq, notify_cq, is_shutdown);
}

void EstablishCallData::tryFlushOne()
{
    // check whether there is a valid msg to write
    {
        std::unique_lock lk(mu);
        if (ready && mpp_tunnel->isSendQueueNextPopNonBlocking()) //not ready or no packet
            ready = false;
        else
            return;
    }
    // there is a valid msg, do single write operation
    mpp_tunnel->sendJob(false);
}

void EstablishCallData::responderFinish(const grpc::Status & status)
{
    if (!(*is_shutdown))
        responder.Finish(status, this);
}

void EstablishCallData::initRpc()
{
    std::exception_ptr eptr = nullptr;
    try
    {
        service->EstablishMPPConnectionSyncOrAsync(&ctx, &request, nullptr, this);
    }
    catch (...)
    {
        eptr = std::current_exception();
    }
    if (eptr)
    {
        state = FINISH;
        grpc::Status status(static_cast<grpc::StatusCode>(GRPC_STATUS_UNKNOWN), getExceptionMessage(eptr, false));
        responderFinish(status);
    }
}

bool EstablishCallData::write(const mpp::MPPDataPacket & packet)
{
    if (*is_shutdown)
        return false;
    responder.Write(packet, this);
    return true;
}

void EstablishCallData::writeErr(const mpp::MPPDataPacket & packet)
{
    state = ERR_HANDLE;
    if (write(packet))
        err_status = grpc::Status::OK;
    else
        err_status = grpc::Status(grpc::StatusCode::UNKNOWN, "Write error message failed for unknown reason.");
}

void EstablishCallData::writeDone(const ::grpc::Status & status)
{
    state = FINISH;
    if (stopwatch)
    {
        LOG_FMT_INFO(mpp_tunnel->getLogger(), "connection for {} cost {} ms.", mpp_tunnel->id(), stopwatch->elapsedMilliseconds());
    }
    responderFinish(status);
}

void EstablishCallData::notifyReady()
{
    std::unique_lock lk(mu);
    ready = true;
}

void EstablishCallData::cancel()
{
    if (state == NEW_REQUEST || state == FINISH) // state == NEW_REQUEST means the server is shutdown and no new rpc has come.
    {
        delete this;
        return;
    }
    state = FINISH;
    if (mpp_tunnel)
        mpp_tunnel->consumerFinish("grpc writes failed.", true); //trigger mpp tunnel finish work
    grpc::Status status(static_cast<grpc::StatusCode>(GRPC_STATUS_UNKNOWN), "Consumer exits unexpected, grpc writes failed.");
    responderFinish(status);
}

void EstablishCallData::proceed()
{
    if (state == NEW_REQUEST)
    {
        state = PROCESSING;

        spawn(service, cq, notify_cq, is_shutdown);
        notifyReady();
        initRpc();
    }
    else if (state == PROCESSING)
    {
        std::unique_lock lk(mu);
        if (mpp_tunnel->isSendQueueNextPopNonBlocking())
        {
            ready = false;
            lk.unlock();
            mpp_tunnel->sendJob(true);
        }
        else
            ready = true;
    }
    else if (state == ERR_HANDLE)
    {
        state = FINISH;
        writeDone(err_status);
    }
    else
    {
        assert(state == FINISH);
        // Once in the FINISH state, deallocate ourselves (EstablishCallData).
        // That't the way GRPC official examples do. link: https://github.com/grpc/grpc/blob/master/examples/cpp/helloworld/greeter_async_server.cc
        delete this;
        return;
    }
}

void EstablishCallData::attachTunnel(const std::shared_ptr<DB::MPPTunnel> & mpp_tunnel_)
{
    stopwatch = std::make_shared<Stopwatch>();
    this->mpp_tunnel = mpp_tunnel_;
}
} // namespace DB
